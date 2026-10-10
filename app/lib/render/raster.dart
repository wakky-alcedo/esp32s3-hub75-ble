import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart';
import 'package:image/image.dart' as img;

/// LDB1 pixel formats (docs/protocol.md).
class LdbFormat {
  static const rgb565 = 0;
  static const rgb565a8 = 1;
  static const a8 = 2;
}

/// Straight (non-premultiplied) RGBA pixels.
class Rgba {
  Rgba(this.width, this.height, this.pixels);
  final int width, height;
  final Uint8List pixels;
}

Uint8List encodeLdb(Rgba src, int format) {
  final n = src.width * src.height;
  final bpp = const [2, 3, 1][format];
  final out = Uint8List(12 + n * bpp);
  out.setAll(0, 'LDB1'.codeUnits);
  final bd = ByteData.view(out.buffer);
  bd.setUint16(4, src.width, Endian.little);
  bd.setUint16(6, src.height, Endian.little);
  out[8] = format;
  final px = src.pixels;
  var o = 12;
  for (var i = 0; i < n; i++) {
    final r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2], a = px[i * 4 + 3];
    if (format == LdbFormat.a8) {
      out[o++] = a;
      continue;
    }
    // Opaque formats get alpha pre-applied over black.
    final rr = format == LdbFormat.rgb565 ? r * a ~/ 255 : r;
    final gg = format == LdbFormat.rgb565 ? g * a ~/ 255 : g;
    final bb = format == LdbFormat.rgb565 ? b * a ~/ 255 : b;
    final c = ((rr & 0xF8) << 8) | ((gg & 0xFC) << 3) | (bb >> 3);
    out[o++] = c & 0xFF;
    out[o++] = c >> 8;
    if (format == LdbFormat.rgb565a8) out[o++] = a;
  }
  return out;
}

String _fontFamily(String font) {
  if (Platform.isWindows) {
    return switch (font) { 'serif' => 'Yu Mincho', 'mono' => 'MS Gothic', _ => 'Yu Gothic UI' };
  }
  return switch (font) { 'serif' => 'serif', 'mono' => 'monospace', _ => 'sans-serif' };
}

/// Renders [text] with the OS fonts at LED resolution.
///
/// colorMode: solid | gradient | perChar | rainbow. For rainbow the glyphs are
/// rendered white and sent as an alpha mask the firmware colors on the fly.
Future<Rgba> renderText({
  required String text,
  required int heightPx,
  required bool bold,
  required String font,
  required String colorMode,
  required Color color,
  required Color color2,
  List<int> charColors = const [],
  bool crisp = true,
}) async {
  final family = _fontFamily(font);
  final base = TextStyle(
    fontSize: heightPx * 0.95,
    height: 1.0,
    fontFamily: family,
    fontFamilyFallback: const ['Noto Sans CJK JP', 'Noto Sans JP', 'Yu Gothic UI', 'Meiryo', 'sans-serif'],
    fontWeight: bold ? FontWeight.w800 : FontWeight.w400,
    color: colorMode == 'rainbow' ? const Color(0xFFFFFFFF) : color,
  );
  final InlineSpan span;
  if (colorMode == 'perChar') {
    final chars = text.characters.toList();
    span = TextSpan(
      style: base,
      children: [
        for (var i = 0; i < chars.length; i++)
          TextSpan(text: chars[i], style: TextStyle(color: i < charColors.length ? Color(charColors[i]) : color)),
      ],
    );
  } else {
    span = TextSpan(text: text, style: base);
  }
  final tp = TextPainter(text: span, textDirection: TextDirection.ltr, maxLines: 1)..layout();
  final w = tp.width.ceil().clamp(1, 8000);
  final h = heightPx;

  final rec = ui.PictureRecorder();
  final canvas = Canvas(rec);
  final dy = (h - tp.height) / 2;
  if (colorMode == 'gradient') {
    // Draw the glyphs as a mask, then fill them with a horizontal gradient.
    canvas.saveLayer(Rect.fromLTWH(0, 0, w.toDouble(), h.toDouble()), Paint());
    tp.paint(canvas, Offset(0, dy));
    canvas.drawRect(
      Rect.fromLTWH(0, 0, w.toDouble(), h.toDouble()),
      Paint()
        ..blendMode = BlendMode.srcIn
        ..shader = ui.Gradient.linear(Offset.zero, Offset(w.toDouble(), 0), [color, color2]),
    );
    canvas.restore();
  } else {
    tp.paint(canvas, Offset(0, dy));
  }
  final image = await rec.endRecording().toImage(w, h);
  final data = await image.toByteData(format: ui.ImageByteFormat.rawStraightRgba);
  image.dispose();
  final px = data!.buffer.asUint8List();
  if (crisp) {
    // LEDs are big pixels; hard edges read better than anti-aliased blur.
    for (var i = 3; i < px.length; i += 4) {
      px[i] = px[i] >= 110 ? 255 : 0;
    }
  }
  return Rgba(w, h, px);
}

// ---------------------------------------------------------------------------
// Image / GIF conversion (runs in a background isolate).

class ConvertRequest {
  ConvertRequest(this.bytes, this.width, this.height, this.fit);
  final Uint8List bytes;
  final int width, height;
  final String fit; // contain | cover | stretch
}

img.Image _fitImage(img.Image src, int w, int h, String fit) {
  switch (fit) {
    case 'stretch':
      return img.copyResize(src, width: w, height: h, interpolation: img.Interpolation.average);
    case 'cover':
      final scale = [w / src.width, h / src.height].reduce((a, b) => a > b ? a : b);
      final rw = (src.width * scale).ceil(), rh = (src.height * scale).ceil();
      final resized = img.copyResize(src, width: rw, height: rh, interpolation: img.Interpolation.average);
      return img.copyCrop(resized, x: (rw - w) ~/ 2, y: (rh - h) ~/ 2, width: w, height: h);
    default:
      final scale = [w / src.width, h / src.height].reduce((a, b) => a < b ? a : b);
      return img.copyResize(src,
          width: (src.width * scale).round().clamp(1, w),
          height: (src.height * scale).round().clamp(1, h),
          interpolation: img.Interpolation.average);
  }
}

Rgba _toRgba(img.Image im) {
  final rgba = im.convert(format: img.Format.uint8, numChannels: 4, alpha: 255);
  return Rgba(rgba.width, rgba.height, Uint8List.fromList(rgba.getBytes(order: img.ChannelOrder.rgba)));
}

Rgba _convertStill(ConvertRequest r) {
  final src = img.decodeImage(r.bytes);
  if (src == null) throw const FormatException('画像を読み込めません');
  return _toRgba(_fitImage(src, r.width, r.height, r.fit));
}

Uint8List _convertGif(ConvertRequest r) {
  final src = img.decodeGif(r.bytes);
  if (src == null) throw const FormatException('GIFを読み込めません');
  final fitted = _fitImage(src, r.width, r.height, r.fit);
  return img.encodeGif(fitted);
}

/// Decodes PNG/JPG/etc. and fits it to the panel.
Future<Rgba> convertImage(ConvertRequest r) => compute(_convertStill, r);

/// Resizes every frame of an animated GIF and re-encodes it.
Future<Uint8List> convertGif(ConvertRequest r) => compute(_convertGif, r);

/// First frame of the converted GIF, for previews.
Future<Rgba> gifFirstFrame(Uint8List gif) => compute((Uint8List b) => _toRgba(img.decodeGif(b)!.frames.first), gif);

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import '../render/raster.dart';

/// Shows a bitmap the way the panel will: one dot per LED, optionally
/// scrolling like the firmware does.
class LedPreview extends StatefulWidget {
  const LedPreview({
    super.key,
    required this.width,
    required this.height,
    this.bitmap,
    this.scrollDir,
    this.speed = 30,
    this.rainbow = false,
    this.background = Colors.black,
  });

  final int width, height;
  final Rgba? bitmap;
  final String? scrollDir; // left | right | up | down, null = static centered
  final int speed; // px/s
  final bool rainbow;
  final Color background;

  @override
  State<LedPreview> createState() => _LedPreviewState();
}

class _LedPreviewState extends State<LedPreview> with SingleTickerProviderStateMixin {
  late final Ticker _ticker;
  Duration _elapsed = Duration.zero;

  @override
  void initState() {
    super.initState();
    _ticker = createTicker((d) => setState(() => _elapsed = d))..start();
  }

  @override
  void dispose() {
    _ticker.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AspectRatio(
      aspectRatio: widget.width / widget.height,
      child: Container(
        decoration: BoxDecoration(color: const Color(0xFF101010), borderRadius: BorderRadius.circular(4)),
        padding: const EdgeInsets.all(4),
        child: CustomPaint(
          painter: _LedPainter(widget, _elapsed.inMilliseconds),
          size: Size.infinite,
        ),
      ),
    );
  }
}

class _LedPainter extends CustomPainter {
  _LedPainter(this.w, this.ms);
  final LedPreview w;
  final int ms;

  @override
  void paint(Canvas canvas, Size size) {
    final cell = size.width / w.width;
    final dot = cell * 0.82;
    final paint = Paint();
    final bmp = w.bitmap;
    var ox = 0, oy = 0;
    if (bmp != null) {
      ox = (w.width - bmp.width) ~/ 2;
      oy = (w.height - bmp.height) ~/ 2;
      final spanX = w.width + bmp.width, spanY = w.height + bmp.height;
      final p = (ms * w.speed / 1000).floor();
      switch (w.scrollDir) {
        case 'left':
          ox = w.width - p % spanX;
        case 'right':
          ox = -bmp.width + p % spanX;
        case 'up':
          oy = w.height - p % spanY;
        case 'down':
          oy = -bmp.height + p % spanY;
      }
    }
    final hueShift = ms / 10 % 256;
    for (var y = 0; y < w.height; y++) {
      for (var x = 0; x < w.width; x++) {
        var c = w.background;
        if (bmp != null) {
          final sx = x - ox, sy = y - oy;
          if (sx >= 0 && sy >= 0 && sx < bmp.width && sy < bmp.height) {
            final i = (sy * bmp.width + sx) * 4;
            final a = bmp.pixels[i + 3];
            if (a > 0) {
              final fg = w.rainbow
                  ? HSVColor.fromAHSV(1, ((hueShift + sx * 4) % 256) / 256 * 360, 1, 1).toColor()
                  : Color.fromARGB(255, bmp.pixels[i], bmp.pixels[i + 1], bmp.pixels[i + 2]);
              c = Color.lerp(c, fg, a / 255)!;
            }
          }
        }
        if (c.toARGB32() & 0xFFFFFF == 0) c = const Color(0xFF1C1C1C); // unlit LED
        paint.color = c;
        canvas.drawCircle(Offset((x + 0.5) * cell, (y + 0.5) * cell), dot / 2, paint);
      }
    }
  }

  @override
  bool shouldRepaint(_LedPainter old) => true;
}

const swatches = [
  0xFFFFFFFF, 0xFFFF2020, 0xFFFF8000, 0xFFFFE000, 0xFF40FF40, 0xFF00E0FF, //
  0xFF2060FF, 0xFFA040FF, 0xFFFF40C0, 0xFF000000,
];

/// Compact color chooser: preset swatches plus a hue slider.
class ColorField extends StatelessWidget {
  const ColorField({super.key, required this.label, required this.value, required this.onChanged});
  final String label;
  final int value;
  final ValueChanged<int> onChanged;

  @override
  Widget build(BuildContext context) {
    return ListTile(
      contentPadding: EdgeInsets.zero,
      title: Text(label),
      trailing: GestureDetector(
        onTap: () async {
          final v = await showDialog<int>(context: context, builder: (_) => _ColorDialog(initial: value));
          if (v != null) onChanged(v);
        },
        child: Container(
          width: 44,
          height: 28,
          decoration: BoxDecoration(
            color: Color(value),
            border: Border.all(color: Colors.white54),
            borderRadius: BorderRadius.circular(6),
          ),
        ),
      ),
    );
  }
}

class _ColorDialog extends StatefulWidget {
  const _ColorDialog({required this.initial});
  final int initial;
  @override
  State<_ColorDialog> createState() => _ColorDialogState();
}

class _ColorDialogState extends State<_ColorDialog> {
  late HSVColor hsv = HSVColor.fromColor(Color(widget.initial));

  @override
  Widget build(BuildContext context) {
    final color = hsv.toColor();
    return AlertDialog(
      title: const Text('色'),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Wrap(spacing: 8, runSpacing: 8, children: [
            for (final s in swatches)
              GestureDetector(
                onTap: () => setState(() => hsv = HSVColor.fromColor(Color(s))),
                child: Container(
                  width: 36,
                  height: 36,
                  decoration: BoxDecoration(
                    color: Color(s),
                    shape: BoxShape.circle,
                    border: Border.all(color: Colors.white38),
                  ),
                ),
              ),
          ]),
          const SizedBox(height: 16),
          Row(children: [
            const Text('色相'),
            Expanded(
              child: Slider(value: hsv.hue, max: 360, onChanged: (v) => setState(() => hsv = hsv.withHue(v))),
            ),
          ]),
          Row(children: [
            const Text('彩度'),
            Expanded(
              child: Slider(value: hsv.saturation, onChanged: (v) => setState(() => hsv = hsv.withSaturation(v))),
            ),
          ]),
          Row(children: [
            const Text('明度'),
            Expanded(child: Slider(value: hsv.value, onChanged: (v) => setState(() => hsv = hsv.withValue(v)))),
          ]),
          Container(height: 32, color: color),
        ],
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('キャンセル')),
        FilledButton(onPressed: () => Navigator.pop(context, color.toARGB32()), child: const Text('OK')),
      ],
    );
  }
}

/// Labeled integer slider.
class IntSlider extends StatelessWidget {
  const IntSlider({
    super.key,
    required this.label,
    required this.value,
    required this.min,
    required this.max,
    required this.onChanged,
    this.format,
    this.onChangeEnd,
  });
  final String label;
  final int value, min, max;
  final ValueChanged<int> onChanged;
  final ValueChanged<int>? onChangeEnd;
  final String Function(int)? format;

  @override
  Widget build(BuildContext context) {
    return Row(children: [
      SizedBox(width: 96, child: Text(label)),
      Expanded(
        child: Slider(
          value: value.clamp(min, max).toDouble(),
          min: min.toDouble(),
          max: max.toDouble(),
          divisions: (max - min).clamp(1, 1000),
          onChanged: (v) => onChanged(v.round()),
          onChangeEnd: onChangeEnd == null ? null : (v) => onChangeEnd!(v.round()),
        ),
      ),
      SizedBox(width: 64, child: Text(format?.call(value) ?? '$value', textAlign: TextAlign.end)),
    ]);
  }
}

/// Dropdown over a fixed set of string values.
class ChoiceField extends StatelessWidget {
  const ChoiceField({super.key, required this.label, required this.value, required this.options, required this.onChanged});
  final String label;
  final String value;
  final Map<String, String> options; // value -> label
  final ValueChanged<String> onChanged;

  @override
  Widget build(BuildContext context) {
    return Row(children: [
      SizedBox(width: 96, child: Text(label)),
      Expanded(
        child: DropdownButton<String>(
          isExpanded: true,
          value: options.containsKey(value) ? value : options.keys.first,
          items: [for (final e in options.entries) DropdownMenuItem(value: e.key, child: Text(e.value))],
          onChanged: (v) => v == null ? null : onChanged(v),
        ),
      ),
    ]);
  }
}

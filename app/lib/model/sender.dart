import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:archive/archive.dart' show getCrc32;
import 'package:flutter/widgets.dart';

import '../ble/link.dart';
import '../render/raster.dart';
import 'content.dart';

String hexColor(int argb) => '#${(argb & 0xFFFFFF).toRadixString(16).padLeft(6, '0')}';

/// Firmware item JSON plus the asset files it references.
class DeviceItem {
  DeviceItem(this.json, this.assets);
  final Map<String, dynamic> json;
  final Map<String, Uint8List> assets; // file name under /assets -> bytes
}

String _assetName(String prefix, Uint8List data, String ext) =>
    '${prefix}_${getCrc32(data).toRadixString(16).padLeft(8, '0')}.$ext';

Map<String, dynamic>? _bgJson(Map<String, dynamic>? bg) {
  if (bg == null) return null;
  if (bg['type'] == 'effect') {
    return {'type': 'effect', 'effect': bg['effect'] ?? 'stars', 'params': bg['params'] ?? {}};
  }
  return {'type': 'color', 'color': hexColor(bg['color'] as int? ?? 0xFF000000)};
}

Future<Uint8List?> _labelAsset(String label, int color, int panelH) async {
  if (label.isEmpty) return null;
  // Label takes up to ~40% of the height so the digits stay readable.
  final h = (panelH * 0.4).round().clamp(8, 24);
  final r = await renderText(
    text: label,
    heightPx: h,
    bold: true,
    font: 'sans',
    colorMode: 'solid',
    color: Color(color),
    color2: Color(color),
  );
  return encodeLdb(r, LdbFormat.rgb565a8);
}

/// Text item bitmap, also used by the editor preview.
Future<Rgba> renderTextItem(ContentItem item, int panelH) {
  final p = item.p;
  final size = (p['fontSize'] as int?) ?? 0;
  return renderText(
    text: (p['text'] as String).isEmpty ? ' ' : p['text'] as String,
    heightPx: size <= 0 ? panelH : size.clamp(6, panelH),
    bold: p['bold'] as bool? ?? true,
    font: p['font'] as String? ?? 'sans',
    colorMode: p['colorMode'] as String? ?? 'solid',
    color: Color(p['color'] as int),
    color2: Color(p['color2'] as int),
    charColors: List<int>.from(p['charColors'] as List? ?? const []),
    crisp: p['crisp'] as bool? ?? true,
  );
}

Future<DeviceItem> toDevice(ContentItem item, int w, int h) async {
  final p = item.p;
  final assets = <String, Uint8List>{};
  final json = <String, dynamic>{'type': item.type};
  final duration = p['duration'] as int? ?? 0;
  if (duration > 0) json['duration'] = duration;

  switch (item.type) {
    case 'text':
      final r = await renderTextItem(item, h);
      final rainbow = p['colorMode'] == 'rainbow';
      final data = encodeLdb(r, rainbow ? LdbFormat.a8 : LdbFormat.rgb565a8);
      final name = _assetName('t', data, 'ldb');
      assets[name] = data;
      json.addAll({
        'asset': name,
        'mode': p['mode'],
        'dir': p['dir'],
        'speed': p['speed'],
        'repeat': p['repeat'],
        if (rainbow) 'color': 'rainbow',
        'bg': _bgJson(p['bg'] as Map<String, dynamic>?),
      });
    case 'image':
      final bytes = await File(p['path'] as String).readAsBytes();
      final r = await convertImage(ConvertRequest(bytes, w, h, p['fit'] as String));
      final data = encodeLdb(r, LdbFormat.rgb565);
      final name = _assetName('i', data, 'ldb');
      assets[name] = data;
      json['asset'] = name;
    case 'gif':
      final bytes = await File(p['path'] as String).readAsBytes();
      final data = await convertGif(ConvertRequest(bytes, w, h, p['fit'] as String));
      final name = _assetName('g', data, 'gif');
      assets[name] = data;
      json.addAll({'asset': name, 'loops': p['loops']});
    case 'clock':
    case 'countdown':
      json['color'] = hexColor(p['color'] as int);
      json['bg'] = _bgJson(p['bg'] as Map<String, dynamic>?);
      final label = await _labelAsset(p['label'] as String? ?? '', p['labelColor'] as int, h);
      if (label != null) {
        final name = _assetName('l', label, 'ldb');
        assets[name] = label;
        json['label'] = name;
      }
      if (item.type == 'clock') {
        json['format'] = p['format'];
      } else {
        switch (p['mode']) {
          case 'timer':
            json['seconds'] = p['seconds'];
          case 'stopwatch':
            json['mode'] = 'stopwatch';
          default:
            json['target'] = (p['target'] as int) ~/ 1000;
        }
      }
    case 'effect':
      json['effect'] = p['effect'];
      json['params'] = p['params'] ?? {};
  }
  json.removeWhere((k, v) => v == null);
  return DeviceItem(json, assets);
}

/// Converts and uploads a preset to slot [index], then starts playing it.
/// Assets already on the device (same content hash) are skipped.
Future<void> sendPreset(
  DeviceLink link,
  int index,
  Preset preset, {
  required int width,
  required int height,
  required void Function(String step, double progress) onProgress,
}) async {
  onProgress('変換中', 0);
  final items = <Map<String, dynamic>>[];
  final assets = <String, Uint8List>{};
  for (final item in preset.items) {
    final d = await toDevice(item, width, height);
    items.add(d.json);
    assets.addAll(d.assets);
  }

  final existing = <String>{
    for (final f in (await link.command('list_files', {'dir': '/assets'}))['files'] as List) f['name'] as String,
  };
  final toSend = assets.entries.where((e) => !existing.contains(e.key)).toList();
  final total = toSend.fold<int>(0, (s, e) => s + e.value.length);
  var done = 0;
  for (final e in toSend) {
    final base = done;
    await link.uploadFile('/assets/${e.key}', e.value, onProgress: (f) {
      onProgress('転送中 ${e.key}', total == 0 ? 1 : (base + f * e.value.length) / total);
    });
    done += e.value.length;
  }

  onProgress('プリセット保存', 1);
  final presetJson = jsonEncode({'name': preset.name, 'transition': preset.transition, 'items': items});
  await link.uploadFile('/presets/$index.json', Uint8List.fromList(utf8.encode(presetJson)));
  await link.command('reload', {'index': index});
  await link.command('preset', {'index': index});
  await _collectGarbage(link);
}

/// Deletes assets no stored preset references.
Future<void> _collectGarbage(DeviceLink link) async {
  final info = (await link.command('info'))['info'] as Map<String, dynamic>;
  final used = <String>{};
  for (final i in info['presets'] as List) {
    final preset = (await link.command('get_preset', {'index': i}))['preset'] as Map<String, dynamic>;
    for (final item in preset['items'] as List) {
      for (final key in ['asset', 'label']) {
        if (item[key] is String) used.add(item[key] as String);
      }
      final bg = item['bg'];
      if (bg is Map && bg['asset'] is String) used.add(bg['asset'] as String);
    }
  }
  final files = (await link.command('list_files', {'dir': '/assets'}))['files'] as List;
  for (final f in files) {
    final name = f['name'] as String;
    if (!used.contains(name)) await link.command('delete_file', {'path': '/assets/$name'});
  }
}

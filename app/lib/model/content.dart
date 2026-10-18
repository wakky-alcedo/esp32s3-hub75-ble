import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';

const effectNames = ['plasma', 'fire', 'matrix', 'life', 'stars', 'noise', 'rainbow'];
const effectLabels = {
  'plasma': 'プラズマ',
  'fire': '炎',
  'matrix': 'マトリックス',
  'life': 'ライフゲーム',
  'stars': '星空',
  'noise': 'ノイズ',
  'rainbow': '虹',
};
const paletteNames = ['rainbow', 'heat', 'lava', 'cloud', 'ocean', 'forest', 'party'];

const endSoundLabels = {
  'none': 'なし',
  'beep': 'ピー',
  'chime': 'ピンポーン',
  'alarm': 'ピピピッ（アラーム）',
};

const itemTypeLabels = {
  'text': 'テキスト',
  'image': '画像',
  'gif': 'GIF',
  'clock': '時計',
  'countdown': 'タイマー',
  'effect': 'エフェクト',
};

/// One playlist entry as edited in the app. [p] holds the editable source
/// (text, colors, local file paths); [toDevice] in sender.dart turns it into
/// the firmware's item JSON plus assets.
class ContentItem {
  ContentItem(this.type, Map<String, dynamic> p) : p = {...defaultsFor(type), ...p};

  final String type;
  final Map<String, dynamic> p;

  static Map<String, dynamic> defaultsFor(String type) {
    switch (type) {
      case 'text':
        return {
          'text': 'いらっしゃいませ',
          'fontSize': 0, // 0 = panel height
          'bold': true,
          'font': 'sans',
          'colorMode': 'solid', // solid | rainbow | gradient | perChar
          'color': 0xFFFFFFFF,
          'color2': 0xFFFF4080,
          'charColors': <int>[],
          'crisp': true,
          'mode': 'scroll',
          'dir': 'left',
          'speed': 30,
          'repeat': 1,
          'duration': 0,
          'bg': {'type': 'color', 'color': 0xFF000000},
        };
      case 'image':
        return {'path': '', 'fit': 'contain', 'duration': 5000};
      case 'gif':
        return {'path': '', 'fit': 'contain', 'loops': 2, 'duration': 0};
      case 'clock':
        return {
          'format': 'HM',
          'color': 0xFF40C0FF,
          'label': '',
          'labelColor': 0xFFFFFFFF,
          'duration': 5000,
          'bg': {'type': 'color', 'color': 0xFF000000},
        };
      case 'countdown':
        return {
          'mode': 'target', // target | timer | stopwatch
          'target': DateTime.now().add(const Duration(hours: 1)).millisecondsSinceEpoch,
          'seconds': 300,
          'color': 0xFF00FF80,
          'label': '開演まで',
          'labelColor': 0xFFFFFFFF,
          'endSound': 'alarm', // none | beep | chime | alarm
          'tickFrom': 0, // beep every second from this many seconds left; 0 = off
          'duration': 10000,
          'bg': {'type': 'color', 'color': 0xFF000000},
        };
      case 'effect':
        return {'effect': 'plasma', 'params': <String, dynamic>{}, 'duration': 10000};
    }
    return {};
  }

  String get summary {
    switch (type) {
      case 'text':
        return p['text'] as String;
      case 'image':
      case 'gif':
        final path = p['path'] as String;
        return path.isEmpty ? '(未選択)' : path.split(RegExp(r'[\\/]')).last;
      case 'clock':
        return p['format'] == 'HMS' ? 'HH:MM:SS' : 'HH:MM';
      case 'countdown':
        final label = p['label'] as String;
        return switch (p['mode']) {
          'timer' => '${label.isEmpty ? '' : '$label '}${p['seconds']}秒タイマー',
          'stopwatch' => 'ストップウォッチ',
          _ => '${label.isEmpty ? '' : '$label '}${DateTime.fromMillisecondsSinceEpoch(p['target'] as int).toString().substring(5, 16)}',
        };
      case 'effect':
        return effectLabels[p['effect']] ?? p['effect'] as String;
    }
    return '';
  }

  ContentItem copy() => ContentItem(type, jsonDecode(jsonEncode(p)) as Map<String, dynamic>);

  Map<String, dynamic> toJson() => {'type': type, 'p': p};
  factory ContentItem.fromJson(Map<String, dynamic> j) =>
      ContentItem(j['type'] as String, Map<String, dynamic>.from(j['p'] as Map));
}

class Preset {
  Preset({this.name = '', this.transition = 'fade', List<ContentItem>? items}) : items = items ?? [];

  String name;
  String transition;
  List<ContentItem> items;

  Map<String, dynamic> toJson() => {
        'name': name,
        'transition': transition,
        'items': items.map((e) => e.toJson()).toList(),
      };
  factory Preset.fromJson(Map<String, dynamic> j) => Preset(
        name: j['name'] as String? ?? '',
        transition: j['transition'] as String? ?? 'fade',
        items: (j['items'] as List? ?? []).map((e) => ContentItem.fromJson(Map<String, dynamic>.from(e))).toList(),
      );
}

const presetSlots = 8;

/// Local copy of the 8 preset slots with their editable sources.
class Library extends ChangeNotifier {
  Library._();
  static final instance = Library._();

  final presets = List<Preset>.generate(presetSlots, (_) => Preset());
  late Directory _dir;

  Directory get mediaDir => Directory('${_dir.path}${Platform.pathSeparator}media');
  File get _file => File('${_dir.path}${Platform.pathSeparator}library.json');

  Future<void> load() async {
    _dir = await getApplicationSupportDirectory();
    await mediaDir.create(recursive: true);
    if (!await _file.exists()) return;
    try {
      final j = jsonDecode(await _file.readAsString()) as Map<String, dynamic>;
      final list = j['presets'] as List;
      for (var i = 0; i < presetSlots && i < list.length; i++) {
        presets[i] = Preset.fromJson(Map<String, dynamic>.from(list[i]));
      }
    } catch (e) {
      debugPrint('[library] load failed: $e');
    }
    notifyListeners();
  }

  Future<void> save() async {
    final tmp = File('${_file.path}.tmp');
    await tmp.writeAsString(jsonEncode({'presets': presets.map((p) => p.toJson()).toList()}));
    await tmp.rename(_file.path);
    notifyListeners();
  }

  /// Copies a picked file into app storage so the preset keeps working when
  /// the original is moved or deleted.
  Future<String> importMedia(String sourcePath) async {
    final src = File(sourcePath);
    final name = '${DateTime.now().millisecondsSinceEpoch}_${src.uri.pathSegments.last}';
    final dst = File('${mediaDir.path}${Platform.pathSeparator}$name');
    await src.copy(dst.path);
    return dst.path;
  }
}

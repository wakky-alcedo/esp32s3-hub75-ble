import 'dart:convert';

import '../ble/link.dart';

/// Where uploaded files go: 'auto' (the SD card when one is inserted, else
/// the internal flash), 'internal' or 'sd'.
const storageLabels = {
  'auto': '自動（SDカードがあればSDカード）',
  'internal': '内蔵メモリ',
  'sd': 'SDカード',
};

/// Device directories. Files on the SD card are addressed with "/sd/...".
const internalAssetDir = '/assets';
const sdAssetDir = '/sd/led/assets';
const internalMusicDir = '/music';
const sdMusicDir = '/sd/music';

bool sdMounted(Map<String, dynamic> info) => (info['sd'] as Map?)?['mounted'] == true;

/// Resolves the storage setting against the device: true = SD card.
/// Throws when the SD card is required but missing.
bool resolveUseSd(String pref, Map<String, dynamic> info) {
  final sd = sdMounted(info);
  if (pref == 'sd' && !sd) throw StateError('SDカードが挿さっていません');
  return pref == 'sd' || (pref == 'auto' && sd);
}

class DeviceFile {
  DeviceFile(this.path, this.size);
  final String path; // full device path
  final int size;
  String get name => path.substring(path.lastIndexOf('/') + 1);
  bool get onSd => path.startsWith('/sd/');
}

/// Lists the files in [dir]; an absent directory is empty.
Future<List<DeviceFile>> listDir(DeviceLink link, String dir) async {
  final r = await link.command('list_files', {'dir': dir});
  return [
    for (final f in r['files'] as List)
      if (f['dir'] != true) DeviceFile('$dir/${f['name']}', f['size'] as int),
  ];
}

const musicExtensions = ['mp3', 'wav'];

bool isMusicFile(String name) => musicExtensions.contains(name.split('.').last.toLowerCase());

/// Music on the device: /music, and on the SD card /sd/music plus any audio
/// files at the card's top level (copied there from a PC, for example).
Future<List<DeviceFile>> listMusic(DeviceLink link, Map<String, dynamic> info) async {
  final out = <DeviceFile>[...await listDir(link, internalMusicDir)];
  if (sdMounted(info)) {
    out.addAll(await listDir(link, sdMusicDir));
    out.addAll(await listDir(link, '/sd'));
  }
  return out.where((f) => isMusicFile(f.name)).toList();
}

/// File name for the device: characters FAT cannot store replaced, and cut
/// (keeping the extension) so the whole path fits the file system's limit.
String deviceFileName(String original, {required bool sd}) {
  final dir = sd ? sdMusicDir : internalMusicDir;
  final maxPath = sd ? 150 : 63; // LittleFS allows 63 bytes per path
  var name = original.replaceAll(RegExp(r'[\\/:*?"<>|\x00-\x1f]'), '_').trim();
  final dot = name.lastIndexOf('.');
  final ext = dot > 0 ? name.substring(dot) : '';
  var base = dot > 0 ? name.substring(0, dot) : name;
  final budget = maxPath - utf8.encode('$dir/$ext').length;
  while (base.isNotEmpty && utf8.encode(base).length > budget) {
    base = String.fromCharCodes(base.runes.take(base.runes.length - 1));
  }
  if (base.isEmpty) base = 'music_${DateTime.now().millisecondsSinceEpoch}';
  return '$base$ext';
}

String formatBytes(num bytes) {
  if (bytes >= 1e9) return '${(bytes / 1e9).toStringAsFixed(1)}GB';
  if (bytes >= 1e6) return '${(bytes / 1e6).toStringAsFixed(1)}MB';
  if (bytes >= 1e3) return '${(bytes / 1e3).round()}KB';
  return '${bytes}B';
}

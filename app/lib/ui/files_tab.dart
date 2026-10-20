import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../model/storage.dart';
import '../state/app_state.dart';
import 'widgets.dart';

/// Storage on the device: capacities, where uploads go, and the music files.
class FilesTab extends StatefulWidget {
  const FilesTab({super.key});
  @override
  State<FilesTab> createState() => _FilesTabState();
}

class _FilesTabState extends State<FilesTab> {
  final app = AppState.instance;
  List<DeviceFile>? music;
  List<DeviceFile> internalAssets = [], sdAssets = [];
  bool loading = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final link = app.link;
    if (link == null || loading) return;
    setState(() => loading = true);
    try {
      await app.refresh();
      final m = await listMusic(link, app.info);
      final ia = await listDir(link, internalAssetDir);
      final sa = sdMounted(app.info) ? await listDir(link, sdAssetDir) : <DeviceFile>[];
      if (!mounted) return;
      setState(() {
        music = m;
        internalAssets = ia;
        sdAssets = sa;
      });
    } catch (e) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('読み込めません: $e')));
    } finally {
      if (mounted) setState(() => loading = false);
    }
  }

  Future<void> _remount() async {
    final r = await app.run('sd_mount');
    if (!mounted) return;
    if (r != null && (r['sd'] as Map?)?['mounted'] != true) {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('SDカードが見つかりません')));
    }
    await _load();
  }

  Future<void> _addMusic() async {
    final link = app.link;
    if (link == null) return;
    final bool toSd;
    try {
      await app.refresh();
      toSd = app.useSd;
    } catch (e) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('$e')));
      return;
    }
    final files = await FilePicker.pickFiles(type: FileType.custom, allowedExtensions: musicExtensions);
    final src = files.isEmpty ? null : files.single.path;
    if (src == null || !mounted) return;
    final name = deviceFileName(src.split(RegExp(r'[\\/]')).last, sd: toSd);
    final path = '${toSd ? sdMusicDir : internalMusicDir}/$name';
    final data = await File(src).readAsBytes();

    final progress = ValueNotifier<double>(0);
    var cancel = false;
    final sw = Stopwatch()..start();
    if (!mounted) return;
    showDialog<void>(
      context: context,
      barrierDismissible: false,
      builder: (ctx) => AlertDialog(
        title: Text('${toSd ? 'SDカード' : '内蔵メモリ'}に送信中'),
        content: ValueListenableBuilder(
          valueListenable: progress,
          builder: (_, v, _) {
            final secs = sw.elapsedMilliseconds / 1000;
            final rate = secs > 0 ? v * data.length / secs : 0;
            return Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(name, maxLines: 2, overflow: TextOverflow.ellipsis),
              const SizedBox(height: 8),
              LinearProgressIndicator(value: v),
              const SizedBox(height: 8),
              Text('${formatBytes(v * data.length)} / ${formatBytes(data.length)}'
                  '${rate > 0 ? '（${formatBytes(rate)}/秒、残り約${((data.length * (1 - v)) / rate).round()}秒）' : ''}'),
            ]);
          },
        ),
        actions: [TextButton(onPressed: () => cancel = true, child: const Text('中止'))],
      ),
    );
    String? error;
    try {
      await link.uploadFile(path, data, onProgress: (f) => progress.value = f, cancelled: () => cancel);
    } catch (e) {
      error = e.toString();
    }
    if (!mounted) return;
    Navigator.pop(context);
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(error == null
          ? '送信しました（${(sw.elapsedMilliseconds / 1000).toStringAsFixed(0)}秒）'
          : error.contains('no space')
              ? '空き容量が足りません'
              : '失敗: $error'),
    ));
    await _load();
  }

  Future<void> _delete(DeviceFile f) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        content: Text('「${f.name}」を本体から消去します'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('やめる')),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('消去')),
        ],
      ),
    );
    if (ok != true) return;
    if (app.status['music'] == f.path) await app.run('music_stop');
    await app.run('delete_file', {'path': f.path});
    await _load();
  }

  Widget _usage(String label, IconData icon, int used, int total) {
    final frac = total > 0 ? used / total : 0.0;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          Icon(icon, size: 18),
          const SizedBox(width: 8),
          Text(label),
          const Spacer(),
          Text('${formatBytes(used)} / ${formatBytes(total)}（空き ${formatBytes(total - used)}）',
              style: Theme.of(context).textTheme.bodySmall),
        ]),
        const SizedBox(height: 4),
        LinearProgressIndicator(value: frac, minHeight: 6, borderRadius: BorderRadius.circular(3)),
      ]),
    );
  }

  String _assetSummary(List<DeviceFile> files) =>
      '${files.length}件 ${formatBytes(files.fold<int>(0, (s, f) => s + f.size))}';

  @override
  Widget build(BuildContext context) {
    final info = app.info;
    final sd = info['sd'] as Map? ?? {};
    final playing = app.status['music'] as String?;
    final theme = Theme.of(context);
    return RefreshIndicator(
      onRefresh: _load,
      child: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Text('容量', style: theme.textTheme.titleMedium),
          _usage('内蔵メモリ', Icons.memory, info['fsUsed'] as int? ?? 0, info['fsTotal'] as int? ?? 0),
          if (sd['mounted'] == true)
            _usage('SDカード', Icons.sd_card, sd['used'] as int? ?? 0, sd['total'] as int? ?? 0)
          else
            Row(children: [
              const Icon(Icons.sd_card_alert, size: 18),
              const SizedBox(width: 8),
              const Expanded(child: Text('SDカード: 挿さっていません')),
              TextButton(onPressed: _remount, child: const Text('読み込み直す')),
            ]),
          const SizedBox(height: 8),
          ChoiceField(
            label: '保存先',
            value: app.storage,
            options: storageLabels,
            onChanged: app.setStorage,
          ),
          Text('画像・GIF・音楽を送るときの保存先です', style: theme.textTheme.bodySmall),
          const Divider(height: 32),
          Row(children: [
            Expanded(child: Text('音楽', style: theme.textTheme.titleMedium)),
            if (loading) const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2)),
          ]),
          if (playing != null)
            Card(
              child: ListTile(
                leading: const Icon(Icons.graphic_eq),
                title: Text(playing.split('/').last, maxLines: 1, overflow: TextOverflow.ellipsis),
                subtitle: const Text('再生中'),
                trailing: IconButton(
                  tooltip: '止める',
                  icon: const Icon(Icons.stop),
                  onPressed: () => app.run('music_stop'),
                ),
              ),
            ),
          if (music != null && music!.isEmpty)
            const Padding(padding: EdgeInsets.symmetric(vertical: 8), child: Text('本体に音楽はありません')),
          for (final f in music ?? <DeviceFile>[])
            ListTile(
              contentPadding: EdgeInsets.zero,
              leading: Icon(f.onSd ? Icons.sd_card : Icons.memory),
              title: Text(f.name, maxLines: 2, overflow: TextOverflow.ellipsis),
              subtitle: Text('${f.onSd ? 'SDカード' : '内蔵'}・${formatBytes(f.size)}'),
              trailing: Row(mainAxisSize: MainAxisSize.min, children: [
                IconButton(
                  tooltip: playing == f.path ? '止める' : '再生',
                  icon: Icon(playing == f.path ? Icons.stop : Icons.play_arrow),
                  onPressed: () => playing == f.path
                      ? app.run('music_stop')
                      : app.run('music_play', {'path': f.path, 'loop': false}),
                ),
                PopupMenuButton<String>(
                  onSelected: (a) {
                    if (a == 'loop') app.run('music_play', {'path': f.path, 'loop': true});
                    if (a == 'del') _delete(f);
                  },
                  itemBuilder: (_) => const [
                    PopupMenuItem(value: 'loop', child: Text('くり返し再生')),
                    PopupMenuItem(value: 'del', child: Text('消去')),
                  ],
                ),
              ]),
            ),
          const SizedBox(height: 8),
          OutlinedButton.icon(
            onPressed: _addMusic,
            icon: const Icon(Icons.library_music),
            label: const Text('音楽を追加（MP3・WAV）'),
          ),
          Text('音量は「設定 → 音」で変えられます。プリセットのBGMにもできます', style: theme.textTheme.bodySmall),
          const Divider(height: 32),
          Text('画像・GIF', style: theme.textTheme.titleMedium),
          const SizedBox(height: 4),
          Text('内蔵メモリ ${_assetSummary(internalAssets)}'
              '${sd['mounted'] == true ? '\nSDカード ${_assetSummary(sdAssets)}' : ''}'),
          Text('プリセットを送ると自動で保存され、使われなくなったものは消えます', style: theme.textTheme.bodySmall),
        ],
      ),
    );
  }
}

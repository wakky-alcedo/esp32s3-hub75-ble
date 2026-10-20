import 'package:flutter/material.dart';

import '../model/content.dart';
import '../model/sender.dart';
import '../model/storage.dart';
import '../state/app_state.dart';
import 'item_editor.dart';

class PresetEditor extends StatefulWidget {
  const PresetEditor({super.key, required this.index});
  final int index;

  @override
  State<PresetEditor> createState() => _PresetEditorState();
}

class _PresetEditorState extends State<PresetEditor> {
  Preset get preset => Library.instance.presets[widget.index];
  final app = AppState.instance;
  int _nameRev = 0; // rebuilds the name field when a template renames the preset

  Future<void> _save() => Library.instance.save();

  Future<void> _addItem() async {
    final type = await showModalBottomSheet<String>(
      context: context,
      builder: (ctx) => SafeArea(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          for (final e in itemTypeLabels.entries)
            ListTile(leading: Icon(_icon(e.key)), title: Text(e.value), onTap: () => Navigator.pop(ctx, e.key)),
        ]),
      ),
    );
    if (type == null || !mounted) return;
    final edited = await _edit(ContentItem(type, {}));
    if (edited == null) return;
    setState(() => preset.items.add(edited));
    await _save();
  }

  Future<ContentItem?> _edit(ContentItem item) => Navigator.push<ContentItem>(
        context,
        MaterialPageRoute(builder: (_) => ItemEditor(item: item, width: app.width, height: app.height)),
      );

  Future<void> _applyTemplate(String name) async {
    if (preset.items.isNotEmpty) {
      final ok = await showDialog<bool>(
        context: context,
        builder: (ctx) => AlertDialog(
          content: Text('今の項目を「$name」の内容に置き換えます'),
          actions: [
            TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('やめる')),
            FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('置き換える')),
          ],
        ),
      );
      if (ok != true || !mounted) return;
    }
    final t = presetTemplates[name]!();
    setState(() {
      preset.items = t.items;
      preset.loop = t.loop;
      if (preset.name.isEmpty) {
        preset.name = t.name;
        _nameRev++;
      }
    });
    await _save();
  }

  Future<void> _pickBgm() async {
    final link = app.link;
    if (link == null) return;
    List<DeviceFile> files;
    try {
      files = await listMusic(link, app.info);
    } catch (e) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('読み込めません: $e')));
      return;
    }
    if (!mounted) return;
    final picked = await showModalBottomSheet<String>(
      context: context,
      isScrollControlled: true,
      builder: (ctx) => SafeArea(
        child: ListView(shrinkWrap: true, children: [
          ListTile(leading: const Icon(Icons.music_off), title: const Text('なし'), onTap: () => Navigator.pop(ctx, '')),
          for (final f in files)
            ListTile(
              leading: Icon(f.onSd ? Icons.sd_card : Icons.memory),
              title: Text(f.name, maxLines: 2, overflow: TextOverflow.ellipsis),
              subtitle: Text(formatBytes(f.size)),
              selected: f.path == preset.bgm,
              onTap: () => Navigator.pop(ctx, f.path),
            ),
          if (files.isEmpty)
            const Padding(
              padding: EdgeInsets.all(16),
              child: Text('本体に音楽がありません。「ファイル」タブから追加してください'),
            ),
        ]),
      ),
    );
    if (picked == null) return;
    setState(() => preset.bgm = picked.isEmpty ? null : picked);
    await _save();
  }

  Future<void> _send() async {
    final link = app.link;
    if (link == null) return;
    final progress = ValueNotifier<(String, double)>(('準備中', 0));
    showDialog<void>(
      context: context,
      barrierDismissible: false,
      builder: (_) => AlertDialog(
        title: const Text('送信中'),
        content: ValueListenableBuilder(
          valueListenable: progress,
          builder: (_, v, _) => Column(mainAxisSize: MainAxisSize.min, children: [
            LinearProgressIndicator(value: v.$2),
            const SizedBox(height: 8),
            Text(v.$1, overflow: TextOverflow.ellipsis),
          ]),
        ),
      ),
    );
    String? error;
    final sw = Stopwatch()..start();
    try {
      await app.refresh(); // picks up an SD card inserted since connecting
      await sendPreset(link, widget.index, preset,
          width: app.width,
          height: app.height,
          toSd: app.useSd,
          onProgress: (s, f) => progress.value = (s, f));
      await app.refresh();
    } catch (e) {
      error = '$e';
    }
    if (!mounted) return;
    Navigator.pop(context);
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(error == null ? '送信しました（${(sw.elapsedMilliseconds / 1000).toStringAsFixed(1)}秒）' : '失敗: $error'),
    ));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('プリセット ${widget.index + 1}'),
        actions: [
          PopupMenuButton<String>(
            tooltip: 'テンプレート',
            icon: const Icon(Icons.auto_fix_high),
            onSelected: _applyTemplate,
            itemBuilder: (_) => [
              for (final name in presetTemplates.keys) PopupMenuItem(value: name, child: Text('テンプレート: $name')),
            ],
          ),
        ],
      ),
      floatingActionButton: ListenableBuilder(
        listenable: app,
        builder: (_, _) => FloatingActionButton.extended(
          onPressed: app.connected && preset.items.isNotEmpty ? _send : null,
          icon: const Icon(Icons.send),
          label: Text(app.connected ? '送信して再生' : '未接続'),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 16, 16, 96),
        children: [
          TextFormField(
            key: ValueKey(_nameRev),
            initialValue: preset.name,
            decoration: const InputDecoration(labelText: '名前', border: OutlineInputBorder()),
            onChanged: (v) {
              preset.name = v;
              _save();
            },
          ),
          SwitchListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('フェードで切り替える'),
            value: preset.transition == 'fade',
            onChanged: (v) {
              setState(() => preset.transition = v ? 'fade' : 'cut');
              _save();
            },
          ),
          SwitchListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('最後まで行ったら最初に戻る'),
            subtitle: const Text('オフにすると最後の項目のまま止まる（タイマーなど）'),
            value: preset.loop,
            onChanged: (v) {
              setState(() => preset.loop = v);
              _save();
            },
          ),
          ListenableBuilder(
            listenable: app,
            builder: (_, _) => ListTile(
              contentPadding: EdgeInsets.zero,
              leading: const Icon(Icons.music_note),
              title: const Text('BGM（このプリセットの間ずっと流す）'),
              subtitle: Text(preset.bgm == null ? 'なし' : preset.bgm!.split('/').last,
                  maxLines: 1, overflow: TextOverflow.ellipsis),
              trailing: const Icon(Icons.chevron_right),
              onTap: app.connected ? _pickBgm : null,
            ),
          ),
          const Divider(),
          ReorderableListView(
            shrinkWrap: true,
            physics: const NeverScrollableScrollPhysics(),
            buildDefaultDragHandles: false,
            onReorderItem: (from, to) {
              setState(() => preset.items.insert(to, preset.items.removeAt(from)));
              _save();
            },
            children: [
              for (var i = 0; i < preset.items.length; i++)
                ListTile(
                  key: ObjectKey(preset.items[i]),
                  leading: ReorderableDragStartListener(index: i, child: Icon(_icon(preset.items[i].type))),
                  title: Text(preset.items[i].summary, maxLines: 1, overflow: TextOverflow.ellipsis),
                  subtitle: Text(itemTypeLabels[preset.items[i].type] ?? ''),
                  trailing: PopupMenuButton<String>(
                    onSelected: (a) {
                      setState(() {
                        if (a == 'dup') preset.items.insert(i + 1, preset.items[i].copy());
                        if (a == 'del') preset.items.removeAt(i);
                      });
                      _save();
                    },
                    itemBuilder: (_) => const [
                      PopupMenuItem(value: 'dup', child: Text('複製')),
                      PopupMenuItem(value: 'del', child: Text('削除')),
                    ],
                  ),
                  onTap: () async {
                    final edited = await _edit(preset.items[i]);
                    if (edited == null) return;
                    setState(() => preset.items[i] = edited);
                    _save();
                  },
                ),
            ],
          ),
          const SizedBox(height: 8),
          OutlinedButton.icon(onPressed: _addItem, icon: const Icon(Icons.add), label: const Text('項目を追加')),
        ],
      ),
    );
  }
}

IconData _icon(String type) => switch (type) {
      'text' => Icons.text_fields,
      'image' => Icons.image,
      'gif' => Icons.gif_box,
      'clock' => Icons.schedule,
      'countdown' => Icons.timer,
      'effect' => Icons.auto_awesome,
      'sensors' => Icons.sensors,
      _ => Icons.help_outline,
    };

import 'package:flutter/material.dart';

import '../model/content.dart';
import '../state/app_state.dart';
import 'preset_editor.dart';
import 'settings_tab.dart';
import 'widgets.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});
  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final app = AppState.instance;
  int tab = 0;

  @override
  void initState() {
    super.initState();
    app.startScan();
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([app, Library.instance]),
      builder: (context, _) {
        final err = app.error;
        if (err != null) {
          WidgetsBinding.instance.addPostFrameCallback((_) {
            ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(err)));
            app.clearError();
          });
        }
        return Scaffold(
          appBar: AppBar(
            title: Text(app.connected ? (app.config['name'] as String? ?? app.deviceName ?? '') : 'LED Display'),
            actions: [
              if (app.connected)
                IconButton(
                  tooltip: '切断',
                  icon: const Icon(Icons.bluetooth_connected),
                  onPressed: () => app.disconnect(forget: true),
                ),
            ],
          ),
          body: switch (tab) {
            0 => app.connected ? const _ControlTab() : const _ConnectPanel(),
            1 => const _PresetsTab(),
            _ => app.connected ? const SettingsTab() : const _ConnectPanel(),
          },
          bottomNavigationBar: NavigationBar(
            selectedIndex: tab,
            onDestinationSelected: (i) => setState(() => tab = i),
            destinations: const [
              NavigationDestination(icon: Icon(Icons.tune), label: '操作'),
              NavigationDestination(icon: Icon(Icons.playlist_play), label: 'プリセット'),
              NavigationDestination(icon: Icon(Icons.settings), label: '設定'),
            ],
          ),
        );
      },
    );
  }
}

class _ConnectPanel extends StatelessWidget {
  const _ConnectPanel();

  @override
  Widget build(BuildContext context) {
    final app = AppState.instance;
    final devices = app.found.values.toList();
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Row(children: [
          Expanded(
            child: Text(switch (app.state) {
              LinkState.scanning => '近くのディスプレイを探しています…',
              LinkState.connecting => '接続中…（初回はパネルに出るPINを入力）',
              _ => 'ディスプレイに接続してください',
            }),
          ),
          if (app.state == LinkState.scanning || app.state == LinkState.connecting)
            const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2))
          else
            FilledButton(onPressed: app.startScan, child: const Text('探す')),
        ]),
        const SizedBox(height: 16),
        for (final d in devices)
          Card(
            child: ListTile(
              leading: const Icon(Icons.grid_on),
              title: Text(d.name ?? '(名前なし)'),
              subtitle: Text('${d.deviceId}  RSSI ${d.rssi ?? '-'}'),
              onTap: app.state == LinkState.connecting ? null : () => app.connect(d),
            ),
          ),
      ],
    );
  }
}

class _ControlTab extends StatefulWidget {
  const _ControlTab();
  @override
  State<_ControlTab> createState() => _ControlTabState();
}

class _ControlTabState extends State<_ControlTab> {
  final app = AppState.instance;
  int? _brightness, _speed; // while dragging

  @override
  Widget build(BuildContext context) {
    final st = app.status;
    final paused = st['paused'] == true;
    final standby = st['standby'] == true;
    final preset = st['preset'] as int? ?? 0;
    final storedPresets = (app.info['presets'] as List? ?? []).cast<int>();
    return RefreshIndicator(
      onRefresh: app.refresh,
      child: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Card(
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Row(mainAxisAlignment: MainAxisAlignment.spaceEvenly, children: [
                IconButton.filledTonal(
                  iconSize: 32,
                  onPressed: () => app.run('prev'),
                  icon: const Icon(Icons.skip_previous),
                ),
                IconButton.filled(
                  iconSize: 40,
                  onPressed: () => app.run(paused ? 'play' : 'pause'),
                  icon: Icon(paused ? Icons.play_arrow : Icons.pause),
                ),
                IconButton.filledTonal(
                  iconSize: 32,
                  onPressed: () => app.run('next'),
                  icon: const Icon(Icons.skip_next),
                ),
                IconButton.outlined(
                  iconSize: 32,
                  tooltip: standby ? '点灯' : '消灯',
                  onPressed: () => app.run('standby', {'on': !standby}),
                  icon: Icon(standby ? Icons.lightbulb_outline : Icons.lightbulb),
                ),
              ]),
            ),
          ),
          const SizedBox(height: 8),
          IntSlider(
            label: '明るさ',
            value: _brightness ?? (st['brightness'] as int? ?? 60),
            min: 1,
            max: 255,
            onChanged: (v) {
              setState(() => _brightness = v);
              // Stream without writing flash; persist once on release.
              app.sendLatest('brightness', {'value': v, 'save': false});
            },
            onChangeEnd: (v) async {
              await app.run('brightness', {'value': v});
              setState(() => _brightness = null);
            },
          ),
          IntSlider(
            label: '速さ',
            value: _speed ?? (st['speed'] as int? ?? 100),
            min: 10,
            max: 300,
            format: (v) => '$v%',
            onChanged: (v) {
              setState(() => _speed = v);
              app.sendLatest('speed', {'value': v, 'save': false});
            },
            onChangeEnd: (v) async {
              await app.run('speed', {'value': v});
              setState(() => _speed = null);
            },
          ),
          const SizedBox(height: 16),
          const Text('プリセット'),
          const SizedBox(height: 8),
          Wrap(spacing: 8, runSpacing: 8, children: [
            for (var i = 0; i < presetSlots; i++)
              ChoiceChip(
                label: Text(Library.instance.presets[i].name.isEmpty
                    ? '${i + 1}'
                    : '${i + 1} ${Library.instance.presets[i].name}'),
                selected: preset == i,
                onSelected: storedPresets.contains(i) || i == 0 ? (_) => app.run('preset', {'index': i}) : null,
              ),
          ]),
          const SizedBox(height: 24),
          Text(
            '推定電流 ${st['mA'] ?? '-'} mA'
            '${(st['powerScale'] as int? ?? 256) < 256 ? '（電流制限で ${((st['powerScale'] as int) * 100 / 256).round()}% に減光中）' : ''}\n'
            '${app.width}x${app.height}  FW ${app.info['fw'] ?? '-'}',
            style: Theme.of(context).textTheme.bodySmall,
          ),
        ],
      ),
    );
  }
}

class _PresetsTab extends StatelessWidget {
  const _PresetsTab();

  @override
  Widget build(BuildContext context) {
    final lib = Library.instance;
    final stored = (AppState.instance.info['presets'] as List? ?? []).cast<int>();
    return ListView(
      padding: const EdgeInsets.all(8),
      children: [
        for (var i = 0; i < presetSlots; i++)
          Card(
            child: ListTile(
              leading: CircleAvatar(child: Text('${i + 1}')),
              title: Text(lib.presets[i].name.isEmpty ? '（無題）' : lib.presets[i].name),
              subtitle: Text(
                '${lib.presets[i].items.length}項目'
                '${stored.contains(i) ? '・本体に保存済み' : ''}',
              ),
              trailing: const Icon(Icons.chevron_right),
              onTap: () => Navigator.push(context, MaterialPageRoute(builder: (_) => PresetEditor(index: i))),
            ),
          ),
      ],
    );
  }
}

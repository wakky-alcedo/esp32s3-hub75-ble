import 'package:flutter/material.dart';

import '../state/app_state.dart';
import 'widgets.dart';

class SettingsTab extends StatefulWidget {
  const SettingsTab({super.key});
  @override
  State<SettingsTab> createState() => _SettingsTabState();
}

class _SettingsTabState extends State<SettingsTab> {
  final app = AppState.instance;
  late Map<String, dynamic> panel = Map<String, dynamic>.from(app.config['panel'] as Map? ?? {});
  late Map<String, dynamic> power = {
    'powerProfile': app.config['powerProfile'] ?? 'battery',
    'limitBatteryMa': app.config['limitBatteryMa'] ?? 2000,
    'limitAcMa': app.config['limitAcMa'] ?? 4000,
  };
  late String name = app.config['name'] as String? ?? '';
  late int volume = app.config['volume'] as int? ?? 60;
  late int tempOffset10 = app.config['tempOffset10'] as int? ?? 0;
  late int micGain = app.config['micGain'] as int? ?? 24;
  Map<String, dynamic>? sensors;

  Future<void> _readSensors() async {
    final r = await app.run('sensors');
    if (r != null && mounted) setState(() => sensors = Map<String, dynamic>.from(r['sensors'] as Map));
  }

  Future<void> _calibrateImu() async {
    final r = await app.run('imu_calibrate');
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(r?['ok'] == true ? '今の向きを「下」として覚えました' : 'パネルを立てた状態で押してください'),
    ));
    await app.refresh();
  }

  bool get _layoutDirty {
    final cur = app.config['panel'] as Map? ?? {};
    for (final k in ['w', 'h', 'cols', 'rows', 'startTop', 'startLeft', 'serpentine', 'driver']) {
      if (cur[k] != panel[k]) return true;
    }
    return false;
  }

  Future<void> _applyOrientation(Map<String, dynamic> change) async {
    setState(() => panel.addAll(change));
    final rebooting = await app.setConfig({'panel': change});
    if (rebooting && mounted) _rebootNotice();
  }

  void _rebootNotice() {
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('本体を再起動しました。数秒後に接続し直してください')),
    );
    app.startScan();
  }

  @override
  Widget build(BuildContext context) {
    final size = '${panel['w']}x${panel['h']}';
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Text('設置の向き', style: Theme.of(context).textTheme.titleMedium),
        ChoiceField(
          label: '回転',
          value: '${panel['rotation'] ?? 0}',
          options: const {'0': '0°', '1': '90°', '2': '180°', '3': '270°'},
          onChanged: (v) => _applyOrientation({'rotation': int.parse(v)}),
        ),
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('左右反転'),
          value: panel['flipH'] == true,
          onChanged: (v) => _applyOrientation({'flipH': v}),
        ),
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('上下反転'),
          value: panel['flipV'] == true,
          onChanged: (v) => _applyOrientation({'flipV': v}),
        ),
        const Text('※ 90°/270° に変えると縦横が入れ替わるため再起動します', style: TextStyle(fontSize: 12)),
        const Divider(height: 32),
        Text('パネル構成', style: Theme.of(context).textTheme.titleMedium),
        ChoiceField(
          label: 'パネル',
          value: size,
          options: const {'64x32': '64×32', '64x64': '64×64'},
          onChanged: (v) => setState(() {
            final parts = v.split('x');
            panel['w'] = int.parse(parts[0]);
            panel['h'] = int.parse(parts[1]);
          }),
        ),
        IntSlider(label: '横の枚数', value: panel['cols'] as int? ?? 1, min: 1, max: 6, onChanged: (v) => setState(() => panel['cols'] = v)),
        IntSlider(label: '縦の枚数', value: panel['rows'] as int? ?? 1, min: 1, max: 6, onChanged: (v) => setState(() => panel['rows'] = v)),
        ChoiceField(
          label: '入力の位置',
          value: '${panel['startTop'] == false ? 'b' : 't'}${panel['startLeft'] == false ? 'r' : 'l'}',
          options: const {'tl': '左上', 'tr': '右上', 'bl': '左下', 'br': '右下'},
          onChanged: (v) => setState(() {
            panel['startTop'] = v[0] == 't';
            panel['startLeft'] = v[1] == 'l';
          }),
        ),
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('S字配線（偶数段は逆さ）'),
          value: panel['serpentine'] != false,
          onChanged: (v) => setState(() => panel['serpentine'] = v),
        ),
        ChoiceField(
          label: 'ドライバIC',
          value: '${panel['driver'] ?? 0}',
          options: const {'0': '標準', '1': 'FM6124', '2': 'FM6126A', '3': 'ICN2038S', '4': 'MBI5124', '5': 'DP3246'},
          onChanged: (v) => setState(() => panel['driver'] = int.parse(v)),
        ),
        if ((panel['cols'] as int? ?? 1) * (panel['rows'] as int? ?? 1) > 6)
          const Text('合計6枚までです', style: TextStyle(color: Colors.redAccent)),
        Align(
          alignment: Alignment.centerRight,
          child: FilledButton(
            onPressed: _layoutDirty
                ? () async {
                    if (await app.setConfig({'panel': panel}) && mounted) _rebootNotice();
                  }
                : null,
            child: const Text('適用（再起動）'),
          ),
        ),
        const Divider(height: 32),
        Text('電源', style: Theme.of(context).textTheme.titleMedium),
        ChoiceField(
          label: '電源の種類',
          value: power['powerProfile'] as String,
          options: const {'battery': 'モバイルバッテリー', 'ac': 'ACアダプタ'},
          onChanged: (v) {
            setState(() => power['powerProfile'] = v);
            app.setConfig({'powerProfile': v});
          },
        ),
        IntSlider(
          label: 'バッテリー上限',
          value: power['limitBatteryMa'] as int,
          min: 500,
          max: 3000,
          format: (v) => '${(v / 1000).toStringAsFixed(1)}A',
          onChanged: (v) => setState(() => power['limitBatteryMa'] = (v ~/ 100) * 100),
          onChangeEnd: (v) => app.setConfig({'limitBatteryMa': (v ~/ 100) * 100}),
        ),
        IntSlider(
          label: 'AC上限',
          value: power['limitAcMa'] as int,
          min: 1000,
          max: 20000,
          format: (v) => '${(v / 1000).toStringAsFixed(1)}A',
          onChanged: (v) => setState(() => power['limitAcMa'] = (v ~/ 500) * 500),
          onChangeEnd: (v) => app.setConfig({'limitAcMa': (v ~/ 500) * 500}),
        ),
        const Divider(height: 32),
        Text('音', style: Theme.of(context).textTheme.titleMedium),
        if (app.info['sound'] != true)
          const Text('スピーカーが使えません（コーデックが見つかりません）', style: TextStyle(color: Colors.redAccent)),
        Row(children: [
          Expanded(
            child: IntSlider(
              label: '音量',
              value: volume,
              min: 0,
              max: 100,
              format: (v) => v == 0 ? '消音' : '$v',
              onChanged: (v) => setState(() => volume = v),
              onChangeEnd: (v) async {
                await app.setConfig({'volume': v});
                app.run('sound', {'name': 'chime'});
              },
            ),
          ),
          IconButton(
            tooltip: '試しに鳴らす',
            onPressed: app.info['sound'] == true ? () => app.run('sound', {'name': 'chime'}) : null,
            icon: const Icon(Icons.volume_up),
          ),
        ]),
        const Divider(height: 32),
        Text('センサー', style: Theme.of(context).textTheme.titleMedium),
        ListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('今の値'),
          subtitle: Text(sensors == null
              ? '更新ボタンで読み込みます'
              : '温度 ${sensors!['tempC']}℃  湿度 ${sensors!['humidity']}%  チップ ${sensors!['chipC']}℃\n'
                  '加速度 ${(sensors!['acc'] as List).join(' / ')} g'),
          trailing: IconButton(tooltip: '更新', onPressed: _readSensors, icon: const Icon(Icons.refresh)),
        ),
        ListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('傾きの向きを合わせる'),
          subtitle: const Text('パネルをいつもの向きに立てて押すと、その向きを「下」として覚えます'),
          trailing: OutlinedButton(onPressed: _calibrateImu, child: const Text('合わせる')),
        ),
        IntSlider(
          label: '温度の補正',
          value: tempOffset10,
          min: -150,
          max: 50,
          format: (v) => '${v > 0 ? '+' : ''}${(v / 10).toStringAsFixed(1)}℃',
          onChanged: (v) => setState(() => tempOffset10 = v),
          onChangeEnd: (v) => app.setConfig({'tempOffset10': v}),
        ),
        const Text('※ 基板の熱で高めに出るため、室温計と比べて合わせてください', style: TextStyle(fontSize: 12)),
        IntSlider(
          label: 'マイク感度',
          value: micGain,
          min: 0,
          max: 37,
          format: (v) => '${v}dB',
          onChanged: (v) => setState(() => micGain = v),
          onChangeEnd: (v) => app.setConfig({'micGain': v}),
        ),
        const Divider(height: 32),
        Text('本体', style: Theme.of(context).textTheme.titleMedium),
        Row(children: [
          Expanded(
            child: TextFormField(
              initialValue: name,
              maxLength: 20,
              decoration: const InputDecoration(labelText: 'デバイス名'),
              onChanged: (v) => name = v,
            ),
          ),
          TextButton(
            onPressed: () async {
              if (await app.setConfig({'name': name}) && mounted) _rebootNotice();
            },
            child: const Text('変更'),
          ),
        ]),
        ListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('情報'),
          subtitle: Text(
            'FW ${app.info['fw']}  プロトコル v${app.info['proto']}\n'
            '解像度 ${app.width}x${app.height}\n'
            '保存領域 ${((app.info['fsUsed'] as int? ?? 0) / 1024).round()}KB / '
            '${((app.info['fsTotal'] as int? ?? 0) / 1024 / 1024).toStringAsFixed(1)}MB\n'
            'RTC ${app.info['rtc'] == true ? 'あり' : 'なし'}  '
            'マイク ${app.info['mic'] == true ? 'あり' : 'なし'}  '
            'センサー ${app.info['sensors'] == true ? 'あり' : 'なし'}\n'
            'SDカード ${(app.info['sd'] as Map?)?['mounted'] == true ? '${(((app.info['sd'] as Map)['total'] as int) / 1e9).toStringAsFixed(1)}GB' : 'なし'}',
          ),
        ),
        Wrap(spacing: 8, children: [
          OutlinedButton(
            onPressed: () async {
              await app.run('reboot');
              if (mounted) _rebootNotice();
            },
            child: const Text('再起動'),
          ),
          OutlinedButton(
            onPressed: () async {
              final ok = await showDialog<bool>(
                context: context,
                builder: (ctx) => AlertDialog(
                  content: const Text('ペアリング情報を消去します。次回接続時にPINの入力が必要です。'),
                  actions: [
                    TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('やめる')),
                    FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('消去')),
                  ],
                ),
              );
              if (ok == true) {
                await app.run('clear_bonds');
                await app.disconnect(forget: true);
              }
            },
            child: const Text('ペアリング解除'),
          ),
        ]),
      ],
    );
  }
}

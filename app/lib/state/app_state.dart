import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:universal_ble/universal_ble.dart';

import '../ble/link.dart';

enum LinkState { idle, scanning, connecting, connected }

/// Connection to the display and the device state mirrored from it.
class AppState extends ChangeNotifier {
  AppState._();
  static final instance = AppState._();

  LinkState state = LinkState.idle;
  final found = <String, BleDevice>{};
  DeviceLink? link;
  String? deviceName;
  String? error;

  Map<String, dynamic> info = {};
  Map<String, dynamic> status = {};
  Map<String, dynamic> config = {};

  StreamSubscription<BleDevice>? _scanSub;
  StreamSubscription<bool>? _connSub;
  StreamSubscription<Map<String, dynamic>>? _evtSub;

  int get width => (info['width'] as int?) ?? 64;
  int get height => (info['height'] as int?) ?? 32;
  bool get connected => state == LinkState.connected;

  Future<void> startScan() async {
    error = null;
    found.clear();
    await UniversalBle.requestPermissions();
    state = LinkState.scanning;
    notifyListeners();
    _scanSub ??= UniversalBle.scanStream.listen((d) {
      found[d.deviceId] = d;
      notifyListeners();
      _maybeAutoConnect(d);
    });
    try {
      await UniversalBle.startScan(scanFilter: ScanFilter(withServices: [Uuids.service]));
    } catch (e) {
      error = 'スキャンできません: $e';
      state = LinkState.idle;
      notifyListeners();
    }
  }

  Future<void> stopScan() async {
    try {
      await UniversalBle.stopScan();
    } catch (_) {}
    if (state == LinkState.scanning) state = LinkState.idle;
    notifyListeners();
  }

  Future<void> _maybeAutoConnect(BleDevice d) async {
    if (state != LinkState.scanning) return;
    final prefs = await SharedPreferences.getInstance();
    if (prefs.getString('lastDevice') == d.deviceId) await connect(d);
  }

  Future<void> connect(BleDevice d) async {
    await stopScan();
    state = LinkState.connecting;
    deviceName = d.name;
    error = null;
    notifyListeners();
    final l = DeviceLink(d.deviceId);
    try {
      await l.open();
      link = l;
      _connSub = UniversalBle.connectionStream(d.deviceId).listen((up) {
        if (!up) _onDisconnected();
      });
      _evtSub = l.events.listen(_onEvent);
      await _syncTime();
      await refresh();
      state = LinkState.connected;
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString('lastDevice', d.deviceId);
    } catch (e) {
      error = '接続できません: $e';
      await l.close();
      link = null;
      state = LinkState.idle;
    }
    notifyListeners();
  }

  Future<void> disconnect({bool forget = false}) async {
    if (forget) {
      final prefs = await SharedPreferences.getInstance();
      await prefs.remove('lastDevice');
    }
    final l = link;
    _onDisconnected();
    await l?.close();
  }

  void _onDisconnected() {
    _connSub?.cancel();
    _evtSub?.cancel();
    _connSub = null;
    _evtSub = null;
    link = null;
    if (state == LinkState.connected) error = '切断されました';
    state = LinkState.idle;
    notifyListeners();
  }

  void _onEvent(Map<String, dynamic> evt) {
    if (evt['evt'] == 'status') {
      status = Map<String, dynamic>.from(evt['status'] as Map);
      notifyListeners();
    }
  }

  Future<void> _syncTime() async {
    final now = DateTime.now();
    await link!.command('sync_time', {
      'epoch': now.millisecondsSinceEpoch ~/ 1000,
      'tzOffsetMin': now.timeZoneOffset.inMinutes,
    });
  }

  Future<void> refresh() async {
    final l = link;
    if (l == null) return;
    info = Map<String, dynamic>.from((await l.command('info'))['info'] as Map);
    status = Map<String, dynamic>.from((await l.command('status'))['status'] as Map);
    config = Map<String, dynamic>.from((await l.command('get_config'))['config'] as Map);
    notifyListeners();
  }

  /// Runs a command and refreshes status; errors land in [error].
  Future<Map<String, dynamic>?> run(String cmd, [Map<String, dynamic>? args]) async {
    final l = link;
    if (l == null) return null;
    try {
      final r = await l.command(cmd, args);
      status = Map<String, dynamic>.from((await l.command('status'))['status'] as Map);
      notifyListeners();
      return r;
    } catch (e) {
      error = e.toString();
      notifyListeners();
      return null;
    }
  }

  /// Applies a partial config. Returns true when the device restarts.
  Future<bool> setConfig(Map<String, dynamic> partial) async {
    final r = await run('set_config', {'config': partial});
    if (r == null) return false;
    if (r['reboot'] == true) {
      await disconnect();
      return true;
    }
    config = Map<String, dynamic>.from((await link!.command('get_config'))['config'] as Map);
    notifyListeners();
    return false;
  }

  final _latest = <String, Map<String, dynamic>>{};
  final _inflight = <String>{};

  /// For sliders: sends only the newest value per command while one is in
  /// flight, so a fast drag never queues up dozens of BLE writes.
  void sendLatest(String cmd, Map<String, dynamic> args) {
    _latest[cmd] = args;
    if (_inflight.contains(cmd)) return;
    _pump(cmd);
  }

  Future<void> _pump(String cmd) async {
    final l = link;
    final args = _latest.remove(cmd);
    if (l == null || args == null) return;
    _inflight.add(cmd);
    try {
      await l.command(cmd, args);
    } catch (_) {
    } finally {
      _inflight.remove(cmd);
    }
    if (_latest.containsKey(cmd)) await _pump(cmd);
  }

  void clearError() {
    error = null;
    notifyListeners();
  }
}

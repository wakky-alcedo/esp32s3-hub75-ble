import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:universal_ble/universal_ble.dart';

import '../ble/link.dart';
import '../model/storage.dart';

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
  Timer? _scanTimer;
  bool _autoTried = false;

  /// Upload destination setting, see storageLabels.
  String storage = 'auto';

  Future<void> loadPrefs() async {
    final prefs = await SharedPreferences.getInstance();
    storage = prefs.getString('storage') ?? 'auto';
  }

  Future<void> setStorage(String v) async {
    storage = v;
    notifyListeners();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString('storage', v);
  }

  /// Whether uploads go to the SD card now. Throws when the setting asks for
  /// the card and none is inserted.
  bool get useSd => resolveUseSd(storage, info);

  int get width => (info['width'] as int?) ?? 64;
  int get height => (info['height'] as int?) ?? 32;
  bool get connected => state == LinkState.connected;

  Future<void> startScan() async {
    if (state != LinkState.idle) return;
    error = null;
    found.clear();
    _autoTried = false;
    state = LinkState.scanning;
    notifyListeners();
    _log('scan: requesting permissions');
    try {
      await UniversalBle.requestPermissions();
      _log('scan: permissions ok');
    } catch (e) {
      error = 'Bluetoothの権限がありません: $e';
      state = LinkState.idle;
      notifyListeners();
      return;
    }
    _scanSub ??= UniversalBle.scanStream.listen((d) {
      if (!found.containsKey(d.deviceId)) _log('scan: found ${d.deviceId} ${d.name}');
      found[d.deviceId] = d;
      notifyListeners();
      _maybeAutoConnect(d);
    });
    try {
      await UniversalBle.startScan(scanFilter: ScanFilter(withServices: [Uuids.service]));
      _log('scan: started');
      // Give up after a while so the UI offers a retry instead of spinning.
      _scanTimer?.cancel();
      _scanTimer = Timer(const Duration(seconds: 15), () {
        if (state == LinkState.scanning) stopScan();
      });
    } catch (e) {
      error = 'スキャンできません: $e';
      state = LinkState.idle;
      notifyListeners();
    }
  }

  Future<void> stopScan() async {
    _log('scan: stop');
    _scanTimer?.cancel();
    try {
      await UniversalBle.stopScan();
    } catch (_) {}
    if (state == LinkState.scanning) state = LinkState.idle;
    notifyListeners();
  }

  Future<void> _maybeAutoConnect(BleDevice d) async {
    if (state != LinkState.scanning || _autoTried) return;
    final prefs = await SharedPreferences.getInstance();
    if (state != LinkState.scanning || _autoTried) return;
    if (prefs.getString('lastDevice') == d.deviceId) {
      _autoTried = true; // once per scan, so a failing device can't loop
      await connect(d);
    }
  }

  Future<void> connect(BleDevice d) async {
    if (state == LinkState.connecting || state == LinkState.connected) return;
    state = LinkState.connecting;
    await stopScan();
    deviceName = d.name;
    error = null;
    notifyListeners();
    _log('connect: ${d.deviceId}');
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
      _log('connect failed: $e');
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
    _onDisconnected(expected: true);
    await l?.close();
  }

  void _onDisconnected({bool expected = false}) {
    _log('disconnected (state=$state expected=$expected)');
    // A failed connect() cleans up after itself.
    if (state != LinkState.connected) return;
    _connSub?.cancel();
    _evtSub?.cancel();
    _connSub = null;
    _evtSub = null;
    link = null;
    state = LinkState.idle;
    if (!expected) {
      error = '切断されました。再接続します';
      // The device may be rebooting; look for it again (auto-connects once).
      Future.delayed(const Duration(seconds: 2), startScan);
    }
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
      Future.delayed(const Duration(seconds: 3), startScan);
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

  void _log(String m) => debugPrint('[led] $m');

  void clearError() {
    error = null;
    notifyListeners();
  }
}

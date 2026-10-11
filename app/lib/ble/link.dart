import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:archive/archive.dart' show getCrc32;
import 'package:flutter/foundation.dart';
import 'package:universal_ble/universal_ble.dart';

/// GATT layout and wire format: docs/protocol.md
class Uuids {
  static const service = '6e4c0001-7b3a-4f2e-9a61-4c45442d4453';
  static const control = '6e4c0002-7b3a-4f2e-9a61-4c45442d4453';
  static const notify = '6e4c0003-7b3a-4f2e-9a61-4c45442d4453';
  static const bulk = '6e4c0004-7b3a-4f2e-9a61-4c45442d4453';
}

class DeviceError implements Exception {
  DeviceError(this.message);
  final String message;
  @override
  String toString() => message;
}

/// One connection to a display: JSON commands, events and file uploads.
class DeviceLink {
  DeviceLink(this.deviceId);

  final String deviceId;
  int mtu = 23;
  int _nextId = 1;
  final _pending = <int, Completer<Map<String, dynamic>>>{};
  final _fragments = BytesBuilder();
  final _events = StreamController<Map<String, dynamic>>.broadcast();
  StreamSubscription<Uint8List>? _valueSub;

  /// Unsolicited {"evt": ...} messages from the device.
  Stream<Map<String, dynamic>> get events => _events.stream;

  Future<void> open() async {
    debugPrint('[led] link: connecting');
    await UniversalBle.connect(deviceId, timeout: const Duration(seconds: 15));
    debugPrint('[led] link: discovering');
    await UniversalBle.discoverServices(deviceId);
    if (Platform.isAndroid) {
      // Bond up front so the OS PIN dialog appears before the first command.
      final paired = await UniversalBle.isPaired(deviceId) ?? false;
      debugPrint('[led] link: paired=$paired');
      if (!paired) await UniversalBle.pair(deviceId, timeout: const Duration(seconds: 60));
      try {
        await UniversalBle.requestConnectionPriority(deviceId, BleConnectionPriority.highPerformance);
      } catch (_) {}
    }
    try {
      mtu = await UniversalBle.requestMtu(deviceId, 247);
    } catch (_) {
      // Windows negotiates on its own and does not report it here.
      mtu = Platform.isWindows ? 185 : 23;
    }
    debugPrint('[led] link: mtu=$mtu, subscribing');
    _valueSub = UniversalBle.characteristicValueStream(deviceId, Uuids.notify).listen(_onNotify);
    await UniversalBle.subscribeNotifications(deviceId, Uuids.service, Uuids.notify);
  }

  Future<void> close() async {
    await _valueSub?.cancel();
    for (final c in _pending.values) {
      if (!c.isCompleted) c.completeError(DeviceError('切断されました'));
    }
    _pending.clear();
    try {
      await UniversalBle.disconnect(deviceId);
    } catch (_) {}
  }

  void _onNotify(Uint8List value) {
    if (value.isEmpty) return;
    _fragments.add(value.sublist(1));
    if (value[0] & 0x01 != 0) return; // more fragments follow
    final text = utf8.decode(_fragments.takeBytes(), allowMalformed: true);
    Map<String, dynamic> msg;
    try {
      msg = jsonDecode(text) as Map<String, dynamic>;
    } catch (e) {
      debugPrint('[link] bad notify: $text');
      return;
    }
    final id = msg['id'];
    if (id is int && _pending.containsKey(id)) {
      _pending.remove(id)!.complete(msg);
    } else if (msg.containsKey('evt')) {
      _events.add(msg);
    }
  }

  /// Sends a command and returns the response. Throws [DeviceError] when the
  /// device answers with ok=false.
  Future<Map<String, dynamic>> command(String cmd, [Map<String, dynamic>? args]) async {
    final id = _nextId++;
    final req = <String, dynamic>{'cmd': cmd, 'id': id, ...?args};
    final completer = Completer<Map<String, dynamic>>();
    _pending[id] = completer;
    try {
      await UniversalBle.write(
        deviceId,
        Uuids.service,
        Uuids.control,
        Uint8List.fromList(utf8.encode(jsonEncode(req))),
      );
    } catch (e) {
      _pending.remove(id);
      rethrow;
    }
    final resp = await completer.future.timeout(const Duration(seconds: 15), onTimeout: () {
      _pending.remove(id);
      throw DeviceError('$cmd: 応答がありません');
    });
    if (resp['ok'] != true) throw DeviceError('$cmd: ${resp['error'] ?? 'failed'}');
    return resp;
  }

  /// Uploads [data] to [path] on the device's flash.
  Future<void> uploadFile(String path, Uint8List data, {void Function(double)? onProgress}) async {
    await command('file_begin', {'path': path, 'size': data.length, 'crc': getCrc32(data)});
    final chunk = (mtu - 3 - 4).clamp(16, 240);
    const window = 24; // chunks between flow-control checks
    var offset = 0;
    try {
      while (offset < data.length) {
        var sent = 0;
        var off = offset;
        while (off < data.length && sent < window) {
          final end = (off + chunk).clamp(0, data.length);
          final packet = BytesBuilder()
            ..add((ByteData(4)..setUint32(0, off, Endian.little)).buffer.asUint8List())
            ..add(data.sublist(off, end));
          await UniversalBle.write(deviceId, Uuids.service, Uuids.bulk, packet.takeBytes(), withoutResponse: true);
          off = end;
          sent++;
        }
        // The device reports how far it really got; resend from there if a
        // write-without-response was dropped.
        final st = await command('file_status');
        offset = st['expected'] as int;
        onProgress?.call(offset / data.length);
      }
      await command('file_end');
    } catch (e) {
      try {
        await command('file_abort');
      } catch (_) {}
      rethrow;
    }
  }
}

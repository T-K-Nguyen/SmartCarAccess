import 'dart:async';
import 'dart:convert';
import 'package:flutter/services.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:nfc_manager/nfc_manager.dart';

class MasterCardPayload {
  MasterCardPayload({
    required this.vehicleId,
    required this.masterSecret,
  });

  final Uint8List vehicleId; // 8 bytes
  final Uint8List masterSecret; // 32 bytes

  String get vehicleIdHex => _bytesToHex(vehicleId);
  String get masterSecretHex => _bytesToHex(masterSecret);

  static String _bytesToHex(Uint8List bytes) {
    final sb = StringBuffer();
    for (final b in bytes) {
      sb.write(b.toRadixString(16).padLeft(2, '0').toUpperCase());
    }
    return sb.toString();
  }
}

/// Master card provisioning helper.
/// - Reads NDEF text payload from master card
/// - Parses JSON {"vid":"...","msk":"..."}
/// - Sends session data to Android HCE service (in-memory only)
class MasterCardProvisioningService {
  static const MethodChannel _channel = MethodChannel('smartcar/mastercard');
  static const MethodChannel _readerChannel = MethodChannel('smartcar/nfc_reader');
  static const String _pendingPrefix = 'pending_mastercard_';
  static final FlutterSecureStorage _secureStorage = FlutterSecureStorage();

  Future<MasterCardPayload> readMasterCard({
    Duration timeout = const Duration(seconds: 30),
  }) async {
    final completer = Completer<MasterCardPayload>();

    // Enable reader mode to suppress system UI while scanning
    await _readerChannel.invokeMethod('enableReaderMode');

    await NfcManager.instance.startSession(
      onDiscovered: (tag) async {
        try {
          final ndef = Ndef.from(tag);
          if (ndef == null) {
            throw StateError('NDEF not supported on this tag');
          }
          final message = ndef.cachedMessage;
          if (message == null || message.records.isEmpty) {
            throw StateError('Empty NDEF message');
          }

          final text = _extractPayloadText(message.records);
          final payload = _parsePayload(text);

          completer.complete(payload);
        } catch (e) {
          completer.completeError(e);
        } finally {
          await NfcManager.instance.stopSession();
        }
      },
      onError: (error) async {
        if (!completer.isCompleted) {
          completer.completeError(error);
        }
        await NfcManager.instance.stopSession();
      },
    );

    return completer.future.timeout(timeout, onTimeout: () async {
      await NfcManager.instance.stopSession();
      throw TimeoutException('Timed out waiting for master card');
    });
  }

  Future<void> savePendingPayload(String carId, MasterCardPayload payload) async {
    final data = jsonEncode({
      'vid': payload.vehicleIdHex,
      'msk': payload.masterSecretHex,
    });
    await _secureStorage.write(key: '$_pendingPrefix$carId', value: data);
  }

  Future<MasterCardPayload?> loadPendingPayload(String carId) async {
    final raw = await _secureStorage.read(key: '$_pendingPrefix$carId');
    if (raw == null) return null;
    try {
      return _parsePayload(raw);
    } catch (_) {
      return null;
    }
  }

  Future<void> clearPendingPayload(String carId) async {
    await _secureStorage.delete(key: '$_pendingPrefix$carId');
  }

  Future<void> activateHceSession(
    MasterCardPayload payload, {
    Duration ttl = const Duration(seconds: 60),
  }) async {
    await _channel.invokeMethod('setMasterSession', {
      'vehicleId': payload.vehicleId,
      'masterSecret': payload.masterSecret,
      'ttlSeconds': ttl.inSeconds,
    });
  }

  Future<void> clearHceSession() async {
    await _channel.invokeMethod('clearMasterSession');
  }

  Future<bool> isHceSessionActive() async {
    final result = await _channel.invokeMethod('isMasterSessionActive');
    return result == true;
  }

  String _extractPayloadText(List<NdefRecord> records) {
    if (records.isEmpty) {
      throw FormatException('Empty NDEF message');
    }

    // Prefer a well-known Text record when available
    for (final record in records) {
      if (record.typeNameFormat == NdefTypeNameFormat.nfcWellknown &&
          record.type.isNotEmpty &&
          String.fromCharCodes(record.type) == 'T') {
        return _decodeTextRecord(record);
      }
    }

    // Fallback: try to decode the first record payload as UTF-8
    final fallback = records.first.payload;
    if (fallback.isEmpty) {
      throw FormatException('Empty NDEF payload');
    }
    return utf8.decode(fallback);
  }

  String _decodeTextRecord(NdefRecord record) {
    final payload = record.payload;
    if (payload.isEmpty) {
      throw FormatException('Empty NDEF payload');
    }

    final status = payload.first;
    final langLen = status & 0x3F;
    final textStart = 1 + langLen;
    if (textStart > payload.length) {
      throw FormatException('Invalid NDEF text payload');
    }

    return utf8.decode(payload.sublist(textStart));
  }

  MasterCardPayload _parsePayload(String text) {
    String? vidHex;
    String? mskHex;

    try {
      final obj = jsonDecode(text);
      if (obj is Map) {
        vidHex = obj['vid']?.toString();
        mskHex = obj['msk']?.toString();
      }
    } catch (_) {
      // Fallback to CSV: vid,msk
      final parts = text.split(',');
      if (parts.length >= 2) {
        vidHex = parts[0].trim();
        mskHex = parts[1].trim();
      }
    }

    if (vidHex == null || mskHex == null) {
      throw FormatException('Master card payload missing vid/msk');
    }

    final vidBytes = _hexToBytes(vidHex);
    final mskBytes = _hexToBytes(mskHex);

    if (vidBytes.length != 8) {
      throw FormatException('Vehicle ID must be 8 bytes (16 hex chars)');
    }
    if (mskBytes.length != 32) {
      throw FormatException('Master Secret must be 32 bytes (64 hex chars)');
    }

    return MasterCardPayload(vehicleId: vidBytes, masterSecret: mskBytes);
  }

  Uint8List _hexToBytes(String hex) {
    final clean = hex.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
    if (clean.length.isOdd) {
      throw FormatException('Hex string must have even length');
    }
    final out = Uint8List(clean.length ~/ 2);
    for (var i = 0; i < clean.length; i += 2) {
      out[i ~/ 2] = int.parse(clean.substring(i, i + 2), radix: 16);
    }
    return out;
  }
}

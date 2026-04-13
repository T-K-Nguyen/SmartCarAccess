import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'gps_service.dart';
import 'pke_telemetry.dart';

const String kPkePreferredBleDeviceAddressKey = 'location_ble_device_address';

typedef PkeAuthProgressCallback = void Function(String step, String message);

class PhaseBResult {
  final bool success;
  final String message;
  final Uint8List? sharedSecret;
  final Uint8List? sessionEncKey;
  final Uint8List? sessionMacKey;
  final Uint8List? challenge;

  PhaseBResult({
    required this.success,
    required this.message,
    this.sharedSecret,
    this.sessionEncKey,
    this.sessionMacKey,
    this.challenge,
  });

  @override
  String toString() {
    return 'PhaseB_Result{success: $success, message: $message}';
  }
}

class PkeAuthOrchestrator {
  static const _handshakeChannel = MethodChannel('smartcar/phaseb/handshake');

  static const String authServiceUUID = '0000aaaa-1234-5678-9abc-def012345678';
  static const String charCccRxUUID = '0000aac1-1234-5678-9abc-def012345678';
  static const String charCccTxUUID = '0000aac2-1234-5678-9abc-def012345678';

  Uint8List? _phoneEphemeralPublicKey;
  Uint8List? _ecuEphemeralPublicKey;
  Uint8List? _sharedSecret;
  Uint8List? _sessionEncKey;
  Uint8List? _sessionMacKey;
  Uint8List? _challenge;

  BluetoothDevice? _device;
  BluetoothCharacteristic? _cCccRx;
  BluetoothCharacteristic? _cCccTx;

  StreamSubscription? _cccTxSubscription;
  final PkeTelemetry _telemetry;

  PkeAuthOrchestrator({PkeTelemetry? telemetry})
    : _telemetry = telemetry ?? PkeTelemetry(source: 'app');

  bool get hasActiveSession =>
      _sessionEncKey != null && _sessionMacKey != null && _device != null;
  Uint8List? get sessionEncKey => _sessionEncKey;
  Uint8List? get sessionMacKey => _sessionMacKey;

  static Future<void> savePreferredDeviceAddress(String deviceAddress) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(
      kPkePreferredBleDeviceAddressKey,
      deviceAddress.trim(),
    );
  }

  static Future<String?> loadPreferredDeviceAddress() async {
    final prefs = await SharedPreferences.getInstance();
    final value = prefs.getString(kPkePreferredBleDeviceAddressKey)?.trim();
    if (value == null || value.isEmpty) {
      return null;
    }
    return value;
  }

  Future<PhaseBResult> authenticatePreferredDevice({
    Duration timeout = const Duration(seconds: 30),
    PkeAuthProgressCallback? onProgress,
  }) async {
    final deviceAddress = await loadPreferredDeviceAddress();
    if (deviceAddress == null) {
      return PhaseBResult(
        success: false,
        message:
            'No preferred BLE device configured. Save ESP32 MAC in Location screen first.',
      );
    }

    return authenticate(
      deviceAddress: deviceAddress,
      timeout: timeout,
      onProgress: onProgress,
    );
  }

  Future<PhaseBResult> authenticate({
    required String deviceAddress,
    BluetoothDevice? device,
    Duration timeout = const Duration(seconds: 30),
    PkeAuthProgressCallback? onProgress,
  }) async {
    debugPrint('=== Phase B Authentication ===');
    debugPrint('Target device: $deviceAddress');
    _telemetry.startAttempt();

    void reportProgress(String step, String message) {
      debugPrint('[$step] $message');
      onProgress?.call(step, message);
    }

    try {
      const int insAuth0 = 0x80;
      const int insAuth1 = 0x81;
      const int insExchange = 0x82;
      const int insControlFlow = 0x83;

      reportProgress('Step 1', 'Connecting to BLE device...');

      if (device != null && device.isConnected) {
        _device = device;
      } else {
        final devices = FlutterBluePlus.connectedDevices;
        _device = devices.firstWhere(
          (d) => d.remoteId.str.toUpperCase() == deviceAddress.toUpperCase(),
          orElse: () => device ?? BluetoothDevice.fromId(deviceAddress),
        );
        if (_device!.isDisconnected) {
          await _device!.connect(timeout: timeout);
          await Future<void>.delayed(const Duration(milliseconds: 200));
        }
      }
      reportProgress('Step 1', 'Connected to ${_device!.platformName}');
      _telemetry.emit(
        event: PkeTelemetryEvent.connect,
        details: _device!.platformName,
      );

      try {
        final rssi = await _device!.readRssi();
        _telemetry.emit(
          event: PkeTelemetryEvent.rssiSnapshot,
          rssiDbm: rssi,
          details: 'post_connect',
        );
      } catch (_) {
        _telemetry.emit(
          event: PkeTelemetryEvent.rssiSnapshot,
          details: 'rssi_read_failed',
        );
      }

      try {
        final mtu = await _device!.requestMtu(512);
        reportProgress('Step 1.5', 'MTU negotiated: $mtu bytes');
      } catch (_) {
        reportProgress('Step 1.5', 'MTU request failed (using default)');
      }

      reportProgress('Step 2', 'Discovering BLE services...');
      final services = await _device!.discoverServices();
      final authService = services.firstWhere(
        (s) => s.uuid.toString().toLowerCase() == authServiceUUID.toLowerCase(),
        orElse: () => throw Exception('Auth service not found'),
      );

      _cCccRx = authService.characteristics.firstWhere(
        (c) => c.uuid.toString().toLowerCase() == charCccRxUUID.toLowerCase(),
      );
      _cCccTx = authService.characteristics.firstWhere(
        (c) => c.uuid.toString().toLowerCase() == charCccTxUUID.toLowerCase(),
      );
      reportProgress('Step 2', 'Found CCC RX/TX characteristics');

      final auth0RespCompleter = Completer<Uint8List>();
      final auth1CmdCompleter = Completer<Uint8List>();
      final exchangeChallengeCompleter = Completer<Uint8List>();
      final exchangeAckCompleter = Completer<void>();
      final controlAckCompleter = Completer<void>();

      _cccTxSubscription = _cCccTx!.lastValueStream.listen((value) {
        if (value.isEmpty) {
          return;
        }

        final frame = _parseTunnelFrame(Uint8List.fromList(value));
        if (frame == null) {
          debugPrint(
            '[Tunnel] Ignored malformed TX frame (${value.length} bytes)',
          );
          return;
        }

        final ins = frame.ins;
        debugPrint(
          '[Tunnel] RX INS=0x${ins.toRadixString(16)} SW=${frame.sw1.toRadixString(16)}${frame.sw2.toRadixString(16)} Lc=${frame.data.length}',
        );
        final swOk = frame.sw1 == 0x90 && frame.sw2 == 0x00;
        if (!swOk) {
          final err = Exception(
            'INS=0x${ins.toRadixString(16)} SW=${frame.sw1.toRadixString(16)}${frame.sw2.toRadixString(16)}',
          );
          if (!auth0RespCompleter.isCompleted) {
            auth0RespCompleter.completeError(err);
          }
          if (!auth1CmdCompleter.isCompleted) {
            auth1CmdCompleter.completeError(err);
          }
          if (!exchangeChallengeCompleter.isCompleted) {
            exchangeChallengeCompleter.completeError(err);
          }
          if (!exchangeAckCompleter.isCompleted) {
            exchangeAckCompleter.completeError(err);
          }
          if (!controlAckCompleter.isCompleted) {
            controlAckCompleter.completeError(err);
          }
          return;
        }

        if (ins == insAuth0 && !auth0RespCompleter.isCompleted) {
          auth0RespCompleter.complete(frame.data);
          return;
        }
        if (ins == insAuth1) {
          if (!auth1CmdCompleter.isCompleted) {
            auth1CmdCompleter.complete(frame.data);
          }
          if (!auth0RespCompleter.isCompleted && frame.data.length >= 65) {
            debugPrint('[Tunnel] AUTH0 fallback using AUTH1 payload');
            auth0RespCompleter.complete(frame.data);
          }
          return;
        }
        if (ins == insExchange) {
          if (frame.data.length == 24 &&
              !exchangeChallengeCompleter.isCompleted) {
            exchangeChallengeCompleter.complete(frame.data);
            return;
          }
          if (!exchangeAckCompleter.isCompleted) {
            exchangeAckCompleter.complete();
            return;
          }
        }
        if (ins == insControlFlow && !controlAckCompleter.isCompleted) {
          controlAckCompleter.complete();
        }
      });

      await _cCccTx!.setNotifyValue(true);
      reportProgress('Step 3', 'CCC_TX notifications enabled');

      reportProgress('Step 4', 'Sending AUTH0 (standard transaction)...');
      await _sendApdu(ins: insAuth0, p1: 0x11, data: Uint8List(0));

      final auth0Data = await auth0RespCompleter.future.timeout(
        const Duration(seconds: 10),
      );
      if (auth0Data.length < 65) {
        throw Exception('AUTH0 response too short: ${auth0Data.length}');
      }
      _ecuEphemeralPublicKey = auth0Data.sublist(0, 65);
      reportProgress('Step 4', 'AUTH0 response received');
      _telemetry.emit(event: PkeTelemetryEvent.auth0Received);

      try {
        final auth1Cmd = await auth1CmdCompleter.future.timeout(
          const Duration(seconds: 5),
        );
        if (auth1Cmd.length >= 65) {
          _ecuEphemeralPublicKey = auth1Cmd.sublist(0, 65);
        }
        reportProgress('Step 5', 'AUTH1 payload received from ECU');
      } catch (_) {
        reportProgress(
          'Step 5',
          'AUTH1 payload not received, using AUTH0 ephemeral',
        );
      }

      reportProgress('Step 6', 'Generating phone ephemeral keypair...');
      final keypairResult = await _handshakeChannel.invokeMethod(
        'generateEphemeralKeypair',
      );
      _phoneEphemeralPublicKey = Uint8List.fromList(
        List<int>.from(keypairResult['publicKey']),
      );

      reportProgress('Step 7', 'Signing phone ephemeral with identity key...');
      final signatureResult = await _handshakeChannel.invokeMethod(
        'signEphemeralWithIdentity',
        _phoneEphemeralPublicKey,
      );
      final signature = Uint8List.fromList(List<int>.from(signatureResult));

      final handshakePacket = BytesBuilder();
      handshakePacket.add(_phoneEphemeralPublicKey!);
      handshakePacket.addByte(signature.length & 0xFF);
      handshakePacket.addByte((signature.length >> 8) & 0xFF);
      handshakePacket.add(signature);

      reportProgress('Step 8', 'Sending AUTH1 response...');
      await _sendApdu(ins: insAuth1, p1: 0x00, data: handshakePacket.toBytes());
      _telemetry.emit(event: PkeTelemetryEvent.auth1Sent);

      reportProgress('Step 9', 'Computing ECDH shared secret...');
      final sharedSecretResult = await _handshakeChannel.invokeMethod(
        'computeECDH',
        {'ecuPublicKey': _ecuEphemeralPublicKey},
      );
      _sharedSecret = Uint8List.fromList(List<int>.from(sharedSecretResult));

      final keysResult = await _handshakeChannel
          .invokeMethod('deriveSessionKeys', {
            'sharedSecret': _sharedSecret,
            'phoneEphemeralPub': _phoneEphemeralPublicKey,
            'ecuEphemeralPub': _ecuEphemeralPublicKey,
          });
      _sessionEncKey = Uint8List.fromList(List<int>.from(keysResult['encKey']));
      _sessionMacKey = Uint8List.fromList(List<int>.from(keysResult['macKey']));
      reportProgress('Step 9', 'Session keys derived');

      reportProgress('Step 10', 'Waiting for EXCHANGE challenge...');
      _challenge = await exchangeChallengeCompleter.future.timeout(
        const Duration(seconds: 12),
      );
      if (_challenge!.length != 24) {
        throw Exception('Invalid challenge length: ${_challenge!.length}');
      }

      final challengeSigResult = await _handshakeChannel.invokeMethod(
        'signChallenge',
        _challenge,
      );
      final challengeSignature = Uint8List.fromList(
        List<int>.from(challengeSigResult),
      );
      await _sendApdu(ins: insExchange, p1: 0x00, data: challengeSignature);
      await exchangeAckCompleter.future.timeout(const Duration(seconds: 10));
      reportProgress('Step 10', 'EXCHANGE signature accepted');
      String? challengeVehicleIdHex;
      if (_challenge != null && _challenge!.length >= 8) {
        challengeVehicleIdHex = _challenge!
            .sublist(0, 8)
            .map((b) => b.toRadixString(16).padLeft(2, '0').toUpperCase())
            .join();
      }
      _telemetry.emit(
        event: PkeTelemetryEvent.authVerified,
        vehicleId: challengeVehicleIdHex,
      );

      await _sendApdu(
        ins: insControlFlow,
        p1: 0x00,
        data: Uint8List.fromList(utf8.encode('Success')),
      );
      await controlAckCompleter.future.timeout(const Duration(seconds: 6));
      reportProgress('Step 11', 'CONTROL FLOW acknowledged');
      _telemetry.emit(event: PkeTelemetryEvent.controlFlowAck);
      _telemetry.emit(
        event: PkeTelemetryEvent.unlockDecision,
        unlockDecision: 'allow',
        details: 'control_flow_ack_ok',
      );

      reportProgress(
        'Complete',
        'Authentication successful through CCC tunnel',
      );

      return PhaseBResult(
        success: true,
        message: 'Authentication successful',
        sharedSecret: _sharedSecret,
        sessionEncKey: _sessionEncKey,
        sessionMacKey: _sessionMacKey,
        challenge: _challenge,
      );
    } catch (e, stackTrace) {
      reportProgress('Error', 'Authentication failed: $e');
      debugPrint('Stack trace: $stackTrace');
      _telemetry.emit(
        event: PkeTelemetryEvent.unlockDecision,
        unlockDecision: 'deny',
        details: 'auth_exception',
      );

      return PhaseBResult(success: false, message: 'Authentication failed: $e');
    } finally {
      await _cccTxSubscription?.cancel();
    }
  }

  Future<void> disconnect() async {
    await _cccTxSubscription?.cancel();

    if (_device != null && _device!.isConnected) {
      await _device!.disconnect();
    }

    try {
      await _handshakeChannel.invokeMethod('resetEphemeralKeys');
    } catch (e) {
      debugPrint('Warning: Failed to reset ephemeral keys: $e');
    }

    _reset();
  }

  Future<bool> sendGpsLocation() async {
    debugPrint('=== Sending GPS Location ===');

    if (_sessionEncKey == null || _sessionMacKey == null) {
      debugPrint(
        '[GPS] ERROR: Session keys not available. Run Phase B authentication first.',
      );
      return false;
    }

    if (_cCccRx == null) {
      debugPrint(
        '[GPS] ERROR: CCC RX characteristic not available. Run Phase B first.',
      );
      return false;
    }

    try {
      final gpsService = GpsService();
      debugPrint('[GPS] Getting location data...');
      final gpsPacket = await gpsService.getEncryptedLocationData(
        _sessionEncKey!,
        _sessionMacKey!,
      );

      if (gpsPacket == null) {
        debugPrint('[GPS] Failed to get location data');
        return false;
      }

      return sendGpsPacket(gpsPacket);
    } catch (e) {
      debugPrint('[GPS] ERROR: $e');
      return false;
    }
  }

  Future<bool> sendGpsPacket(GpsDataPacket gpsPacket) async {
    const int insExchange = 0x82;

    if (_sessionEncKey == null || _sessionMacKey == null) {
      debugPrint(
        '[GPS] ERROR: Session keys not available. Run Phase B authentication first.',
      );
      return false;
    }

    if (_cCccRx == null) {
      debugPrint(
        '[GPS] ERROR: CCC RX characteristic not available. Run Phase B first.',
      );
      return false;
    }

    try {
      debugPrint('[GPS] Location: ${gpsPacket.locationString}');
      debugPrint(
        '[GPS] Encrypted packet size: ${gpsPacket.encryptedData.length} bytes',
      );
      await _sendApdu(
        ins: insExchange,
        p1: 0x20,
        data: gpsPacket.encryptedData,
      );
      debugPrint('[GPS] GPS packet sent on CCC tunnel');
      return true;
    } catch (e) {
      debugPrint('[GPS] ERROR: $e');
      return false;
    }
  }

  void _reset() {
    _phoneEphemeralPublicKey = null;
    _ecuEphemeralPublicKey = null;
    _sharedSecret = null;
    _sessionEncKey = null;
    _sessionMacKey = null;
    _challenge = null;
    _cCccRx = null;
    _cCccTx = null;
    _cccTxSubscription = null;
    _device = null;
  }

  Future<void> _sendApdu({
    required int ins,
    int cla = 0x00,
    int p1 = 0,
    int p2 = 0,
    Uint8List? data,
  }) async {
    if (_cCccRx == null) {
      throw Exception('CCC RX characteristic not initialized');
    }

    final payload = data ?? Uint8List(0);
    if (payload.length > 255) {
      throw Exception('APDU payload too large (${payload.length} > 255)');
    }

    final frame = BytesBuilder();
    frame.addByte(cla & 0xFF);
    frame.addByte(ins & 0xFF);
    frame.addByte(p1 & 0xFF);
    frame.addByte(p2 & 0xFF);
    frame.addByte(payload.length & 0xFF);
    frame.add(payload);
    await _cCccRx!.write(frame.toBytes(), withoutResponse: false);
  }

  _TunnelFrame? _parseTunnelFrame(Uint8List raw) {
    if (raw.length < 4) {
      return null;
    }

    final ins = raw[0];
    final sw1 = raw[1];
    final sw2 = raw[2];
    final lc = raw[3];
    if (raw.length < 4 + lc) {
      return null;
    }

    final data = lc > 0 ? raw.sublist(4, 4 + lc) : Uint8List(0);
    return _TunnelFrame(ins: ins, sw1: sw1, sw2: sw2, data: data);
  }
}

class _TunnelFrame {
  final int ins;
  final int sw1;
  final int sw2;
  final Uint8List data;

  _TunnelFrame({
    required this.ins,
    required this.sw1,
    required this.sw2,
    required this.data,
  });
}

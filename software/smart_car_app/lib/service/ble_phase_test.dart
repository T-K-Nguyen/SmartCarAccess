import 'dart:async';
import 'dart:typed_data';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'dart:convert';
import 'gps_service.dart';

/// Callback for reporting progress during authentication
typedef ProgressCallback = void Function(String step, String message);

/// BLE Phase Test Service
/// 
/// This service provides testing utilities for:
/// - Phase A: NFC Provisioning (Android HCE to ESP32 NFC reader)
/// - Phase B: BLE Authentication (Android to ESP32 BLE peripheral)
///
/// Uses Android Keystore via MethodChannel for all cryptographic operations.
///
/// Usage:
/// ```dart
/// final tester = BlePhaseTestService();
/// await tester.testPhaseB(
///   deviceAddress: "XX:XX:XX:XX:XX:XX",
///   onProgress: (step, msg) => print('$step: $msg'),
/// );
/// ```
class BlePhaseTestService {
  // Method channels for Android Keystore integration
  static const _handshakeChannel = MethodChannel('smartcar/phaseb/handshake');
  static const _keystoreChannel = MethodChannel('smartcar/keystore');
  // Phase B Authentication Service UUIDs
  static const String authServiceUUID = "0000aaaa-1234-5678-9abc-def012345678";
  static const String charHandshakeWriteUUID = "0000aaab-1234-5678-9abc-def012345678";
  static const String charHandshakeReadUUID = "0000aaac-1234-5678-9abc-def012345678";
  static const String charStatusUUID = "0000aaad-1234-5678-9abc-def012345678";
  static const String charChallengeReadUUID = "0000aaae-1234-5678-9abc-def012345678";
  static const String charChallengeWriteUUID = "0000aaaf-1234-5678-9abc-def012345678";
  static const String charGpsDataUUID = "0000aab0-1234-5678-9abc-def012345678";

  // State machine for Phase B
  PhaseB_State _stateBLE = PhaseB_State.PHONE_IDLE;
  
  // Keys and data (stored as raw bytes from Android)
  Uint8List? _phoneEphemeralPublicKey;
  Uint8List? _phoneEphemeralPrivateKey;
  Uint8List? _ecuEphemeralPublicKey;
  Uint8List? _sharedSecret;
  Uint8List? _sessionEncKey;
  Uint8List? _sessionMacKey;
  Uint8List? _challenge;
  
  // BLE connection
  BluetoothDevice? _device;
  BluetoothCharacteristic? _cHandshakeRead;
  BluetoothCharacteristic? _cHandshakeWrite;
  BluetoothCharacteristic? _cStatus;
  BluetoothCharacteristic? _cChallengeRead;
  BluetoothCharacteristic? _cChallengeWrite;
  BluetoothCharacteristic? _cGpsData;
  
  StreamSubscription? _handshakeSubscription;
  StreamSubscription? _statusSubscription;
  StreamSubscription? _challengeSubscription;

  bool get hasActiveSession =>
      _sessionEncKey != null && _sessionMacKey != null && _device != null;
  Uint8List? get sessionEncKey => _sessionEncKey;
  Uint8List? get sessionMacKey => _sessionMacKey;

  /// Test Phase A: NFC Provisioning
  /// 
  /// This demonstrates what happens during NFC provisioning:
  /// 1. Phone (Android HCE) emulates a card with AID F0:01:02:03:04:05
  /// 2. ECU (ESP32 NFC reader) performs:
  ///    a. SELECT AID → receives 4-byte UID + 9000
  ///    b. GET_CHALLENGE (Lc=0) → receives [keyId(1) + phonePub(65) + certLen(2)=0 + 9000]
  ///    c. Generates challenge (vehicleId[8] + nonce[16] = 24 bytes)
  ///    d. GET_CHALLENGE (Lc=24, data=challenge) → receives [sigLen(2,BE) + DER_sig + 9000]
  /// 3. ECU verifies signature and stores phone public key
  ///
  /// Android implementation: ProvisioningHostApduService.kt
  /// ESP32 implementation: provisioning_phase.cpp
  Future<PhaseA_Result> testPhaseAInfo() async {
    debugPrint('=== Phase A: NFC Provisioning Test ===');
    
    // This is informational only - actual NFC provisioning happens via HCE
    debugPrint('Phase A is handled by native Android HCE service:');
    debugPrint('  Service: ProvisioningHostApduService');
    debugPrint('  AID: F0:01:02:03:04:05');
    debugPrint('');
    debugPrint('Provisioning Flow:');
    debugPrint('  1. ECU sends: 00 A4 04 00 06 F0 01 02 03 04 05 (SELECT AID)');
    debugPrint('     Phone responds: [UID(4)] 90 00');
    debugPrint('');
    debugPrint('  2. ECU sends: 00 CA 00 00 00 (GET_CHALLENGE, Lc=0)');
    debugPrint('     Phone responds: [keyId(1) + phonePub(65) + certLen(2)=0000] 90 00');
    debugPrint('');
    debugPrint('  3. ECU generates challenge: vehicleId(8) || nonce(16)');
    debugPrint('     ECU sends: 00 CA 00 00 18 [challenge(24)] (GET_CHALLENGE, Lc=24)');
    debugPrint('     Phone responds: [sigLen(2,BE) + DER_signature] 90 00');
    debugPrint('');
    debugPrint('  4. ECU verifies signature with phone public key and stores it');
    debugPrint('');
    debugPrint('Result: ECU is now provisioned with phone\'s public key');
    debugPrint('        Ready for Phase B (BLE authentication)');
    
    return PhaseA_Result(
      success: true,
      message: 'Phase A is handled by native HCE. See ProvisioningHostApduService.kt',
      phonePublicKeyStored: true,
    );
  }

  /// Test Phase B: BLE Authentication
  /// 
  /// This performs the full BLE authentication sequence:
  /// 1. Phone connects to ECU BLE peripheral
  /// 2. ECU generates ephemeral keypair and sends ECU handshake (no signature)
  /// 3. Phone receives ECU handshake (skips verification for now)
  /// 4. Phone generates ephemeral keypair
  /// 5. Phone signs ephemeral public key with identity key
  /// 6. Phone sends handshake to ECU
  /// 7. ECU verifies phone signature with stored public key
  /// 8. Both sides compute ECDH shared secret
  /// 9. Derive session keys with HKDF-SHA256
  /// 10. ECU sends challenge for final verification
  /// 11. Phone signs challenge and sends back
  /// 12. ECU verifies → AUTH_SUCCESS
  Future<PhaseB_Result> testPhaseB({
    required String deviceAddress,
    BluetoothDevice? device,
    Duration timeout = const Duration(seconds: 30),
    ProgressCallback? onProgress,
  }) async {
    debugPrint('=== Phase B: BLE Authentication Test ===');
    debugPrint('Target device: $deviceAddress');
    
    void reportProgress(String step, String message) {
      debugPrint('[$step] $message');
      onProgress?.call(step, message);
    }
    
    try {
      // Step 1: Connect to device
      _stateBLE = PhaseB_State.PHONE_CONNECTING;
      reportProgress('Step 1', 'Connecting to BLE device...');
      
      if (device != null && device.isConnected) {
        _device = device;
        reportProgress('Step 1', '✓ Using existing connection');
      } else {
        final devices = await FlutterBluePlus.connectedDevices;
        _device = devices.firstWhere(
          (d) => d.remoteId.str.toUpperCase() == deviceAddress.toUpperCase(),
          orElse: () => device ?? BluetoothDevice.fromId(deviceAddress),
        );
        
        if (_device!.isDisconnected) {
          await _device!.connect(timeout: timeout);
          await Future.delayed(const Duration(milliseconds: 200));
        }
        reportProgress('Step 1', '✓ Connected to ${_device!.platformName}');
      }
      
      // Step 1.5: Request larger MTU for better throughput
      reportProgress('Step 1.5', 'Requesting MTU increase...');
      try {
        final mtu = await _device!.requestMtu(512);
        reportProgress('Step 1.5', '✓ MTU negotiated: $mtu bytes');
      } catch (e) {
        reportProgress('Step 1.5', '⚠ MTU request failed (using default)');
      }
      
      // Step 2: Discover services
      reportProgress('Step 2', 'Discovering BLE services...');
      final services = await _device!.discoverServices();
      reportProgress('Step 2', '✓ Found ${services.length} services');
      
      final authService = services.firstWhere(
        (s) => s.uuid.toString().toLowerCase() == authServiceUUID.toLowerCase(),
        orElse: () => throw Exception('Auth service not found'),
      );
      
      reportProgress('Step 2', '✓ Found auth service');
      
      // Get characteristics
      _cHandshakeRead = authService.characteristics.firstWhere(
        (c) => c.uuid.toString().toLowerCase() == charHandshakeReadUUID.toLowerCase(),
      );
      _cHandshakeWrite = authService.characteristics.firstWhere(
        (c) => c.uuid.toString().toLowerCase() == charHandshakeWriteUUID.toLowerCase(),
      );
      _cStatus = authService.characteristics.firstWhere(
        (c) => c.uuid.toString().toLowerCase() == charStatusUUID.toLowerCase(),
      );
      _cChallengeRead = authService.characteristics.firstWhere(
        (c) => c.uuid.toString().toLowerCase() == charChallengeReadUUID.toLowerCase(),
      );
      _cChallengeWrite = authService.characteristics.firstWhere(
        (c) => c.uuid.toString().toLowerCase() == charChallengeWriteUUID.toLowerCase(),
      );
      
      // GPS data characteristic (optional, may not be available on older firmware)
      try {
        _cGpsData = authService.characteristics.firstWhere(
          (c) => c.uuid.toString().toLowerCase() == charGpsDataUUID.toLowerCase(),
        );
        reportProgress('Step 2', '✓ GPS data characteristic found');
      } catch (e) {
        reportProgress('Step 2', '⚠ GPS characteristic not available (older firmware)');
      }

      reportProgress('Step 2', '✓ All characteristics found');
      
      // Step 3: Subscribe to notifications
      reportProgress('Step 3', 'Subscribing to notifications...');
      
      _stateBLE = PhaseB_State.PHONE_WAITING_FOR_ECU_HANDSHAKE;
      
      // Set up listeners BEFORE enabling notifications
      final handshakeCompleter = Completer<Uint8List>();
      final statusCompleter = Completer<String>();
      final challengeCompleter = Completer<Uint8List>();
      
      _handshakeSubscription = _cHandshakeRead!.lastValueStream.listen((value) {
        if (value.isNotEmpty && !handshakeCompleter.isCompleted) {
          debugPrint('[Notification] Received ECU handshake: ${value.length} bytes');
          handshakeCompleter.complete(Uint8List.fromList(value));
        }
      });
      
      _statusSubscription = _cStatus!.lastValueStream.listen((value) {
        if (value.isNotEmpty) {
          final status = utf8.decode(value);
          debugPrint('[Notification] Status: $status');
          if (!statusCompleter.isCompleted && 
              (status == 'AUTH_SUCCESS' || status == 'AUTH_FAILED')) {
            statusCompleter.complete(status);
          }
        }
      });
      
      _challengeSubscription = _cChallengeRead!.lastValueStream.listen((value) {
        if (value.isNotEmpty && !challengeCompleter.isCompleted) {
          debugPrint('[Notification] Received challenge: ${value.length} bytes');
          challengeCompleter.complete(Uint8List.fromList(value));
        }
      });
      
      // Now enable notifications
      await _cHandshakeRead!.setNotifyValue(true);
      await _cStatus!.setNotifyValue(true);
      await _cChallengeRead!.setNotifyValue(true);
      
      reportProgress('Step 3', '✓ Notifications enabled');
      
      // Check if handshake was already sent (cached value)
      // ESP32 may have sent handshake during connection, before notifications enabled
      final cachedHandshake = await _cHandshakeRead!.read();
      if (cachedHandshake.isNotEmpty && !handshakeCompleter.isCompleted) {
        reportProgress('Step 3', '✓ Found cached handshake');
        handshakeCompleter.complete(Uint8List.fromList(cachedHandshake));
      }
      
      // Step 4: Wait for ECU handshake
      reportProgress('Step 4', 'Waiting for ECU handshake...');
      final ecuHandshake = await handshakeCompleter.future
          .timeout(Duration(seconds: 10));
      
      // Parse ECU handshake: [ephemeral_pub(65) + sig_len(2) + signature]
      if (ecuHandshake.length < 67) {
        throw Exception('ECU handshake too short: ${ecuHandshake.length} bytes');
      }
      
      _ecuEphemeralPublicKey = ecuHandshake.sublist(0, 65);
      final sigLen = ecuHandshake[65] | (ecuHandshake[66] << 8);
      
      reportProgress('Step 4', '✓ ECU handshake received ($sigLen byte signature)');
      
      // Step 5: Generate phone ephemeral keypair (via Android)
      _stateBLE = PhaseB_State.PHONE_EPHEMERAL_GENERATING;
      reportProgress('Step 5', 'Generating phone ephemeral keypair...');
      
      final keypairResult = await _handshakeChannel.invokeMethod('generateEphemeralKeypair');
      _phoneEphemeralPublicKey = Uint8List.fromList(List<int>.from(keypairResult['publicKey']));
      _phoneEphemeralPrivateKey = Uint8List.fromList(List<int>.from(keypairResult['privateKey']));
      
      reportProgress('Step 5', '✓ Phone keypair generated');
      
      // Step 6: Sign ephemeral public key with phone identity key (Android Keystore)
      _stateBLE = PhaseB_State.PHONE_SIGNING_EPHEMERAL;
      reportProgress('Step 6', 'Signing with identity key...');
      
      final signatureResult = await _handshakeChannel.invokeMethod(
        'signEphemeralWithIdentity',
        _phoneEphemeralPublicKey,
      );
      final signature = Uint8List.fromList(List<int>.from(signatureResult));
      
      reportProgress('Step 6', '✓ Signature generated (${signature.length} bytes)');
      
      // Step 7: Send phone handshake to ECU
      _stateBLE = PhaseB_State.PHONE_SENT_HANDSHAKE;
      reportProgress('Step 7', 'Sending phone handshake...');
      
      // Build packet: [ephemeral_pub(65) + sig_len(2,LE) + signature]
      final handshakePacket = BytesBuilder();
      handshakePacket.add(_phoneEphemeralPublicKey!);
      handshakePacket.addByte(signature.length & 0xFF);
      handshakePacket.addByte((signature.length >> 8) & 0xFF);
      handshakePacket.add(signature);
      
      await _cHandshakeWrite!.write(handshakePacket.toBytes(), withoutResponse: false);
      reportProgress('Step 7', '✓ Phone handshake sent');
      
      // Step 8: Wait for authentication status
      _stateBLE = PhaseB_State.PHONE_WAITING_FOR_STATUS;
      reportProgress('Step 8', 'Waiting for ECU verification...');
      
      final status = await statusCompleter.future
          .timeout(Duration(seconds: 10));
      
      if (status != 'AUTH_SUCCESS') {
        throw Exception('Authentication failed: $status');
      }
      
      reportProgress('Step 8', '✓ ECU verified phone');
      
      // Step 9: Compute ECDH shared secret (via Android)
      reportProgress('Step 9', 'Computing ECDH shared secret...');
      
      final sharedSecretResult = await _handshakeChannel.invokeMethod(
        'computeECDH',
        {'ecuPublicKey': _ecuEphemeralPublicKey},
      );
      _sharedSecret = Uint8List.fromList(List<int>.from(sharedSecretResult));
      
      reportProgress('Step 9', '✓ Shared secret computed');
      
      // Step 10: Derive session keys with HKDF-SHA256 (via Android)
      reportProgress('Step 10', 'Deriving session keys...');
      
      final keysResult = await _handshakeChannel.invokeMethod(
        'deriveSessionKeys',
        {
          'sharedSecret': _sharedSecret,
          'phoneEphemeralPub': _phoneEphemeralPublicKey,
          'ecuEphemeralPub': _ecuEphemeralPublicKey,
        },
      );
      
      _sessionEncKey = Uint8List.fromList(List<int>.from(keysResult['encKey']));
      _sessionMacKey = Uint8List.fromList(List<int>.from(keysResult['macKey']));
      
      reportProgress('Step 10', '✓ Session keys derived');
      
      // Step 11: Wait for challenge
      reportProgress('Step 11', 'Waiting for challenge...');
      
      _challenge = await challengeCompleter.future
          .timeout(Duration(seconds: 10));
      
      if (_challenge!.length != 24) {
        throw Exception('Invalid challenge length: ${_challenge!.length}');
      }
      
      final vehicleId = _challenge!.sublist(0, 8);
      final nonce = _challenge!.sublist(8, 24);
      
      reportProgress('Step 11', '✓ Challenge received');
      
      // Step 12: Sign challenge and send back (via Android Keystore)
      reportProgress('Step 12', 'Signing challenge...');
      
      final challengeSigResult = await _handshakeChannel.invokeMethod(
        'signChallenge',
        _challenge,
      );
      final challengeSignature = Uint8List.fromList(List<int>.from(challengeSigResult));
      
      await _cChallengeWrite!.write(challengeSignature, withoutResponse: false);
      reportProgress('Step 12', '✓ Challenge signature sent');
      
      _stateBLE = PhaseB_State.PHONE_AUTH_SUCCESS;
      reportProgress('Complete', '✓✓✓ Authentication successful!');
      
      return PhaseB_Result(
        success: true,
        message: 'Authentication successful',
        sharedSecret: _sharedSecret,
        sessionEncKey: _sessionEncKey,
        sessionMacKey: _sessionMacKey,
        challenge: _challenge,
      );
      
    } catch (e, stackTrace) {
      reportProgress('Error', '✗ Authentication failed: $e');
      debugPrint('Stack trace: $stackTrace');
      
      _stateBLE = PhaseB_State.PHONE_AUTH_FAILED;
      
      return PhaseB_Result(
        success: false,
        message: 'Authentication failed: $e',
      );
    } finally {
      // Cleanup subscriptions
      await _handshakeSubscription?.cancel();
      await _statusSubscription?.cancel();
      await _challengeSubscription?.cancel();
    }
  }

  /// Cleanup and disconnect
  Future<void> disconnect() async {
    await _handshakeSubscription?.cancel();
    await _statusSubscription?.cancel();
    await _challengeSubscription?.cancel();
    
    if (_device != null && _device!.isConnected) {
      await _device!.disconnect();
    }
    
    // Reset ephemeral keys on Android side
    try {
      await _handshakeChannel.invokeMethod('resetEphemeralKeys');
    } catch (e) {
      debugPrint('Warning: Failed to reset ephemeral keys: $e');
    }
    
    _reset();
  }

  /// Send encrypted GPS location data to ESP32
  /// 
  /// This method:
  /// 1. Gets current GPS location using GpsService
  /// 2. Encrypts and authenticates the location data
  /// 3. Sends encrypted packet via BLE to ESP32
  /// 
  /// Must be called after successful Phase B authentication (session keys ready)
  Future<bool> sendGpsLocation() async {
    debugPrint('\n=== Sending GPS Location ===');
    
    // Check if we have session keys
    if (_sessionEncKey == null || _sessionMacKey == null) {
      debugPrint('[GPS] ERROR: Session keys not available. Run Phase B authentication first.');
      return false;
    }
    
    // Check if GPS characteristic is available
    if (_cGpsData == null) {
      debugPrint('[GPS] ERROR: GPS characteristic not found. Discovering...');
      
      if (_device == null) {
        debugPrint('[GPS] ERROR: No device connected');
        return false;
      }
      
      try {
        final services = await _device!.discoverServices();
        final authService = services.firstWhere(
          (s) => s.uuid.toString().toLowerCase() == authServiceUUID.toLowerCase(),
        );
        
        _cGpsData = authService.characteristics.firstWhere(
          (c) => c.uuid.toString().toLowerCase() == charGpsDataUUID.toLowerCase(),
        );
        
        debugPrint('[GPS] ✓ GPS characteristic found');
      } catch (e) {
        debugPrint('[GPS] ERROR: Failed to find GPS characteristic: $e');
        return false;
      }
    }
    
    try {
      // Import GPS service
      final gpsService = GpsService();
      
      // Get encrypted location data
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
    if (_sessionEncKey == null || _sessionMacKey == null) {
      debugPrint('[GPS] ERROR: Session keys not available. Run Phase B authentication first.');
      return false;
    }

    if (_cGpsData == null) {
      debugPrint('[GPS] ERROR: GPS characteristic not found. Discovering...');

      if (_device == null) {
        debugPrint('[GPS] ERROR: No device connected');
        return false;
      }

      try {
        final services = await _device!.discoverServices();
        final authService = services.firstWhere(
          (s) => s.uuid.toString().toLowerCase() == authServiceUUID.toLowerCase(),
        );

        _cGpsData = authService.characteristics.firstWhere(
          (c) => c.uuid.toString().toLowerCase() == charGpsDataUUID.toLowerCase(),
        );

        debugPrint('[GPS] ✓ GPS characteristic found');
      } catch (e) {
        debugPrint('[GPS] ERROR: Failed to find GPS characteristic: $e');
        return false;
      }
    }

    try {
      debugPrint('[GPS] Location: ${gpsPacket.locationString}');
      debugPrint('[GPS] Encrypted packet size: ${gpsPacket.encryptedData.length} bytes');
      await _cGpsData!.write(gpsPacket.encryptedData.toList(), withoutResponse: false);
      debugPrint('[GPS] ✓ GPS data sent successfully to ESP32');
      return true;
    } catch (e) {
      debugPrint('[GPS] ERROR: $e');
      return false;
    }
  }

  void _reset() {
    _stateBLE = PhaseB_State.PHONE_IDLE;
    _phoneEphemeralPublicKey = null;
    _phoneEphemeralPrivateKey = null;
    _ecuEphemeralPublicKey = null;
    _sharedSecret = null;
    _sessionEncKey = null;
    _sessionMacKey = null;
    _challenge = null;
    _device = null;
  }

  // ========== Helper Functions ==========

  /// Convert bytes to hex string
  String _bytesToHex(Uint8List bytes) {
    return bytes.map((b) => b.toRadixString(16).padLeft(2, '0').toUpperCase()).join(' ');
  }
}

// ========== State Machines ==========

enum PhaseB_State {
  PHONE_IDLE,
  PHONE_CONNECTING,
  PHONE_WAITING_FOR_ECU_HANDSHAKE,
  PHONE_VERIFY_ECU_SIGNATURE,
  PHONE_EPHEMERAL_GENERATING,
  PHONE_SIGNING_EPHEMERAL,
  PHONE_SENT_HANDSHAKE,
  PHONE_WAITING_FOR_STATUS,
  PHONE_AUTH_SUCCESS,
  PHONE_AUTH_FAILED,
}

// ========== Result Classes ==========

class PhaseA_Result {
  final bool success;
  final String message;
  final bool phonePublicKeyStored;

  PhaseA_Result({
    required this.success,
    required this.message,
    required this.phonePublicKeyStored,
  });

  @override
  String toString() {
    return 'PhaseA_Result{success: $success, message: $message, phonePublicKeyStored: $phonePublicKeyStored}';
  }
}

class PhaseB_Result {
  final bool success;
  final String message;
  final Uint8List? sharedSecret;
  final Uint8List? sessionEncKey;
  final Uint8List? sessionMacKey;
  final Uint8List? challenge;

  PhaseB_Result({
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



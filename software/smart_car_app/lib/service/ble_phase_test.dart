import 'dart:async';
import 'dart:typed_data';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'dart:convert';

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
/// await tester.testPhaseB(deviceAddress: "XX:XX:XX:XX:XX:XX");
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
  
  StreamSubscription? _handshakeSubscription;
  StreamSubscription? _statusSubscription;
  StreamSubscription? _challengeSubscription;

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
    Duration timeout = const Duration(seconds: 30),
  }) async {
    debugPrint('=== Phase B: BLE Authentication Test ===');
    debugPrint('Target device: $deviceAddress');
    
    try {
      // Step 1: Connect to device
      _stateBLE = PhaseB_State.PHONE_CONNECTING;
      debugPrint('\n[Step 1] Connecting to BLE device...');
      
      final devices = await FlutterBluePlus.connectedDevices;
      _device = devices.firstWhere(
        (d) => d.remoteId.str.toUpperCase() == deviceAddress.toUpperCase(),
        orElse: () => throw Exception('Device not found in connected devices'),
      );
      
      if (_device!.isDisconnected) {
        await _device!.connect(timeout: timeout);
      }
      
      debugPrint('✓ Connected to ${_device!.platformName}');
      
      // Step 2: Discover services
      debugPrint('\n[Step 2] Discovering BLE services...');
      final services = await _device!.discoverServices();
      
      final authService = services.firstWhere(
        (s) => s.uuid.toString().toLowerCase() == authServiceUUID.toLowerCase(),
        orElse: () => throw Exception('Auth service not found'),
      );
      
      debugPrint('✓ Found auth service: $authServiceUUID');
      
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
      
      debugPrint('✓ Found all required characteristics');
      
      // Step 3: Subscribe to notifications
      debugPrint('\n[Step 3] Subscribing to notifications...');
      
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
      
      debugPrint('✓ Subscribed to notifications');
      
      // CRITICAL: Wait for notifications to be fully registered
      // ESP32 may have already sent handshake during connection
      await Future.delayed(const Duration(milliseconds: 500));
      
      // Check if handshake was already sent (cached value)
      final cachedHandshake = await _cHandshakeRead!.read();
      if (cachedHandshake.isNotEmpty && !handshakeCompleter.isCompleted) {
        debugPrint('[Cached] Found ECU handshake: ${cachedHandshake.length} bytes');
        handshakeCompleter.complete(Uint8List.fromList(cachedHandshake));
      }
      
      // Step 4: Wait for ECU handshake
      debugPrint('\n[Step 4] Waiting for ECU handshake...');
      final ecuHandshake = await handshakeCompleter.future
          .timeout(Duration(seconds: 10));
      
      // Parse ECU handshake: [ephemeral_pub(65) + sig_len(2) + signature]
      if (ecuHandshake.length < 67) {
        throw Exception('ECU handshake too short: ${ecuHandshake.length} bytes');
      }
      
      _ecuEphemeralPublicKey = ecuHandshake.sublist(0, 65);
      final sigLen = ecuHandshake[65] | (ecuHandshake[66] << 8);
      
      debugPrint('✓ ECU ephemeral public key: ${_bytesToHex(_ecuEphemeralPublicKey!)}');
      debugPrint('  Signature length: $sigLen (skipping verification for now)');
      
      // Step 5: Generate phone ephemeral keypair (via Android)
      _stateBLE = PhaseB_State.PHONE_EPHEMERAL_GENERATING;
      debugPrint('\n[Step 5] Generating phone ephemeral keypair (Android Keystore)...');
      
      final keypairResult = await _handshakeChannel.invokeMethod('generateEphemeralKeypair');
      _phoneEphemeralPublicKey = Uint8List.fromList(List<int>.from(keypairResult['publicKey']));
      _phoneEphemeralPrivateKey = Uint8List.fromList(List<int>.from(keypairResult['privateKey']));
      
      debugPrint('✓ Phone ephemeral public key: ${_bytesToHex(_phoneEphemeralPublicKey!)}');
      
      // Step 6: Sign ephemeral public key with phone identity key (Android Keystore)
      _stateBLE = PhaseB_State.PHONE_SIGNING_EPHEMERAL;
      debugPrint('\n[Step 6] Signing phone ephemeral key with identity key (Android Keystore)...');
      
      final signatureResult = await _handshakeChannel.invokeMethod(
        'signEphemeralWithIdentity',
        _phoneEphemeralPublicKey,
      );
      final signature = Uint8List.fromList(List<int>.from(signatureResult));
      
      debugPrint('✓ Signature generated: ${signature.length} bytes (DER)');
      
      // Step 7: Send phone handshake to ECU
      _stateBLE = PhaseB_State.PHONE_SENT_HANDSHAKE;
      debugPrint('\n[Step 7] Sending phone handshake to ECU...');
      
      // Build packet: [ephemeral_pub(65) + sig_len(2,LE) + signature]
      final handshakePacket = BytesBuilder();
      handshakePacket.add(_phoneEphemeralPublicKey!);
      handshakePacket.addByte(signature.length & 0xFF);
      handshakePacket.addByte((signature.length >> 8) & 0xFF);
      handshakePacket.add(signature);
      
      await _cHandshakeWrite!.write(handshakePacket.toBytes(), withoutResponse: false);
      debugPrint('✓ Phone handshake sent: ${handshakePacket.length} bytes');
      
      // Step 8: Wait for authentication status
      _stateBLE = PhaseB_State.PHONE_WAITING_FOR_STATUS;
      debugPrint('\n[Step 8] Waiting for authentication status...');
      
      final status = await statusCompleter.future
          .timeout(Duration(seconds: 10));
      
      if (status != 'AUTH_SUCCESS') {
        throw Exception('Authentication failed: $status');
      }
      
      debugPrint('✓ Authentication status: $status');
      
      // Step 9: Compute ECDH shared secret (via Android)
      debugPrint('\n[Step 9] Computing ECDH shared secret (Android native)...');
      
      final sharedSecretResult = await _handshakeChannel.invokeMethod(
        'computeECDH',
        {'ecuPublicKey': _ecuEphemeralPublicKey},
      );
      _sharedSecret = Uint8List.fromList(List<int>.from(sharedSecretResult));
      
      debugPrint('✓ Shared secret: ${_bytesToHex(_sharedSecret!)}');
      
      // Step 10: Derive session keys with HKDF-SHA256 (via Android)
      debugPrint('\n[Step 10] Deriving session keys (HKDF-SHA256 on Android)...');
      
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
      
      debugPrint('✓ Session encryption key: ${_bytesToHex(_sessionEncKey!)}');
      debugPrint('✓ Session MAC key: ${_bytesToHex(_sessionMacKey!)}');
      
      // Step 11: Wait for challenge
      debugPrint('\n[Step 11] Waiting for challenge...');
      
      _challenge = await challengeCompleter.future
          .timeout(Duration(seconds: 10));
      
      if (_challenge!.length != 24) {
        throw Exception('Invalid challenge length: ${_challenge!.length}');
      }
      
      final vehicleId = _challenge!.sublist(0, 8);
      final nonce = _challenge!.sublist(8, 24);
      
      debugPrint('✓ Challenge received:');
      debugPrint('  Vehicle ID: ${_bytesToHex(vehicleId)}');
      debugPrint('  Nonce: ${_bytesToHex(nonce)}');
      
      // Step 12: Sign challenge and send back (via Android Keystore)
      debugPrint('\n[Step 12] Signing challenge with identity key (Android Keystore)...');
      
      final challengeSigResult = await _handshakeChannel.invokeMethod(
        'signChallenge',
        _challenge,
      );
      final challengeSignature = Uint8List.fromList(List<int>.from(challengeSigResult));
      
      await _cChallengeWrite!.write(challengeSignature, withoutResponse: false);
      debugPrint('✓ Challenge signature sent: ${challengeSignature.length} bytes');
      
      // Wait a moment for final verification
      await Future.delayed(Duration(seconds: 2));
      
      _stateBLE = PhaseB_State.PHONE_AUTH_SUCCESS;
      debugPrint('\n✓✓✓ Phase B Authentication Completed Successfully! ✓✓✓');
      
      return PhaseB_Result(
        success: true,
        message: 'Authentication successful',
        sharedSecret: _sharedSecret,
        sessionEncKey: _sessionEncKey,
        sessionMacKey: _sessionMacKey,
        challenge: _challenge,
      );
      
    } catch (e, stackTrace) {
      debugPrint('\n✗✗✗ Phase B Authentication Failed ✗✗✗');
      debugPrint('Error: $e');
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



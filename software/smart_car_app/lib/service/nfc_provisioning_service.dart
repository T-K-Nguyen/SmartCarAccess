import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:cryptography/cryptography.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';

/// NfcProvisioningService
///
/// Phase A: NFC Provisioning handshake between Phone and Car ECU (ESP32-S3 + PN532).
///
/// HCE Flow (phone acts as NFC card target):
/// - Android Host Card Emulation (HCE) service receives APDUs from ECU reader.
/// - HCE service calls into Flutter via MethodChannel("smartcar.hce", ...):
///   getProvisioningData -> returns { publicKey, certChain, keyId }.
/// - HCE service builds R-APDU = data || 0x9000.
///
/// Notes:
/// - Requires dependencies: cryptography, flutter_secure_storage.
/// - On Android, ensure NFC permissions are added by the plugin; test on NFC-capable device.
/// - Owner credentials are loaded from secure storage. If absent, the provisioning will fail
///   by default (to avoid insecure behavior). A dev-only helper ensureOwnerKeysExist() is
///   provided to generate/store keys during testing if needed. The Manufacturer CA certificate
///   is expected to be provisioned separately (if required by ECU).
///
/// Usage (e.g., from a button handler):
///
///   final service = NfcProvisioningService(ownerId: currentUser.email!);
///   final ok = await service.startProvisioning(onLog: (m) => debugPrint(m));
///   if (ok) { /* show success */ } else { /* show error */ }
class NfcProvisioningService {
  NfcProvisioningService({
    required this.ownerId,
    FlutterSecureStorage? storage,
  }) : storage = storage ?? const FlutterSecureStorage();

  final String ownerId; // e.g., user@example.com
  final FlutterSecureStorage storage;

  // ECDSA P-256 with SHA-256 as required
  final Ecdsa _ecdsa = Ecdsa.p256(Sha256());

  // MethodChannel for HCE interaction
  static const String _channelName = 'smartcar.hce';
  static final MethodChannel _channel = MethodChannel(_channelName);
  static bool _initialized = false;
  static Map<String, dynamic>? _cachedProvisioningData;

  /// Initialize MethodChannel handler and prefetch provisioning data for faster response.
  static Future<void> initialize({String ownerIdHint = 'owner'}) async {
    if (_initialized) return;
    debugPrint('[HCE] Initializing MethodChannel handler...');

    final svc = NfcProvisioningService(ownerId: ownerIdHint);

    // Dev helper: ensure we at least have a keypair persisted. In production,
    // this should be replaced with real provisioning of owner credentials.
    try {
      await svc.ensureOwnerKeysExist();
    } catch (e) {
      debugPrint('[HCE] ensureOwnerKeysExist failed: $e');
    }

    // Prefetch once to avoid service timeout
    try {
      _cachedProvisioningData = await svc._handleGetProvisioningData();
      debugPrint('[HCE] Prefetched provisioning data (publicKey len: '
          '${(_cachedProvisioningData?['publicKey'] as String?)?.length ?? 0}).');
    } catch (e) {
      debugPrint('[HCE] Prefetch failed: $e');
    }

    _channel.setMethodCallHandler((call) async {
      if (call.method == 'getProvisioningData') {
        debugPrint('[HCE] getProvisioningData request received');
        if (_cachedProvisioningData != null) return _cachedProvisioningData;
        final data = await svc._handleGetProvisioningData();
        _cachedProvisioningData = data;
        return data;
      }
      throw PlatformException(
        code: 'unimplemented',
        message: 'Unknown method: ${call.method}',
      );
    });

    _initialized = true;
  }

  /// Call once (e.g., app startup) to handle requests from HCE service.
  /// The HCE service will call method "getProvisioningData" and expect a Map:
  /// { publicKey: base64, certChain: base64, keyId: string }
  void registerHceHandler() {
    _channel.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'getProvisioningData':
          debugPrint('[HCE] getProvisioningData request received');
          if (_cachedProvisioningData != null) return _cachedProvisioningData;
          final data = await _handleGetProvisioningData();
          _cachedProvisioningData = data;
          return data;
        default:
          throw PlatformException(code: 'unimplemented', message: 'Unknown method: ${call.method}');
      }
    });
  }

  /// Handle HCE call to fetch provisioning data for ECU reader.
  Future<Map<String, dynamic>> _handleGetProvisioningData() async {
    final creds = await _loadOwnerCredentials((m) => debugPrint('[NFC-Provision] $m'));

    // publicKey: base64 uncompressed P-256
    final publicKeyB64 = base64Encode(creds.publicKey.bytes);

    // certChain: use DER certificate if present (single cert chain); empty otherwise
    final certChainB64 = creds.certDer != null && creds.certDer!.isNotEmpty
        ? base64Encode(creds.certDer!)
        : '';

    // keyId: derive from SHA-256(publicKey) -> first 16 hex chars
    final keyId = await _deriveKeyId(creds.publicKey.bytes);

    final out = {
      'publicKey': publicKeyB64,
      'certChain': certChainB64,
      'keyId': keyId,
    };
    return out;
  }

  // Legacy builder kept for compatibility if needed elsewhere
  // ignore: unused_element
  Future<Map<String, dynamic>> _buildPhoneProvision(
    Map<String, dynamic> ecuHello,
    void Function(String m) log,
  ) async {
    log('Building phone_provision payload...');

    final creds = await _loadOwnerCredentials(log);

    final now = DateTime.now().millisecondsSinceEpoch ~/ 1000; // seconds
    final nonceB64 = ecuHello['nonce'] as String? ?? '';
    final phonePubKeyB64 = base64Encode(creds.publicKey.bytes);
    final certDerB64 = creds.certDer != null ? base64Encode(creds.certDer!) : '';

    final message = <String, dynamic>{
      'type': 'phone_provision',
      'owner_id': ownerId,
      'phone_pubkey': phonePubKeyB64,
      'phone_cert': certDerB64,
      'nonce': nonceB64,
      'timestamp': now,
    };

    final signatureB64 = await _signProvision(message, creds, log);
    message['signature'] = signatureB64;

    return message;
  }

  /// Step 3b: sign the payload using owner private key.
  /// Spec: signature over (phone_pubkey || owner_id || nonce || timestamp)
  Future<String> _signProvision(
    Map<String, dynamic> message,
    _OwnerCredentials creds,
    void Function(String m) log,
  ) async {
    log('Signing phone_provision payload...');
    if (creds.keyPair == null) {
      throw Exception('No private key available for signing');
    }

    final phonePubKeyRaw = base64Decode(message['phone_pubkey'] as String);
    final ownerIdUtf8 = utf8.encode(message['owner_id'] as String);
    final nonceBytes = base64Decode(message['nonce'] as String);
    final timestampUtf8 = utf8.encode((message['timestamp'] as int).toString());

    final data = BytesBuilder()
      ..add(phonePubKeyRaw)
      ..add(ownerIdUtf8)
      ..add(nonceBytes)
      ..add(timestampUtf8);

    final dataToSignBytes = Uint8List.fromList(data.toBytes());
    final signature = await _ecdsa.sign(
      dataToSignBytes,
      keyPair: creds.keyPair!,
    );

    // cryptography emits raw r||s bytes for ECDSA
    final raw = Uint8List.fromList(signature.bytes);
    final der = _ecdsaRsToDer(raw);
    return base64Encode(der);
  }

  // NFC write removed in HCE mode

  // ECU public key storage is not used in HCE transport phase

  /// Helper: Extract the first NDEF text record's text content.
  // NDEF helpers removed in HCE mode

  /// Decode NDEF Text payload according to NFC Forum Text RTD:
  /// payload = status(1) | langCode(n) | text(m)
  // NDEF helpers removed in HCE mode

  /// Helper: Convert ECDSA raw (r||s) into ASN.1 DER encoded signature
  Uint8List _ecdsaRsToDer(Uint8List rsBytes) {
    final n = rsBytes.length ~/ 2;
    final r = rsBytes.sublist(0, n);
    final s = rsBytes.sublist(n);

    BigInt _trimmedBigInt(Uint8List b) {
      int i = 0;
      while (i < b.length - 1 && b[i] == 0) {
        i++;
      }
      final trimmed = b.sublist(i);
      if (trimmed.isEmpty) return BigInt.zero;
      final hex = trimmed.map((e) => e.toRadixString(16).padLeft(2, '0')).join();
      return BigInt.parse(hex, radix: 16);
    }

    BigInt rInt = _trimmedBigInt(Uint8List.fromList(r));
    BigInt sInt = _trimmedBigInt(Uint8List.fromList(s));

    Uint8List _encodeInteger(BigInt x) {
      var hex = x.toRadixString(16);
      if (hex.length % 2 == 1) hex = '0$hex';
      final bytes = Uint8List.fromList(List.generate(
          hex.length ~/ 2,
          (i) => int.parse(hex.substring(i * 2, i * 2 + 2), radix: 16)));
      if (bytes.isNotEmpty && (bytes[0] & 0x80) != 0) {
        final out = Uint8List(bytes.length + 1);
        out[0] = 0;
        out.setRange(1, out.length, bytes);
        return out;
      }
      return bytes;
    }

    final rEnc = _encodeInteger(rInt);
    final sEnc = _encodeInteger(sInt);

    final len = 2 + rEnc.length + 2 + sEnc.length;
    final out = BytesBuilder();
    out.addByte(0x30); // SEQUENCE
    out.addByte(len);
    out.addByte(0x02); // INTEGER
    out.addByte(rEnc.length);
    out.add(rEnc);
    out.addByte(0x02);
    out.addByte(sEnc.length);
    out.add(sEnc);
    return out.toBytes();
  }

  // NDEF helpers removed in HCE mode

  Future<String> _deriveKeyId(List<int> pubKeyBytes) async {
    final hash = await Sha256().hash(pubKeyBytes);
    final bytes = hash.bytes;
    final short = bytes.sublist(0, 8); // 64-bit prefix
    final sb = StringBuffer();
    for (final b in short) {
      sb.write(b.toRadixString(16).padLeft(2, '0'));
    }
    return sb.toString();
  }

  /// Load owner credentials from secure storage, creating them if missing.
  Future<_OwnerCredentials> _loadOwnerCredentials(
    void Function(String m) log,
  ) async {
    log('Loading owner credentials from secure storage...');

    final privB64 = await storage.read(key: 'owner_private_key_p256');
    final pubB64 = await storage.read(key: 'owner_public_key_p256');
    final certB64 = await storage.read(key: 'owner_cert_der');

    if (pubB64 == null) {
      throw Exception(
        'Missing owner credentials. Please provision and store '
        'base64(P-256 public key) under key owner_public_key_p256.',
      );
    }

    final pubBytes = base64Decode(pubB64);
    SimpleKeyPair? keyPair;
    if (privB64 != null) {
      final privBytes = base64Decode(privB64);
      keyPair = SimpleKeyPairData(
        privBytes,
        publicKey: SimplePublicKey(pubBytes, type: KeyPairType.p256),
        type: KeyPairType.p256,
      );
      log('Loaded P-256 keypair from secure storage.');
    } else {
      log('Loaded P-256 public key only (no private key stored).');
    }

    return _OwnerCredentials(
      keyPair: keyPair,
      publicKey: SimplePublicKey(pubBytes, type: KeyPairType.p256),
      certDer: certB64 != null && certB64.isNotEmpty ? base64Decode(certB64) : null,
    );
  }

  /// Optional: generate and store owner keys for dev/test only. Use with caution.
  /// Not suitable for production if keys should live in platform keystore.

  //technical debt
  Future<void> ensureOwnerKeysExist() async {
    final pub = await storage.read(key: 'owner_public_key_p256');
    if (pub != null) return;

    debugPrint('[NFC-Provision] Generating dev-only owner public key...');
    try {
      final algorithm = Ecdsa.p256(Sha256());
      final keyPair = await algorithm.newKeyPair();
  final EcPublicKey pubKey = await keyPair.extractPublicKey();
      // Build uncompressed 65-byte point: 0x04 || X(32) || Y(32)
      final x = _leftPadTo32(pubKey.x);
      final y = _leftPadTo32(pubKey.y);
      final pkBytes = Uint8List(65)
        ..[0] = 0x04
        ..setRange(1, 33, x)
        ..setRange(33, 65, y);
      await storage.write(key: 'owner_public_key_p256', value: base64Encode(pkBytes));
      debugPrint('[NFC-Provision] Dev-only public key generated and stored.');
    } catch (e) {
      debugPrint('[NFC-Provision] Dev key generation failed: $e');
      // Fallback: store a well-known valid P-256 public key (base point G) uncompressed.
      // X = 6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
      // Y = 4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5
      final pkBytes = Uint8List.fromList([
        0x04,
        0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47,
        0xF8, 0xBC, 0xE6, 0xE5, 0x63, 0xA4, 0x40, 0xF2,
        0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0,
        0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96,
        0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B,
        0x8E, 0xE7, 0xEB, 0x4A, 0x7C, 0x0F, 0x9E, 0x16,
        0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE,
        0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5,
      ]);
      await storage.write(key: 'owner_public_key_p256', value: base64Encode(pkBytes));
      debugPrint('[NFC-Provision] Stored fallback dev public key.');
    }
  }
}

class _OwnerCredentials {
  _OwnerCredentials({
    required this.keyPair,
    required this.publicKey,
    this.certDer,
  });

  final SimpleKeyPair? keyPair;
  final SimplePublicKey publicKey;
  final Uint8List? certDer;
}

Uint8List _leftPadTo32(List<int> input) {
  final out = Uint8List(32);
  final src = Uint8List.fromList(input);
  final copy = src.length > 32 ? 32 : src.length;
  out.setRange(32 - copy, 32, src.sublist(src.length - copy));
  return out;
}

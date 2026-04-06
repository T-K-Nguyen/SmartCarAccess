package com.example.smart_car_app

import android.content.Context
import android.util.Log
import com.smartcaraccess.KeystoreBridge
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * ProvisioningResponseBuilder - Build APDU response packets for Phase A (NFC Provisioning)
 * 
 * Handles two types of responses:
 * 1. Base credentials: keyId + publicKey + certificate
 * 2. Signature: DER-encoded ECDSA signature over challenge
 */
object ProvisioningResponseBuilder {
    private const val TAG = "ProvisioningResponseBuilder"
    
    /**
     * Build base credentials packet for Phase A.
     * Format: [keyIdLen(1) + keyId(keyIdLen) + publicKey(65) + certLen(2) + certificate]
     * 
     * For now, we don't include certificate (certLen = 0).
     * Returns 69 bytes total: 1 + 1 + 65 + 2 + 0
     */
    fun buildBaseCredentialsPacket(context: Context): ByteArray {
        Log.d(TAG, "Building base credentials packet")
        
        // Ensure key exists in Keystore
        KeystoreBridge.ensurePhaseAKey()
        
        // Get public key
        val publicKey65 = KeystoreBridge.getPhaseAPublicKey65()
        if (publicKey65 == null) {
            Log.e(TAG, "Failed to get public key from Keystore")
            return ByteArray(0)
        }
        
        // Build packet
        val packet = ByteArray(69) // keyIdLen(1) + keyId(1) + pubKey(65) + certLen(2)
        var offset = 0
        
        // KeyId length (1 byte)
        packet[offset++] = 0x01 // keyId is 1 byte
        
        // KeyId (1 byte) - arbitrary identifier
        packet[offset++] = 0x01 // actual keyId value
        
        // Public key (65 bytes uncompressed)
        System.arraycopy(publicKey65, 0, packet, offset, 65)
        offset += 65
        
        // Certificate length (2 bytes, big-endian) - 0 for now
        packet[offset++] = 0x00
        packet[offset] = 0x00
        
        Log.i(TAG, "✓ Base credentials packet built: ${packet.size} bytes")
        Log.d(TAG, "   KeyIdLen: 1, KeyId: 0x01")
        Log.d(TAG, "   Public key: ${publicKey65.toHex()}")
        Log.d(TAG, "   Cert length: 0")
        
        return packet
    }

    /**
     * Wrap base credentials in CCC GET DATA tag 7F24.
     */
    fun buildCccGetDataPacket(context: Context): ByteArray {
        val inner = buildBaseCredentialsPacket(context)
        if (inner.isEmpty()) return ByteArray(0)
        return wrapWith7F24(inner)
    }
    
    /**
     * Build signature packet for Phase A challenge-response.
     * Format: [sigLen(2, big-endian) + DER_signature]
     * 
     * @param context Android context
     * @param challenge Challenge from ECU (vehicleId(8) + nonce(16))
     * @return Signature packet
     */
    fun buildSignaturePacket(context: Context, challenge: ByteArray): ByteArray {
        Log.d(TAG, "Building signature packet for challenge: ${challenge.toHex()}")
        
        if (challenge.size != 24) {
            Log.w(TAG, "Challenge size is ${challenge.size}, expected 24 bytes")
        }
        
        // Sign challenge with Keystore identity key.
        // Some PN532 + phone combinations are flaky when APDU payload exceeds 72 bytes,
        // so prefer DER signatures <= 70 bytes (packet <= 74 with SW1SW2).
        var signature = KeystoreBridge.signPhaseA(challenge)
        if (signature != null && signature.size > 70) {
            for (attempt in 2..8) {
                val retrySig = KeystoreBridge.signPhaseA(challenge)
                if (retrySig != null) {
                    signature = retrySig
                    if (retrySig.size <= 70) {
                        Log.i(TAG, "Using compact DER signature after retry #$attempt (len=${retrySig.size})")
                        break
                    }
                }
            }
        }

        if (signature == null) {
            Log.e(TAG, "Failed to sign challenge")
            return ByteArray(0)
        }
        
        // Build packet: [sigLen(2, BE) + signature]
        val packet = ByteArray(2 + signature.size)
        
        // Signature length (big-endian)
        packet[0] = ((signature.size shr 8) and 0xFF).toByte()
        packet[1] = (signature.size and 0xFF).toByte()
        
        // Signature bytes
        System.arraycopy(signature, 0, packet, 2, signature.size)
        
        Log.i(TAG, "✓ Signature packet built: ${packet.size} bytes")
        Log.d(TAG, "   Signature length (BE): ${signature.size}")
        Log.d(TAG, "   Signature: ${signature.toHex()}")
        
        return packet
    }

    private fun wrapWith7F24(inner: ByteArray): ByteArray {
        val len = inner.size
        return if (len <= 0x7F) {
            byteArrayOf(0x7F.toByte(), 0x24.toByte(), len.toByte()) + inner
        } else {
            byteArrayOf(0x7F.toByte(), 0x24.toByte(), 0x81.toByte(), len.toByte()) + inner
        }
    }

    /**
     * Build signature packet with HMAC for master-card provisioning.
     * Format: [mac(32) + sigLen(2, big-endian) + DER_signature]
     * MAC = HMAC_SHA256(masterSecret, challenge || phonePub65)
     */
    fun buildSignaturePacketWithMac(
        context: Context,
        challenge: ByteArray,
        masterSecret: ByteArray
    ): ByteArray {
        Log.d(TAG, "Building signature+MAC packet for master-card provisioning")

        // Ensure key exists in Keystore and fetch phone public key for binding
        KeystoreBridge.ensurePhaseAKey()
        val phonePub65 = KeystoreBridge.getPhaseAPublicKey65()
        if (phonePub65 == null) {
            Log.e(TAG, "Failed to get phone public key for MAC binding")
            return ByteArray(0)
        }

        var signature = KeystoreBridge.signPhaseA(challenge)
        if (signature != null && signature.size > 70) {
            for (attempt in 2..8) {
                val retrySig = KeystoreBridge.signPhaseA(challenge)
                if (retrySig != null) {
                    signature = retrySig
                    if (retrySig.size <= 70) {
                        Log.i(TAG, "Using compact DER signature+MAC after retry #$attempt (len=${retrySig.size})")
                        break
                    }
                }
            }
        }
        if (signature == null) {
            Log.e(TAG, "Failed to sign challenge")
            return ByteArray(0)
        }

        val macInput = ByteArray(challenge.size + phonePub65.size)
        System.arraycopy(challenge, 0, macInput, 0, challenge.size)
        System.arraycopy(phonePub65, 0, macInput, challenge.size, phonePub65.size)

        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(masterSecret, "HmacSHA256"))
        val macOut = mac.doFinal(macInput)

        val packet = ByteArray(32 + 2 + signature.size)
        var offset = 0
        System.arraycopy(macOut, 0, packet, offset, 32)
        offset += 32

        packet[offset++] = ((signature.size shr 8) and 0xFF).toByte()
        packet[offset++] = (signature.size and 0xFF).toByte()
        System.arraycopy(signature, 0, packet, offset, signature.size)

        Log.i(TAG, "✓ Signature+MAC packet built: ${packet.size} bytes")
        Log.d(TAG, "   MAC: ${macOut.toHex()}")
        Log.d(TAG, "   Signature length (BE): ${signature.size}")

        return packet
    }
    
    /**
     * Verify signature length prefix is correctly encoded as big-endian.
     * This is a helper for debugging.
     */
    fun verifySigLenPrefix(packet: ByteArray): Boolean {
        if (packet.size < 2) return false
        
        val declaredBE = ((packet[0].toInt() and 0xFF) shl 8) or (packet[1].toInt() and 0xFF)
        val actualLen = packet.size - 2
        
        if (declaredBE != actualLen) {
            Log.w(TAG, "Signature length mismatch: declared(BE)=$declaredBE, actual=$actualLen")
            return false
        }
        
        return true
    }
}

// Extension function for hex encoding
private fun ByteArray.toHex(): String = joinToString("") { "%02X".format(it) }

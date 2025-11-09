package com.example.smart_car_app

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import java.security.*
import java.security.spec.ECGenParameterSpec
import java.security.spec.PKCS8EncodedKeySpec
import java.security.spec.X509EncodedKeySpec
import java.util.*

private const val PREF_KEYS = "smart_car_keys"
private const val KEY_PRIV = "ec_priv_p256_b64"
private const val KEY_PUB = "ec_pub_p256_b64"

object ProvisioningKeyManager {
    private const val TAG = "ProvisioningKeyManager"
    private val ecSpec = ECGenParameterSpec("secp256r1")

    fun loadOrGenerate(context: Context): KeyPair {
        val prefs = context.getSharedPreferences(PREF_KEYS, Context.MODE_PRIVATE)
        val privB64 = prefs.getString(KEY_PRIV, null)
        val pubB64 = prefs.getString(KEY_PUB, null)
        if (privB64 != null && pubB64 != null) {
            try {
                val kf = KeyFactory.getInstance("EC")
                val priv = kf.generatePrivate(PKCS8EncodedKeySpec(Base64.getDecoder().decode(privB64)))
                val pub = kf.generatePublic(X509EncodedKeySpec(Base64.getDecoder().decode(pubB64)))
                return KeyPair(pub, priv)
            } catch (e: Exception) {
                Log.w(TAG, "Failed to reload keypair, regenerating: ${e.message}")
            }
        }
        val kpg = KeyPairGenerator.getInstance("EC")
        kpg.initialize(ecSpec, SecureRandom())
        val kp = kpg.generateKeyPair()
        prefs.edit()
            .putString(KEY_PRIV, Base64.getEncoder().encodeToString(kp.private.encoded))
            .putString(KEY_PUB, Base64.getEncoder().encodeToString(kp.public.encoded))
            .apply()
        return kp
    }

    fun publicKeyUncompressed(kp: KeyPair): ByteArray {
        val ecPub = kp.public
        val encoded = ecPub.encoded // X.509 SubjectPublicKeyInfo
        // Extract EC point from ASN.1 (quick heuristic search for 0x04 leading uncompressed point)
        val idx = encoded.indexOf(0x04)
        require(idx >= 0 && idx + 65 <= encoded.size) { "Cannot find uncompressed EC point" }
        return encoded.copyOfRange(idx, idx + 65)
    }

    fun computeKeyId(pubUncompressed: ByteArray): String {
        val md = MessageDigest.getInstance("SHA-256")
        val hash = md.digest(pubUncompressed)
        return hash.copyOfRange(0, 8).joinToString("") { String.format("%02X", it) }
    }

    fun signChallenge(kp: KeyPair, challenge: ByteArray): ByteArray {
        val sig = Signature.getInstance("SHA256withECDSA")
        sig.initSign(kp.private)
        sig.update(challenge)
        return sig.sign() // DER encoded (r,s)
    }
}

object ProvisioningResponseBuilder {
    private const val TAG = "ProvisioningResponseBuilder"

    fun buildBaseCredentialsPacket(context: Context): ByteArray {
        val kp = ProvisioningKeyManager.loadOrGenerate(context)
        val pub65 = ProvisioningKeyManager.publicKeyUncompressed(kp)
        val keyId = ProvisioningKeyManager.computeKeyId(pub65)
        val keyIdBytes = keyId.toByteArray(Charsets.UTF_8)
        val certBytes = ByteArray(0) // No cert chain Phase A
        val out = ArrayList<Byte>()
        out.add(keyIdBytes.size.toByte())
        keyIdBytes.forEach { out.add(it) }
        pub65.forEach { out.add(it) }
        // 2-byte cert length (little endian for consistency with earlier design)
        out.add((certBytes.size and 0xFF).toByte())
        out.add(((certBytes.size shr 8) and 0xFF).toByte())
        // No cert data appended (empty)
        return out.toByteArray()
    }

    fun buildSignaturePacket(context: Context, challenge: ByteArray): ByteArray {
        val kp = ProvisioningKeyManager.loadOrGenerate(context)
        val derSig = ProvisioningKeyManager.signChallenge(kp, challenge)
        val out = ArrayList<Byte>()
        // length prefix (2 bytes BIG-endian) then DER signature
        out.add(((derSig.size shr 8) and 0xFF).toByte())
        out.add((derSig.size and 0xFF).toByte())
        derSig.forEach { out.add(it) }
        return out.toByteArray()
    }
}
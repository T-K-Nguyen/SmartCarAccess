package com.example.smart_car_app

import android.nfc.cardemulation.HostApduService
import android.os.Bundle
import android.util.Log

class ProvisioningHostApduService : HostApduService() {
    companion object {
        private const val TAG = "ProvisioningHostApduService"
        private val AID = byteArrayOf(0xF0.toByte(),0x01,0x02,0x03,0x04,0x05) // F0 01 02 03 04 05
        private val SW_SUCCESS = byteArrayOf(0x90.toByte(), 0x00)
        private val SW_INS_NOT_SUPPORTED = byteArrayOf(0x6D.toByte(), 0x00)
        private val SW_CLA_NOT_SUPPORTED = byteArrayOf(0x6E.toByte(), 0x00)
        private val SW_UNKNOWN = byteArrayOf(0x6F.toByte(), 0x00)
        private val SW_SECURITY_STATUS_NOT_SATISFIED = byteArrayOf(0x69.toByte(), 0x82.toByte()) // Not logged in
        private const val INS_GET_CHALLENGE = 0xCA.toByte()
        private const val INS_READ_BINARY = 0xB0.toByte()
        private const val INS_PROVISION_RESULT = 0xDA.toByte()
    }

    // Track selection
    private var aidSelected = false

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "HCE provisioning service created. AID=${AID.toHex()}")
    }

    override fun processCommandApdu(commandApdu: ByteArray?, extras: Bundle?): ByteArray {
        if (commandApdu == null) return SW_UNKNOWN

        // NOTE: We now only enforce login for the signature step. Allow SELECT and base (Lc=0) for testing.

        if (commandApdu.size < 4) return SW_UNKNOWN
        val cla = commandApdu[0]
        val ins = commandApdu[1]
        val p1 = commandApdu[2]
        val p2 = commandApdu[3]
        val hasLc = commandApdu.size >= 5
        val lc = if (hasLc) (commandApdu[4].toInt() and 0xFF) else 0
        val dataStart = if (hasLc) 5 else 4
        val dataEnd = dataStart + lc
        val commandData = if (lc > 0 && dataEnd <= commandApdu.size) commandApdu.copyOfRange(dataStart, dataEnd) else ByteArray(0)

        if (cla != 0x00.toByte()) return SW_CLA_NOT_SUPPORTED

        return try {
            when (ins) {
                0xA4.toByte() -> handleSelect(commandApdu) // always allow SELECT
                INS_GET_CHALLENGE -> handleGetChallenge(lc, commandData)
                INS_READ_BINARY -> handleReadBinary(p1, p2)
                INS_PROVISION_RESULT -> handleProvisionResult(lc, commandData)
                else -> SW_INS_NOT_SUPPORTED
            }
        } catch (e: Exception) {
            Log.e(TAG, "Exception processing APDU: ${e.message}", e)
            SW_UNKNOWN
        }
    }

    private fun handleSelect(apdu: ByteArray): ByteArray {
        // Minimal SELECT parsing: 00 A4 04 00 Lc <AID>
        if (apdu.size < 5) return SW_UNKNOWN
        val lc = apdu[4].toInt() and 0xFF
        val start = 5
        val end = start + lc
        if (end > apdu.size) return SW_UNKNOWN
        val aidBytes = apdu.copyOfRange(start, end)
        aidSelected = aidBytes.contentEquals(AID)
        return if (aidSelected) {
            // Return 4-byte UID + 9000
            val uid = DataStoreUtil.getOrCreate4ByteUid(this)
            uid + SW_SUCCESS
        } else {
            SW_INS_NOT_SUPPORTED
        }
    }

    private fun handleGetChallenge(lc: Int, data: ByteArray): ByteArray {
        if (!aidSelected) return SW_UNKNOWN
        
        // SECURITY: Require user to be logged in for ALL provisioning steps
        // This ensures user must open app and authenticate before provisioning
        if (!isUserLoggedIn()) {
            if (!isTestBypassEnabled()) {
                Log.w(TAG, "[PhaseA] SECURITY: User not logged in, provisioning denied")
                return SW_SECURITY_STATUS_NOT_SATISFIED
            }
            Log.w(TAG, "[PhaseA] TEST BYPASS: proceeding while not logged in (DEBUG MODE ONLY)")
        }
        
        return if (lc == 0) {
            // Base credentials (keyId + pub + empty cert)
            val base = ProvisioningResponseBuilder.buildBaseCredentialsPacket(this)
            Log.i(TAG, "[PhaseA] OUT Base len=${base.size} last2=${String.format("%02X%02X", SW_SUCCESS[0], SW_SUCCESS[1])}")
            base + SW_SUCCESS
        } else {
            // Signature-only: challenge = incoming data
            Log.i(TAG, "[PhaseA] challenge(${lc}): ${data.toHex()}")
            // Master-card mode: include HMAC binding of challenge + phone pub
            if (MasterCardSession.isActive()) {
                val masterSecret = MasterCardSession.getMasterSecret()
                val masterVid = MasterCardSession.getVehicleId()
                if (masterSecret == null || masterVid == null) {
                    Log.w(TAG, "[PhaseA] Master session inactive after check")
                    return SW_SECURITY_STATUS_NOT_SATISFIED
                }
                if (data.size < 24) {
                    Log.w(TAG, "[PhaseA] Challenge too short for master provisioning: ${data.size}")
                    return SW_UNKNOWN
                }
                val challengeVid = data.copyOfRange(0, 8)
                if (!challengeVid.contentEquals(masterVid)) {
                    Log.w(TAG, "[PhaseA] VehicleId mismatch for master provisioning")
                    return SW_SECURITY_STATUS_NOT_SATISFIED
                }
                val pkt = ProvisioningResponseBuilder.buildSignaturePacketWithMac(this, data, masterSecret)
                Log.i(TAG, "[PhaseA] OUT Master pktLen=${pkt.size}")
                return pkt + SW_SUCCESS
            }

            // Legacy signature-only response
            val pkt = ProvisioningResponseBuilder.buildSignaturePacket(this, data)
            // Validate length prefix (BIG-endian) vs actual DER length
            if (pkt.size >= 2) {
                val declaredBE = ((pkt[0].toInt() and 0xFF) shl 8) or (pkt[1].toInt() and 0xFF)
                val actual = pkt.size - 2
                if (declaredBE != actual) {
                    // Try detect if it was little-endian and fix to big-endian for the reader
                    val declaredLE = (pkt[0].toInt() and 0xFF) or ((pkt[1].toInt() and 0xFF) shl 8)
                    Log.w(TAG, "[PhaseA] sigLen prefix mismatch: declaredBE=$declaredBE declaredLE=$declaredLE actual=$actual. Fixing to BE.")
                    // Write BIG-endian prefix
                    pkt[0] = ((actual shr 8) and 0xFF).toByte()
                    pkt[1] = (actual and 0xFF).toByte()
                }
                val sigLenBE = ((pkt[0].toInt() and 0xFF) shl 8) or (pkt[1].toInt() and 0xFF)
                Log.i(TAG, "[PhaseA] OUT Sig pktLen=${pkt.size} sigLenBE=${sigLenBE} first4=${pkt.copyOfRange(0, minOf(4, pkt.size)).toHex()}")
            } else {
                Log.w(TAG, "[PhaseA] Signature packet too short: ${pkt.size}")
            }
            pkt + SW_SUCCESS
        }
    }

    private fun handleReadBinary(p1: Byte, p2: Byte): ByteArray {
        if (!aidSelected) return SW_UNKNOWN
        val uid = DataStoreUtil.getOrCreate4ByteUid(this)
        return uid + SW_SUCCESS
    }

    private fun handleProvisionResult(lc: Int, data: ByteArray): ByteArray {
        if (!aidSelected) return SW_UNKNOWN
        if (lc < 1) return SW_UNKNOWN
        val success = data[0] == 0x01.toByte()
        val prefs = getSharedPreferences("FlutterSharedPreferences", MODE_PRIVATE)
        prefs.edit()
            .putBoolean("flutter.provision_result", success)
            .putLong("flutter.provision_ts", System.currentTimeMillis())
            .apply()
        Log.i(TAG, "[PhaseA] Provision result received: ${if (success) "OK" else "FAIL"}")
        return SW_SUCCESS
    }

    private fun isUserLoggedIn(): Boolean {
        // Check Flutter's shared_preferences (FlutterSharedPreferences, key flutter.is_logged_in)
        val flutterPrefs = getSharedPreferences("FlutterSharedPreferences", MODE_PRIVATE)
        if (flutterPrefs.contains("flutter.is_logged_in")) {
            return flutterPrefs.getBoolean("flutter.is_logged_in", false)
        }
        // Fallback to app-local prefs if you choose to set it natively
        val prefs = getSharedPreferences("smart_car_login", MODE_PRIVATE)
        return prefs.getBoolean("logged_in", false)
    }

    private fun isTestBypassEnabled(): Boolean {
        // PRODUCTION: Only allow bypass in debug builds (BuildConfig.DEBUG)
        // Remove test_mode preference for production security
        return try {
            val isDebuggable = (applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0
            if (!isDebuggable) {
                // Production build - NO BYPASS
                return false
            }
            // Debug build - allow explicit test mode preference
            val testPrefs = getSharedPreferences("smart_car_test", MODE_PRIVATE)
            testPrefs.getBoolean("test_mode", false) || isDebuggable
        } catch (e: Exception) { false }
    }

    override fun onDeactivated(reason: Int) {
        aidSelected = false
        Log.i(TAG, "HCE deactivated reason=$reason")
    }
}

private fun ByteArray.toHex(): String = joinToString("") { String.format("%02X", it) }
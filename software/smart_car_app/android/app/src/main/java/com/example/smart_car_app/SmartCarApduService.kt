package com.example.smart_car_app

import android.nfc.cardemulation.HostApduService
import android.os.Bundle
import android.util.Base64
import android.util.Log
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.dart.DartExecutor
import io.flutter.plugin.common.MethodChannel
import java.io.ByteArrayOutputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit


import io.flutter.FlutterInjector

class SmartCarApduService : HostApduService() {
    companion object {
        private const val TAG = "SmartCarApduService"
        // Use the standard AID that matches apduservice.xml and ESP32
        private val AID = byteArrayOf(
            0xA0.toByte(), 0x00, 0x00, 0x00, 0x04, 0x10, 0x10
        )

        private const val CHANNEL_NAME = "smartcar.hce"
        private val SW_SUCCESS = byteArrayOf(0x90.toByte(), 0x00)
        private val SW_UNKNOWN = byteArrayOf(0x6A.toByte(), 0x80.toByte())
        
        // Activation tracking
        private var activationCount = 0
        private var lastActivation = 0L
    }

    private var engine: FlutterEngine? = null
    private var methodChannel: MethodChannel? = null

    override fun onCreate() {
        super.onCreate()
        
        // EMERGENCY LOGGING - MUST BE VISIBLE
        Log.e(TAG, "🔥🔥🔥 HCE SERVICE CREATED!!!")
        Log.e(TAG, "SmartCarApduService is now active and listening")
        Log.e(TAG, "Registered AID: ${AID.joinToString("") { String.format("%02X", it) }}")
        Log.e(TAG, "Expected ESP32 SELECT: 00 A4 04 00 07 A0 00 00 00 04 10 10")
        Log.e(TAG, "Device unlock required: false")
        Log.e(TAG, "Service category: other (non-payment)")
        Log.e(TAG, "Ready to receive APDU commands from NFC readers")
        
        // CRITICAL: Multiple output streams for maximum visibility
        System.out.println("🔥🔥🔥 SmartCarApduService CREATED - HCE is ready!")
        System.err.println("🔥🔥🔥 HCE AID: ${AID.joinToString(" ") { String.format("%02X", it) }}")
        System.err.println("🔥🔥🔥 ESP32 should send: 00 A4 04 00 07 A0 00 00 00 04 10 10")
        println("🔥🔥🔥 HCE SERVICE CREATED AND READY!")
        
        // Android 12 specific logging and checks
        android.util.Log.wtf(TAG, "🔥🔥🔥 ANDROID 12 HCE SERVICE CREATED!")
        
        // Check Android 12 HCE registration
        try {
            val nfcAdapter = android.nfc.NfcAdapter.getDefaultAdapter(this)
            if (nfcAdapter != null) {
                val cardEmulation = android.nfc.cardemulation.CardEmulation.getInstance(nfcAdapter)
                Log.e(TAG, "📱 Android 12 NFC Status: ${if (nfcAdapter.isEnabled) "ENABLED" else "DISABLED"}")
                Log.e(TAG, "📱 HCE Available: ${cardEmulation != null}")
                System.out.println("📱📱📱 Android 12 NFC: ${if (nfcAdapter.isEnabled) "ON" else "OFF"}")
            }
        } catch (e: Exception) {
            Log.e(TAG, "📱 Android 12 NFC check failed: ${e.message}")
        }
        
        // Force immediate method channel setup
        ensureEngine()
        
        // Log service registration info
        try {
            val packageManager = packageManager
            val serviceInfo = packageManager.getServiceInfo(
                android.content.ComponentName(this, SmartCarApduService::class.java),
                android.content.pm.PackageManager.GET_META_DATA
            )
            Log.i(TAG, "✅ HCE Service successfully registered in manifest")
            Log.i(TAG, "Service enabled: ${serviceInfo.isEnabled}")
        } catch (e: Exception) {
            Log.e(TAG, "❌ HCE Service registration check failed: ${e.message}")
        }
    }

    override fun processCommandApdu(commandApdu: ByteArray?, extras: Bundle?): ByteArray {
        // IMMEDIATE EMERGENCY LOGGING - MUST BE VISIBLE
        Log.e(TAG, "🚨🚨🚨 PROCESS COMMAND APDU CALLED!!!")
        System.out.println("🚨🚨🚨 HCE processCommandApdu CALLED - ESP32 IS COMMUNICATING!")
        System.err.println("🚨🚨🚨 HCE SERVICE ACTIVATED BY NFC READER!")
        println("🚨🚨🚨 ANDROID HCE SERVICE IS RESPONDING!")
        
        // Track activations
        val currentTime = System.currentTimeMillis()
        
        if (currentTime - lastActivation > 1000) { // New session if >1 second gap
            activationCount++
            lastActivation = currentTime
            Log.e(TAG, "=== HCE SERVICE ACTIVATION #$activationCount ===")
            Log.e(TAG, "🎯 NFC READER DETECTED! ESP32 is trying to communicate!")
            
            // CRITICAL: Print to system streams for Flutter console visibility
            System.out.println("🎯🎯🎯 HCE ACTIVATION #$activationCount - ESP32 DETECTED!")
            System.err.println("🎯🎯🎯 NFC READER IS COMMUNICATING WITH PHONE!")
            println("🎯🎯🎯 NEW HCE SESSION STARTED!")
        }
        
        Log.e(TAG, "=== processCommandApdu called (activation #$activationCount) ===")
        Log.e(TAG, "Timestamp: $currentTime")
        Log.e(TAG, "Thread: ${Thread.currentThread().name}")
        
        // CRITICAL: Always print APDU reception to system streams with different markers
        System.out.println("📨📨📨 HCE RECEIVED APDU FROM ESP32!")
        System.err.println("📨📨📨 PROCESSING APDU COMMAND!")
        println("📨📨📨 HCE RECEIVED APDU!") // Also use println for Dart console
        
        if (commandApdu == null) {
            Log.e(TAG, "❌ Received null APDU - returning error")
            System.err.println("❌❌❌ NULL APDU RECEIVED!")
            println("❌❌❌ NULL APDU RECEIVED!")
            return SW_UNKNOWN
        }
        
        val apduHex = commandApdu.joinToString(" ") { String.format("%02X", it) }
        Log.e(TAG, "📨 APDU IN (${commandApdu.size} bytes): $apduHex")
        Log.e(TAG, "📋 Expected SELECT AID: 00 A4 04 00 07 F0 01 02 03 04 05 0F")
        
        // CRITICAL: Print APDU details to system streams
        System.out.println("📨📨📨 APDU RECEIVED (${commandApdu.size} bytes): $apduHex")
        System.err.println("📋📋📋 Expected ESP32 Command: 00 A4 04 00 07 F0 01 02 03 04 05 0F")
        println("📨📨📨 APDU: $apduHex")
        
        // FOR DEBUGGING: ALWAYS RETURN SUCCESS FIRST
        Log.e(TAG, "🧪 DEBUG MODE: Returning SUCCESS (90 00) for ANY APDU")
        System.out.println("🧪🧪🧪 DEBUG: ALWAYS RETURNING SUCCESS!")
        System.err.println("✅✅✅ SENDING 90 00 SUCCESS TO ESP32")
        println("✅✅✅ RETURNING SUCCESS (90 00)")
        return SW_SUCCESS

        /*
        // COMMENTED OUT FOR DEBUGGING - WILL RE-ENABLE AFTER BASIC COMMUNICATION WORKS
        
        // Simple APDU decoding:
        // SELECT AID: 00 A4 04 00 Lc <AID>
        if (isSelectAid(commandApdu)) {
            System.out.println("🔍🔍🔍 DETECTED SELECT AID COMMAND!")
            val aid = extractAid(commandApdu)
            val extractedHex = aid?.joinToString("") { String.format("%02X", it) } ?: "null"
            val expectedHex = AID.joinToString("") { String.format("%02X", it) }
            
            Log.d(TAG, "Extracted AID: $extractedHex")
            Log.d(TAG, "Expected AID: $expectedHex")
            
            System.out.println("🔍🔍🔍 Extracted AID: $extractedHex")
            System.out.println("🔍🔍🔍 Expected AID: $expectedHex")
            
            return if (aid != null && aid.contentEquals(AID)) {
                Log.i(TAG, "SELECT AID matched - returning SUCCESS (90 00)")
                System.out.println("✅✅✅ AID MATCHED! RETURNING SUCCESS (90 00)")
                System.err.println("✅✅✅ SENDING 90 00 RESPONSE TO ESP32")
                SW_SUCCESS
            } else {
                Log.w(TAG, "SELECT AID mismatch - returning UNKNOWN (6A 80)")
                System.err.println("❌❌❌ AID MISMATCH! RETURNING ERROR (6A 80)")
                System.err.println("❌❌❌ EXPECTED: ${AID.joinToString("") { String.format("%02X", it) }}")
                System.err.println("❌❌❌ RECEIVED: ${aid?.joinToString("") { String.format("%02X", it) } ?: "null"}")
                SW_UNKNOWN
            }
        }

        // GET INFO (custom): 00 CA 00 00 {vehicleId || nonce}
        if (isGetData(commandApdu)) {
            Log.i(TAG, "GET_INFO request received; fetching data from Flutter...")
            val data = getProvisioningDataFromFlutter()
            return if (data != null) {
                Log.i(TAG, "Returning provisioning payload (${data.size} bytes) + 0x9000")
                data + SW_SUCCESS
            } else {
                SW_UNKNOWN
            }
        }

        return SW_UNKNOWN
        */
    }

    override fun onDeactivated(reason: Int) {
        Log.i(TAG, "=== HCE Service DEACTIVATED ===")
        Log.i(TAG, "Deactivation reason: $reason (0=LINK_LOST, 1=DESELECTED)")
        Log.i(TAG, "Session ended at: ${System.currentTimeMillis()}")
        
        System.out.println("🔚🔚🔚 HCE SESSION ENDED - reason: $reason")
    }
    
    // This method is called when the HCE service is activated by an NFC reader
    // Unfortunately, there's no onActivated() callback, but we can detect it in processCommandApdu

    private fun ensureEngine() {
        if (engine != null && methodChannel != null) return
        engine = FlutterEngine(applicationContext)
        val dartExecutor: DartExecutor = engine!!.dartExecutor
        // Use a background Dart entrypoint that only registers MethodChannel, no UI
        dartExecutor.executeDartEntrypoint(
            DartExecutor.DartEntrypoint(
                FlutterInjector.instance().flutterLoader().findAppBundlePath(),
                "hceMain"
            )
        )
        methodChannel = MethodChannel(dartExecutor.binaryMessenger, CHANNEL_NAME)
    }

    private fun getProvisioningDataFromFlutter(): ByteArray? {
        ensureEngine()
        val channel = methodChannel ?: return null

        var resultBytes: ByteArray? = null
        val latch = CountDownLatch(1)

        // Test connection first
        try {
            Log.i(TAG, "🧪 Testing method channel connection...")
            System.out.println("🧪 TESTING METHOD CHANNEL CONNECTION...")
            channel.invokeMethod("testConnection", null, object : MethodChannel.Result {
                override fun success(result: Any?) {
                    Log.i(TAG, "✅ Method channel test successful")
                    System.out.println("✅ METHOD CHANNEL TEST SUCCESSFUL")
                }
                override fun error(errorCode: String, errorMessage: String?, errorDetails: Any?) {
                    Log.w(TAG, "⚠️ Method channel test error: $errorCode $errorMessage")
                }
                override fun notImplemented() {
                    Log.w(TAG, "⚠️ Method channel test not implemented")
                }
            })
        } catch (e: Exception) {
            Log.e(TAG, "❌ Method channel test failed: ${e.message}", e)
        }

        try {
            channel.invokeMethod("getProvisioningData", null, object : MethodChannel.Result {
                override fun success(result: Any?) {
                    try {
                        resultBytes = buildProvisioningPayload(result)
                        if (resultBytes == null) {
                            Log.e(TAG, "Provisioning payload formatting failed")
                        }
                    } catch (e: Exception) {
                        Log.e(TAG, "Error building provisioning payload: ${e.message}", e)
                    } finally {
                        latch.countDown()
                    }
                }

                override fun error(errorCode: String, errorMessage: String?, errorDetails: Any?) {
                    Log.e(TAG, "MethodChannel error: $errorCode $errorMessage")
                    latch.countDown()
                }

                override fun notImplemented() {
                    Log.e(TAG, "Method not implemented in Dart")
                    latch.countDown()
                }
            })
        } catch (e: Exception) {
            Log.e(TAG, "invokeMethod exception: ${e.message}", e)
            latch.countDown()
        }

        // Wait up to 4 seconds for Dart to respond
        latch.await(4, TimeUnit.SECONDS)
        return resultBytes
    }

    private fun isSelectAid(apdu: ByteArray): Boolean {
        return apdu.size >= 5 && apdu[0] == 0x00.toByte() && apdu[1] == 0xA4.toByte() && apdu[2] == 0x04.toByte()
    }

    private fun extractAid(apdu: ByteArray): ByteArray? {
        if (apdu.size < 5) return null
        val lc = apdu[4].toInt() and 0xFF
        val start = 5
        val end = start + lc
        return if (end <= apdu.size) apdu.copyOfRange(start, end) else null
    }

    private fun isGetData(apdu: ByteArray): Boolean {
        return apdu.size >= 4 && apdu[0] == 0x00.toByte() && apdu[1] == 0xCA.toByte() && apdu[2] == 0x00.toByte() && apdu[3] == 0x00.toByte()
    }

    @Suppress("UNCHECKED_CAST")
    private fun buildProvisioningPayload(result: Any?): ByteArray? {
        if (result !is Map<*, *>) {
            Log.e(TAG, "Expected Map from Flutter but got ${result?.javaClass}")
            return null
        }

        val map = result as Map<String, Any?>
        val keyId = map["keyId"] as? String ?: run {
            Log.e(TAG, "Missing keyId in provisioning data")
            return null
        }
        val pubB64 = map["publicKey"] as? String ?: run {
            Log.e(TAG, "Missing publicKey in provisioning data")
            return null
        }
        val certB64 = map["certChain"] as? String ?: ""

        val keyIdBytes = keyId.toByteArray(Charsets.UTF_8)
        if (keyIdBytes.size > 255) {
            Log.e(TAG, "keyId too long (${keyIdBytes.size} bytes)")
            return null
        }

        val pubBytes = try {
            Base64.decode(pubB64, Base64.DEFAULT)
        } catch (e: IllegalArgumentException) {
            Log.e(TAG, "publicKey base64 decode failed", e)
            return null
        }
        if (pubBytes.size != 65 || pubBytes[0] != 0x04.toByte()) {
            Log.e(TAG, "publicKey must be uncompressed P-256 (65 bytes, starts with 0x04). Got len=${pubBytes.size}")
            return null
        }

        val certBytes = if (certB64.isNotEmpty()) {
            try {
                Base64.decode(certB64, Base64.DEFAULT)
            } catch (e: IllegalArgumentException) {
                Log.e(TAG, "certChain base64 decode failed", e)
                return null
            }
        } else {
            ByteArray(0)
        }
        if (certBytes.size > 0xFFFF) {
            Log.e(TAG, "certChain too long (${certBytes.size} bytes)")
            return null
        }

        val payload = ByteArrayOutputStream()
        payload.write(keyIdBytes.size)
        payload.write(keyIdBytes)
        payload.write(pubBytes)
        payload.write(certBytes.size and 0xFF)
        payload.write((certBytes.size shr 8) and 0xFF)
        if (certBytes.isNotEmpty()) {
            payload.write(certBytes)
        }

        return payload.toByteArray()
    }
}

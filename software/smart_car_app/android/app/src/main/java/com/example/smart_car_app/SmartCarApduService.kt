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
        // Example AID (F00102030405). Replace with your production AID.
        private val AID = byteArrayOf(
            0xF0.toByte(), 0x01, 0x02, 0x03, 0x04, 0x05
        )

        private const val CHANNEL_NAME = "smartcar.hce"
        private val SW_SUCCESS = byteArrayOf(0x90.toByte(), 0x00)
        private val SW_UNKNOWN = byteArrayOf(0x6A.toByte(), 0x80.toByte())
    }

    private var engine: FlutterEngine? = null
    private var methodChannel: MethodChannel? = null

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "HCE service created")
    }

    override fun processCommandApdu(commandApdu: ByteArray?, extras: Bundle?): ByteArray {
        if (commandApdu == null) return SW_UNKNOWN
        Log.d(TAG, "APDU IN: ${commandApdu.joinToString(" ") { String.format("%02X", it) }}")

        // Simple APDU decoding:
        // SELECT AID: 00 A4 04 00 Lc <AID>
        if (isSelectAid(commandApdu)) {
            val aid = extractAid(commandApdu)
            return if (aid != null && aid.contentEquals(AID)) {
                Log.i(TAG, "SELECT AID matched")
                SW_SUCCESS
            } else {
                Log.w(TAG, "SELECT AID mismatch")
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
    }

    override fun onDeactivated(reason: Int) {
        Log.i(TAG, "HCE deactivated, reason=$reason")
    }

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

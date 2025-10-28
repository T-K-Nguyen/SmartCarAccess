package com.example.smart_car_app

import android.nfc.cardemulation.HostApduService
import android.os.Bundle
import android.util.Log
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.dart.DartExecutor
import io.flutter.plugin.common.MethodChannel
import java.nio.charset.Charset
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

        // GET DATA (example): 00 CA 00 00
        if (isGetData(commandApdu)) {
            val data = getProvisioningDataFromFlutter()
            return if (data != null) {
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
                        // Expect Map<String, Any?> or a String (JSON)
                        val json = when (result) {
                            is Map<*, *> -> mapToJson(result as Map<String, Any?>)
                            is String -> result
                            else -> null
                        }
                        if (json != null) {
                            resultBytes = json.toByteArray(Charset.forName("UTF-8"))
                        }
                    } catch (e: Exception) {
                        Log.e(TAG, "Error parsing provisioning data: ${e.message}", e)
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

        // Wait up to 2 seconds for Dart to respond
        latch.await(2, TimeUnit.SECONDS)
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

    private fun mapToJson(map: Map<String, Any?>): String {
        // Minimal JSON building to avoid adding deps
        val sb = StringBuilder()
        sb.append('{')
        var first = true
        for ((k, v) in map) {
            if (!first) sb.append(',')
            first = false
            sb.append('"').append(escape(k)).append('"').append(':')
            sb.append(valueToJson(v))
        }
        sb.append('}')
        return sb.toString()
    }

    private fun valueToJson(v: Any?): String {
        return when (v) {
            null -> "null"
            is Number, is Boolean -> v.toString()
            is Map<*, *> -> mapToJson(v as Map<String, Any?>)
            is Iterable<*> -> {
                val sb = StringBuilder("[")
                var first = true
                for (item in v) {
                    if (!first) sb.append(',')
                    first = false
                    sb.append(valueToJson(item))
                }
                sb.append(']')
                sb.toString()
            }
            else -> '"' + escape(v.toString()) + '"'
        }
    }

    private fun escape(s: String): String {
        return s
            .replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\n", "\\n")
            .replace("\r", "\\r")
            .replace("\t", "\\t")
    }
}

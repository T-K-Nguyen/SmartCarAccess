package com.example.smart_car_app

import android.content.pm.PackageManager
import android.nfc.NfcAdapter
import android.nfc.cardemulation.CardEmulation
import android.os.Bundle
import android.util.Log
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import com.smartcaraccess.KeystoreBridge
import com.smartcar.phaseb.HandshakeChannel

class MainActivity : FlutterActivity() {
    companion object {
        private const val TAG = "MainActivity"
        private const val CHANNEL = "smartcar/keystore"
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        // Register Phase B handshake channel (Android Keystore-backed ECDSA signing)
        HandshakeChannel.register(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            try {
                when (call.method) {
                    "ensurePhaseAKey" -> {
                        val ok = KeystoreBridge.ensurePhaseAKey()
                        result.success(ok)
                    }
                    "getPhaseAPublicKey65" -> {
                        val pub = KeystoreBridge.getPhaseAPublicKey65()
                        result.success(pub)
                    }
                    "signPhaseA" -> {
                        val args = call.arguments as ByteArray
                        val sig = KeystoreBridge.signPhaseA(args)
                        result.success(sig)
                    }
                    "signPhaseAData" -> {
                        val args = call.arguments as ByteArray
                        val sig = KeystoreBridge.signPhaseAData(args)
                        result.success(sig)
                    }
                    else -> result.notImplemented()
                }
            } catch (e: Exception) {
                Log.e(TAG, "Keystore channel error", e)
                result.error("KEYSTORE_ERROR", e.message, null)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Check for HCE support as required by Android documentation
        if (packageManager.hasSystemFeature(PackageManager.FEATURE_NFC_HOST_CARD_EMULATION)) {
            Log.i(TAG, "✅ HCE feature is supported on this device")
            
            val nfcAdapter = NfcAdapter.getDefaultAdapter(this)
            if (nfcAdapter != null && nfcAdapter.isEnabled) {
                Log.i(TAG, "✅ NFC is enabled and ready")
                Log.i(TAG, "✅ HCE service (ProvisioningHostApduService) is active via manifest")
                Log.i(TAG, "   App can be in foreground or background - HCE works independently")
                
                // NOTE: Do NOT use setPreferredService for HCE
                // HCE services work automatically when declared in manifest
                // setPreferredService is for foreground dispatch, not HCE
            } else {
                Log.w(TAG, "⚠️ NFC is not available or disabled")
            }
        } else {
            Log.e(TAG, "❌ HCE feature not supported on this device")
        }
    }

    override fun onResume() {
        super.onResume()
        
        // CRITICAL: Keep HCE service active even when app is in foreground
        // Do NOT unset in onPause - let Android handle HCE lifecycle
        val nfcAdapter = NfcAdapter.getDefaultAdapter(this)
        if (nfcAdapter?.isEnabled == true) {
            val cardEmulation = CardEmulation.getInstance(nfcAdapter)
            val componentName = android.content.ComponentName(this, ProvisioningHostApduService::class.java)
            
            try {
                // setPreferredService only affects foreground dispatch, not HCE
                // For HCE to work in foreground, service must be in manifest with correct category
                Log.d(TAG, "Activity resumed - HCE service remains active")
            } catch (e: Exception) {
                Log.w(TAG, "Error in onResume: ${e.message}")
            }
        }
    }
    
    override fun onPause() {
        super.onPause()
        
        // DO NOT unset HCE service - it should remain active
        // HCE works independently of activity lifecycle
        Log.d(TAG, "Activity paused - HCE service continues running")
    }
}

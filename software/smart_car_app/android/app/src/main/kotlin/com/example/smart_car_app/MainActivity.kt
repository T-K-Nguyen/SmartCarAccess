package com.example.smart_car_app

import android.content.pm.PackageManager
import android.nfc.NfcAdapter
import android.nfc.cardemulation.CardEmulation
import android.os.Bundle
import android.util.Log
import io.flutter.embedding.android.FlutterActivity

class MainActivity : FlutterActivity() {
    companion object {
        private const val TAG = "MainActivity"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Check for HCE support as required by Android documentation
        if (packageManager.hasSystemFeature(PackageManager.FEATURE_NFC_HOST_CARD_EMULATION)) {
            Log.i(TAG, "✅ HCE feature is supported on this device")
            
            val nfcAdapter = NfcAdapter.getDefaultAdapter(this)
            if (nfcAdapter != null && nfcAdapter.isEnabled) {
                Log.i(TAG, "✅ NFC is enabled and ready")
                
                // Set our HCE service as preferred when app is in foreground
                val cardEmulation = CardEmulation.getInstance(nfcAdapter)
                val componentName = android.content.ComponentName(this, ProvisioningHostApduService::class.java)
                
                try {
                    cardEmulation.setPreferredService(this, componentName)
                    Log.i(TAG, "✅ HCE service set as preferred for foreground activity")
                } catch (e: Exception) {
                    Log.w(TAG, "Could not set preferred HCE service: ${e.message}")
                }
            } else {
                Log.w(TAG, "⚠️ NFC is not available or disabled")
            }
        } else {
            Log.e(TAG, "❌ HCE feature not supported on this device")
        }
    }

    override fun onResume() {
        super.onResume()
        
        // Re-set preferred service when activity resumes
        val nfcAdapter = NfcAdapter.getDefaultAdapter(this)
        if (nfcAdapter?.isEnabled == true) {
            val cardEmulation = CardEmulation.getInstance(nfcAdapter)
            val componentName = android.content.ComponentName(this, ProvisioningHostApduService::class.java)
            
            try {
                cardEmulation.setPreferredService(this, componentName)
                Log.d(TAG, "HCE service re-set as preferred on resume")
            } catch (e: Exception) {
                Log.w(TAG, "Could not re-set preferred HCE service: ${e.message}")
            }
        }
    }
    
    override fun onPause() {
        super.onPause()
        
        // Clear preferred service when activity pauses
        val nfcAdapter = NfcAdapter.getDefaultAdapter(this)
        if (nfcAdapter?.isEnabled == true) {
            val cardEmulation = CardEmulation.getInstance(nfcAdapter)
            try {
                cardEmulation.unsetPreferredService(this)
                Log.d(TAG, "HCE preferred service unset on pause")
            } catch (e: Exception) {
                Log.w(TAG, "Could not unset preferred HCE service: ${e.message}")
            }
        }
    }
}

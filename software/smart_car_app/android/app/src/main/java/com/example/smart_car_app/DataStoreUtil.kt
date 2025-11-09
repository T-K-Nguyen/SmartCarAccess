package com.example.smart_car_app

import android.content.Context
import android.content.SharedPreferences
import java.security.SecureRandom

object DataStoreUtil {
    private const val PREFS = "smart_car_uid"
    private const val KEY_UID = "uid_hex"

    fun getOrCreate4ByteUid(context: Context): ByteArray {
        val prefs: SharedPreferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val existingHex = prefs.getString(KEY_UID, null)
        if (!existingHex.isNullOrEmpty()) {
            return ByteArrayHexUtil.hexToBytes(existingHex)
        }
        val rnd = SecureRandom()
        val uid = ByteArray(4)
        rnd.nextBytes(uid)
        val hex = uid.joinToString("") { String.format("%02X", it) }
        prefs.edit().putString(KEY_UID, hex).apply()
        return uid
    }
}
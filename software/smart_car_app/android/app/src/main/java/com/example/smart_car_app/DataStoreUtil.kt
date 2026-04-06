package com.example.smart_car_app

import android.content.Context
import android.content.SharedPreferences
import java.security.SecureRandom

object DataStoreUtil {
    private const val PREFS = "smart_car_uid"
    private const val KEY_UID = "uid_hex"
    private const val KEY_PROV_VEHICLE_ID = "prov_vehicle_id_hex"
    private const val KEY_PROV_VEHICLE_PUB = "prov_vehicle_pub_hex"

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

    fun saveProvisioningVehicleBinding(
        context: Context,
        vehicleId: ByteArray,
        vehiclePub: ByteArray,
    ): Boolean {
        if (vehicleId.size != 8 || vehiclePub.size != 65) return false
        val prefs: SharedPreferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val vehicleIdHex = vehicleId.joinToString("") { String.format("%02X", it) }
        val vehiclePubHex = vehiclePub.joinToString("") { String.format("%02X", it) }
        return prefs.edit()
            .putString(KEY_PROV_VEHICLE_ID, vehicleIdHex)
            .putString(KEY_PROV_VEHICLE_PUB, vehiclePubHex)
            .commit()
    }

    fun getProvisionedVehicleId(context: Context): ByteArray? {
        val prefs: SharedPreferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val hex = prefs.getString(KEY_PROV_VEHICLE_ID, null) ?: return null
        return try {
            ByteArrayHexUtil.hexToBytes(hex)
        } catch (_: Exception) {
            null
        }
    }

    fun getProvisionedVehiclePub(context: Context): ByteArray? {
        val prefs: SharedPreferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val hex = prefs.getString(KEY_PROV_VEHICLE_PUB, null) ?: return null
        return try {
            ByteArrayHexUtil.hexToBytes(hex)
        } catch (_: Exception) {
            null
        }
    }
}
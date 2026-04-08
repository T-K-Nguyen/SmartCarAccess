package com.example.smart_car_app

import android.content.Context
import android.content.SharedPreferences
import java.security.SecureRandom

object DataStoreUtil {
    private const val PREFS = "smart_car_uid"
    private const val KEY_UID = "uid_hex"
    private const val KEY_PROV_VEHICLE_ID = "prov_vehicle_id_hex"
    private const val KEY_PROV_VEHICLE_PUB = "prov_vehicle_pub_hex"
    private const val KEY_PROV_WRITE_DATA_HEX = "prov_write_data_hex"
    private const val KEY_PROV_UPDATED_AT_MS = "prov_updated_at_ms"

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
        val writeDataPayload = ByteArray(2 + 8 + 2 + 65)
        var offset = 0
        writeDataPayload[offset++] = 0x80.toByte()
        writeDataPayload[offset++] = 0x08.toByte()
        System.arraycopy(vehicleId, 0, writeDataPayload, offset, vehicleId.size)
        offset += vehicleId.size
        writeDataPayload[offset++] = 0x81.toByte()
        writeDataPayload[offset++] = 0x41.toByte()
        System.arraycopy(vehiclePub, 0, writeDataPayload, offset, vehiclePub.size)
        val writeDataPayloadHex = writeDataPayload.joinToString("") { String.format("%02X", it) }
        val updatedAtMs = System.currentTimeMillis()
        return prefs.edit()
            .putString(KEY_PROV_VEHICLE_ID, vehicleIdHex)
            .putString(KEY_PROV_VEHICLE_PUB, vehiclePubHex)
            .putString(KEY_PROV_WRITE_DATA_HEX, writeDataPayloadHex)
            .putLong(KEY_PROV_UPDATED_AT_MS, updatedAtMs)
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

    fun getProvisionedWriteDataPayload(context: Context): ByteArray? {
        val prefs: SharedPreferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val hex = prefs.getString(KEY_PROV_WRITE_DATA_HEX, null) ?: return null
        return try {
            val payload = ByteArrayHexUtil.hexToBytes(hex)
            if (payload.size == 77) payload else null
        } catch (_: Exception) {
            null
        }
    }

    fun getProvisionedUpdatedAtMs(context: Context): Long? {
        val prefs: SharedPreferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (!prefs.contains(KEY_PROV_UPDATED_AT_MS)) return null
        return prefs.getLong(KEY_PROV_UPDATED_AT_MS, 0L)
    }

    fun clearProvisioningVehicleBinding(context: Context): Boolean {
        val prefs: SharedPreferences = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        return prefs.edit()
            .remove(KEY_PROV_VEHICLE_ID)
            .remove(KEY_PROV_VEHICLE_PUB)
            .remove(KEY_PROV_WRITE_DATA_HEX)
            .remove(KEY_PROV_UPDATED_AT_MS)
            .commit()
    }
}
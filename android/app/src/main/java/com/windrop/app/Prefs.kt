package com.windrop.app

import android.content.Context
import android.os.Build
import android.provider.Settings
import java.util.UUID

class Prefs(context: Context) {
    private val app = context.applicationContext
    private val sp = app.getSharedPreferences("windrop", Context.MODE_PRIVATE)

    /** Master switch (Quick Settings tile / app switch): service, pill and receiving. */
    var enabled: Boolean
        get() = sp.getBoolean("enabled", false)
        set(v) = sp.edit().putBoolean("enabled", v).apply()

    var lastPcName: String
        get() = sp.getString("last_pc_name", "") ?: ""
        set(v) = sp.edit().putString("last_pc_name", v).apply()


    /** The PC's Wi-Fi Direct name as sent in its BLE beacon (may be cut short). */
    var pcP2pName: String
        get() = sp.getString("pc_p2p_name", "") ?: ""
        set(v) = sp.edit().putString("pc_p2p_name", v).apply()

    /** Wi-Fi Direct device address of the PC, learned on the first successful match. */
    var pcP2pAddress: String
        get() = sp.getString("pc_p2p_addr", "") ?: ""
        set(v) = sp.edit().putString("pc_p2p_addr", v).apply()

    /** The paired PC's id, set by pairing (after the user confirmed the check number on the PC). */
    var pairedPcId: String
        get() = sp.getString("paired_pc_id", "") ?: ""
        set(v) = sp.edit().putString("paired_pc_id", v).apply()

    @Volatile private var keyCache: ByteArray? = null

    /** 32-byte key from pairing, kept encrypted with an Android Keystore key. Null: not paired. */
    var pairKey: ByteArray?
        get() = keyCache ?: sp.getString("pair_key", null)?.let { Crypto.unprotect(it) }?.also { keyCache = it }
        set(v) {
            keyCache = v
            sp.edit().putString("pair_key", v?.let { Crypto.protect(it) }).apply()
        }

    val paired: Boolean get() = pairKey != null && pairedPcId.isNotEmpty()

    val deviceId: String
        get() = sp.getString("device_id", null) ?: UUID.randomUUID().toString().replace("-", "").take(16)
            .also { sp.edit().putString("device_id", it).apply() }

    val deviceName: String
        get() = Settings.Global.getString(app.contentResolver, Settings.Global.DEVICE_NAME)
            ?: "${Build.MANUFACTURER} ${Build.MODEL}"

    val pcLabel: String get() = lastPcName.ifEmpty { "PC" }
}

package com.windrop.app

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build

/**
 * Unpaired ("nearby") transfers, phone side of the Bluetooth part. While the user lets nearby PCs
 * send, the phone advertises "WV" + flags + tag[4] + name; a PC with something for it answers with
 * "WC" + that tag (see BleWatcher), and the phone then joins that PC like it joins its own. The tag
 * is SHA-256("WV" + device id)[0..4], so a PC can tell its own paired phone apart.
 */
class NearbyAdvertiser(private val ctx: Context, private val prefs: Prefs) {
    companion object {
        fun tag(deviceId: String): ByteArray = Crypto.sha256(("WV$deviceId").toByteArray()).copyOf(4)

        fun hasPermission(ctx: Context) = Build.VERSION.SDK_INT < 31 ||
            ctx.checkSelfPermission(Manifest.permission.BLUETOOTH_ADVERTISE) == PackageManager.PERMISSION_GRANTED
    }

    val tag: ByteArray by lazy { tag(prefs.deviceId) }
    private var on = false

    private val cb = object : AdvertiseCallback() {
        override fun onStartSuccess(settingsInEffect: AdvertiseSettings?) { Logs.i("nearby: visible to nearby PCs") }
        override fun onStartFailure(errorCode: Int) { Logs.w("nearby: can't advertise ($errorCode)"); on = false }
    }

    @SuppressLint("MissingPermission")
    fun start(wifiOff: Boolean): Boolean {
        if (on) return true
        if (!hasPermission(ctx)) { Logs.w("nearby: no Bluetooth advertise permission"); return false }
        val adv = ctx.getSystemService(BluetoothManager::class.java)?.adapter?.takeIf { it.isEnabled }?.bluetoothLeAdvertiser
            ?: run { Logs.w("nearby: Bluetooth is off or can't advertise"); return false }
        // A legacy advert has room for 17 bytes of name after our 7; cut on a character boundary.
        var name = prefs.deviceName
        while (name.toByteArray().size > 17) name = name.dropLast(1)
        val payload = "WV".toByteArray() + byteArrayOf(if (wifiOff) 1 else 0) + tag + name.toByteArray()
        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_BALANCED)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_MEDIUM)
            .setConnectable(false)
            .setTimeout(0)
            .build()
        val data = AdvertiseData.Builder()
            .setIncludeDeviceName(false)
            .setIncludeTxPowerLevel(false)
            .addManufacturerData(Proto.BLE_COMPANY_ID, payload)
            .build()
        return runCatching { adv.startAdvertising(settings, data, cb); on = true; true }
            .getOrElse { Logs.w("nearby: advertise failed", it); false }
    }

    @SuppressLint("MissingPermission")
    fun stop() {
        if (!on) return
        on = false
        runCatching { ctx.getSystemService(BluetoothManager::class.java)?.adapter?.bluetoothLeAdvertiser?.stopAdvertising(cb) }
        Logs.i("nearby: no longer visible")
    }
}

package com.windrop.app

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.wifi.WifiManager
import android.os.Build
import android.provider.Settings

/**
 * The two switches WinDrop depends on. Wi-Fi carries every transfer (the Wi-Fi Direct link);
 * Bluetooth is how the phone finds a PC that is pairing and how the PC wakes the phone.
 */
object Radios {
    fun wifiOn(ctx: Context) = ctx.getSystemService(WifiManager::class.java)?.isWifiEnabled == true

    fun bluetoothOn(ctx: Context) = ctx.getSystemService(BluetoothManager::class.java)?.adapter?.isEnabled == true

    /** "Wi-Fi is off" / "Bluetooth is off" / "Wi-Fi and Bluetooth are off", or null when both are on. */
    fun offText(ctx: Context): String? {
        val wifi = !wifiOn(ctx)
        val bt = !bluetoothOn(ctx)
        return when {
            wifi && bt -> "Wi-Fi and Bluetooth are off"
            wifi -> "Wi-Fi is off"
            bt -> "Bluetooth is off"
            else -> null
        }
    }

    /** Android's Wi-Fi panel (apps can't switch Wi-Fi themselves since Android 10). */
    fun openWifi(ctx: Context) {
        val i = Intent(Settings.Panel.ACTION_WIFI).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        runCatching { ctx.startActivity(i) }.onFailure {
            runCatching { ctx.startActivity(Intent(Settings.ACTION_WIFI_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
        }
    }

    /** Asks to switch Bluetooth on (one tap in the system dialog), else opens its settings. */
    fun openBluetooth(activity: Activity) {
        val allowed = Build.VERSION.SDK_INT < 31 ||
            activity.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
        if (allowed && runCatching { activity.startActivityForResult(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), 2) }.isSuccess) return
        runCatching { activity.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) }
    }
}

package com.windrop.app

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.ContentValues
import android.content.Context
import android.content.pm.PackageManager
import android.location.LocationManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Environment
import android.os.PowerManager
import android.provider.MediaStore
import android.provider.Settings
import java.net.Inet4Address
import java.net.NetworkInterface
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Save logs: a report on the things that usually stop WinDrop (radios, permissions, VPNs, network
 * interfaces) followed by the log file, written to Downloads/WinDrop so it can be shared.
 */
object Diagnostics {
    fun report(ctx: Context): String = buildString {
        val prefs = Prefs(ctx)
        val svc = WinDropService.instance
        appendLine("WinDrop ${Crypto.appVersion} (protocol ${Crypto.PROTO}) on ${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE} (SDK ${Build.VERSION.SDK_INT})")
        appendLine("Phone name: ${prefs.deviceName}")
        appendLine("On: ${prefs.enabled}, service running: ${svc != null}")
        appendLine("Paired: ${prefs.paired}, PC: ${prefs.lastPcName.ifEmpty { "-" }}, PC Wi-Fi Direct name: ${prefs.pcP2pName.ifEmpty { "-" }}")
        appendLine("Link to PC: ${svc?.p2p?.connectedIp ?: "none"}")
        appendLine()
        appendLine("== Radios")
        val wifi = ctx.getSystemService(WifiManager::class.java)
        appendLine("Wi-Fi on: ${wifi?.isWifiEnabled}, Wi-Fi Direct supported: ${ctx.packageManager.hasSystemFeature(PackageManager.FEATURE_WIFI_DIRECT)}")
        val bt = ctx.getSystemService(BluetoothManager::class.java)?.adapter
        appendLine("Bluetooth on: ${runCatching { bt?.isEnabled }.getOrNull()}, BLE supported: ${ctx.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)}")
        val loc = ctx.getSystemService(LocationManager::class.java)
        appendLine("Location on: ${runCatching { loc?.isLocationEnabled }.getOrNull()}")
        appendLine()
        appendLine("== Permissions")
        val perms = listOf(
            Manifest.permission.POST_NOTIFICATIONS, Manifest.permission.NEARBY_WIFI_DEVICES, Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.ACCESS_FINE_LOCATION,
        )
        for (p in perms) appendLine("${p.substringAfterLast('.')}: ${if (ctx.checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED) "granted" else "not granted"}")
        appendLine("Draw over other apps: ${Settings.canDrawOverlays(ctx)}")
        appendLine("Battery unrestricted: ${ctx.getSystemService(PowerManager::class.java)?.isIgnoringBatteryOptimizations(ctx.packageName)}")
        appendLine("Install updates allowed: ${ctx.packageManager.canRequestPackageInstalls()}")
        appendLine()
        appendLine("== Networks")
        val cm = ctx.getSystemService(ConnectivityManager::class.java)
        @Suppress("DEPRECATION")
        for (n in cm.allNetworks) {
            val caps = cm.getNetworkCapabilities(n) ?: continue
            val kinds = listOf(
                NetworkCapabilities.TRANSPORT_WIFI to "Wi-Fi", NetworkCapabilities.TRANSPORT_CELLULAR to "mobile",
                NetworkCapabilities.TRANSPORT_VPN to "VPN", NetworkCapabilities.TRANSPORT_ETHERNET to "ethernet",
            ).filter { caps.hasTransport(it.first) }.joinToString("+") { it.second }
            val addrs = cm.getLinkProperties(n)?.linkAddresses?.joinToString { it.toString() } ?: ""
            appendLine("$kinds: $addrs${if (n == cm.activeNetwork) " (default)" else ""}")
        }
        appendLine(interfaces())
        appendLine("Private DNS: ${Settings.Global.getString(ctx.contentResolver, "private_dns_mode") ?: "-"}")
        appendLine()
        appendLine("== Log")
        append(Logs.fileText())
    }

    /** Network interfaces with IPv4 addresses (the Wi-Fi Direct one is usually p2p-…). */
    fun interfaces(): String = runCatching {
        NetworkInterface.getNetworkInterfaces().toList().filter { it.isUp }.joinToString("\n") { ni ->
            val v4 = ni.inetAddresses.toList().filterIsInstance<Inet4Address>().joinToString { it.hostAddress ?: "" }
            "interface ${ni.name}: ${v4.ifEmpty { "(no IPv4)" }}"
        }
    }.getOrElse { "interfaces: $it" }

    /** True if a VPN app is routing this phone's traffic (it can swallow the link to the PC). */
    fun vpnActive(ctx: Context): Boolean {
        val cm = ctx.getSystemService(ConnectivityManager::class.java)
        @Suppress("DEPRECATION")
        return cm.allNetworks.any { cm.getNetworkCapabilities(it)?.hasTransport(NetworkCapabilities.TRANSPORT_VPN) == true }
    }

    /** Writes the report to Downloads/WinDrop; returns the file name, or null. Call off the main thread. */
    fun save(ctx: Context): String? {
        val name = "WinDrop-phone-log " + SimpleDateFormat("yyyy-MM-dd HHmmss", Locale.US).format(Date()) + ".txt"
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, name)
            put(MediaStore.Downloads.MIME_TYPE, "text/plain")
            put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/WinDrop")
        }
        val uri = ctx.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: return null
        return runCatching {
            ctx.contentResolver.openOutputStream(uri)!!.use { it.write(report(ctx).toByteArray()) }
            Logs.i("logs: saved $name")
            name
        }.getOrNull()
    }
}

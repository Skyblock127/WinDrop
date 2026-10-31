package com.windrop.app

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build

/**
 * Listens for the PC's Bluetooth LE beacons (formats below): the paired PC calling us, or a PC in
 * its pairing window telling a pairing phone its Wi-Fi Direct name. The radio does the filtering;
 * the app only wakes on a hit.
 */
class BleWatcher(
    private val ctx: Context,
    private val prefs: Prefs,
    private val onBeacon: (pending: Boolean, pcName: String, pcWifiOff: Boolean) -> Unit,
) {
    // One scan that runs for as long as WinDrop is on. Android silently stops delivering results to
    // apps that start scans more than 5 times in 30 s, so it is never restarted for a new sighting;
    // repeated sightings are de-duplicated here instead.
    @Volatile private var lastName = ""
    @Volatile private var lastWifiOff = false
    @Volatile private var lastPendingAt = 0L

    // "WD" + tag[6] + flag: the paired PC wants us. The tag is HMAC(pairing key, 10-minute
    // window); only we can check it, and it changes, so the beacon can't be used to track the PC.
    // "WP" + flags + name: a PC in its pairing window; tells a pairing phone its name. Flags bit 0:
    // the PC's Wi-Fi is off (PCs before 0.2.6 always send 0).
    private fun dispatch(r: ScanResult) {
        val d = r.scanRecord?.getManufacturerSpecificData(Proto.BLE_COMPANY_ID) ?: return
        if (d.size < 3) return
        val kind = String(d, 0, 2, Charsets.US_ASCII)
        var name = ""
        var wifiOff = false
        var pending = false
        when (kind) {
            "WP" -> {
                name = String(d, 3, d.size - 3, Charsets.UTF_8).trim { it <= ' ' }
                wifiOff = d[2].toInt() and 1 != 0
            }
            "WD" -> {
                if (d.size < 9 || d[8].toInt() != 1) return
                val key = prefs.pairKey ?: return
                val tag = d.copyOfRange(2, 8)
                val w = Crypto.beaconWindow()
                // Allow a window either side for clock differences between PC and phone.
                pending = (w - 1..w + 1).any { Crypto.same(Crypto.beaconTag(key, it), tag) }
                if (!pending) return
            }
            else -> return
        }
        val now = android.os.SystemClock.elapsedRealtime()
        val newName = name.isNotEmpty() && (name != lastName || wifiOff != lastWifiOff)
        val firePending = pending && now - lastPendingAt > PENDING_REPEAT_MS
        if (!newName && !firePending) return
        if (newName) { lastName = name; lastWifiOff = wifiOff }
        if (firePending) lastPendingAt = now
        onBeacon(firePending, name, wifiOff)
    }

    /** Treat the next sightings as new (link closed, or pairing just started). */
    fun reset() { lastPendingAt = 0L; lastName = ""; lastWifiOff = false }
    private var scanning = false
    private var saidOff = false

    private val cb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) = dispatch(result)
        override fun onBatchScanResults(results: MutableList<ScanResult>) { results.forEach { dispatch(it) } }
        override fun onScanFailed(errorCode: Int) { Logs.w("ble scan failed: $errorCode"); scanning = false }
    }

    private fun hasPermission() =
        if (Build.VERSION.SDK_INT >= 31) listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
            .all { ctx.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }
        else ctx.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED

    /** Starts the scan; safe to call repeatedly. */
    @SuppressLint("MissingPermission")
    fun start() {
        if (!hasPermission()) return
        if (scanning) return
        stop()
        val scanner = ctx.getSystemService(BluetoothManager::class.java)?.adapter?.takeIf { it.isEnabled }?.bluetoothLeScanner ?: run {
            if (!saidOff) Logs.w("ble: bluetooth off")  // start() runs on every screen-on: say it once
            saidOff = true
            return
        }
        saidOff = false
        fun prefix(p: String) = p.toByteArray().let { data ->
            ScanFilter.Builder().setManufacturerData(Proto.BLE_COMPANY_ID, data, ByteArray(data.size) { -1 }).build()
        }
        val filters = listOf(prefix("WD"), prefix("WP"))
        val settings = ScanSettings.Builder()
            // Balanced: notices the PC within ~1-2 s instead of ~5 s, still a filtered hardware scan.
            .setScanMode(ScanSettings.SCAN_MODE_BALANCED)
            .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
            .build()
        runCatching { scanner.startScan(filters, settings, cb) }
            .onSuccess { scanning = true; Logs.i("ble: watching for PC") }
            .onFailure { Logs.w("ble: start failed", it) }
    }

    @SuppressLint("MissingPermission")
    fun stop() {
        if (!scanning) return
        scanning = false
        runCatching { ctx.getSystemService(BluetoothManager::class.java)?.adapter?.bluetoothLeScanner?.stopScan(cb) }
    }

    companion object {
        /** A beacon still "pending" this long after we acted on it is a new request. */
        private const val PENDING_REPEAT_MS = 20_000L
    }
}

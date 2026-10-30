package com.windrop.app

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast

/**
 * Setup screen: on/off, pairing, the permissions WinDrop still needs, and anything waiting to send.
 * Dark, built in code, no dependencies.
 */
class MainActivity : Activity() {
    private lateinit var prefs: Prefs
    private lateinit var outbox: OutboxManager
    private lateinit var master: Switch
    private lateinit var status: TextView
    private lateinit var statusDot: View
    private lateinit var pcName: TextView
    private lateinit var pairBtn: Button
    private lateinit var setupCard: LinearLayout
    private lateinit var permBtn: Button
    private lateinit var overlayBtn: Button
    private lateinit var batteryBtn: Button
    private lateinit var outboxCard: LinearLayout
    private lateinit var outboxNote: TextView
    private lateinit var outboxList: LinearLayout
    private lateinit var updateNote: TextView
    private lateinit var updateBtn: Button
    private var updateState = UpdateState.IDLE
    private var updateProgress = 0
    private var rendering = false
    private val refresh: () -> Unit = { runOnUiThread { render() } }

    private val d get() = resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private fun rounded(color: Int, radiusDp: Int, strokeColor: Int = 0) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = radiusDp * d
        if (strokeColor != 0) setStroke(dp(1), strokeColor)
    }

    private fun label(t: String, size: Float, color: Int = TEXT, bold: Boolean = false) = TextView(this).apply {
        text = t
        textSize = size
        setTextColor(color)
        if (bold) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }

    private fun card(parent: LinearLayout) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        background = rounded(CARD, 18)
        setPadding(dp(18), dp(16), dp(18), dp(16))
        parent.addView(this, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(14)
        })
    }

    private fun button(t: String, parent: ViewGroup, filled: Boolean, onClick: () -> Unit) = Button(this).apply {
        text = t
        isAllCaps = false
        textSize = 15f
        setTextColor(if (filled) Color.WHITE else TEXT)
        background = rounded(if (filled) ACCENT else FIELD, 14)
        stateListAnimator = null
        setPadding(dp(16), 0, dp(16), 0)
        setOnClickListener { onClick() }
        val lp = if (parent is LinearLayout && parent.orientation == LinearLayout.HORIZONTAL)
            LinearLayout.LayoutParams(0, dp(48), 1f).apply { marginStart = dp(4); marginEnd = dp(4) }
        else LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48)).apply { topMargin = dp(10) }
        parent.addView(this, lp)
    }

    @SuppressLint("SetTextI18n", "UseSwitchCompatOrMaterialCode")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Logs.init(this)
        prefs = Prefs(this)
        outbox = OutboxManager(this)
        window.statusBarColor = BG
        window.navigationBarColor = BG

        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(20), dp(18), dp(28))
        }

        // ---- header: logo, name, status
        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        header.addView(ImageView(this).apply {
            setImageResource(R.mipmap.ic_launcher)
        }, LinearLayout.LayoutParams(dp(56), dp(56)))
        val titles = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(14), 0, 0, 0) }
        titles.addView(label("WinDrop", 26f, bold = true))
        val statusRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        statusDot = View(this)
        statusRow.addView(statusDot, LinearLayout.LayoutParams(dp(8), dp(8)).apply { marginEnd = dp(8) })
        status = label("", 14f, MUTED)
        statusRow.addView(status)
        titles.addView(statusRow)
        header.addView(titles, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        col.addView(header)

        // ---- on / off
        val power = card(col)
        master = Switch(this).apply {
            text = "On"
            textSize = 17f
            setTextColor(TEXT)
            thumbTintList = ColorStateList(arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()), intArrayOf(Color.WHITE, 0xFF9A9A9A.toInt()))
            trackTintList = ColorStateList(arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()), intArrayOf(ACCENT, 0xFF3A3B3F.toInt()))
            setOnCheckedChangeListener { _, on ->
                if (rendering) return@setOnCheckedChangeListener
                if (on) WinDropService.start(this@MainActivity) else WinDropService.stop(this@MainActivity)
                status.postDelayed({ render() }, 300)
            }
        }
        power.addView(master)
        button("Send clipboard to PC", power, filled = false) { startActivity(ClipboardSendActivity.intent(this)) }

        // ---- pairing
        val pair = card(col)
        pair.addView(label("PAIRED PC", 12f, MUTED, bold = true).apply { letterSpacing = 0.08f })
        pcName = label("", 17f).apply { setPadding(0, dp(4), 0, dp(12)) }
        pair.addView(pcName)
        pairBtn = button("Pair with PC", pair, filled = true) {
            if (pairingNow) return@button
            if (missingPairPermissions().isNotEmpty()) {
                // Without these the Bluetooth scan and Wi-Fi Direct search can't run: ask, then pair.
                pairAfterPermissions = true
                requestRuntimePermissions()
                return@button
            }
            requestRuntimePermissions()
            if (!prefs.enabled || WinDropService.instance == null) WinDropService.start(this)
            pairingNow = true
            renderPair()
            withService { svc ->
                var sasDialog: android.app.AlertDialog? = null
                svc.startPairing(onSas = { sas ->
                    sasDialog = android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                        .setTitle("Pairing with ${prefs.pcP2pName}")
                        .setMessage("Check that your PC shows the same number:\n\n        ${sas.take(3)} ${sas.drop(3)}\n\nThen click Yes on the PC.")
                        .setCancelable(false)
                        .show()
                }, onDone = { ok, msg ->
                    sasDialog?.dismiss()
                    pairingNow = false
                    render()
                    android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                        .setTitle(if (ok) "Paired" else "Not paired")
                        .setMessage(if (ok) "$msg. Transfers are encrypted from now on." else msg)
                        .setPositiveButton("OK", null)
                        .show()
                })
            }
        }

        // ---- only when something is missing
        setupCard = card(col)
        setupCard.addView(label("NEEDS YOUR OK", 12f, AMBER, bold = true).apply { letterSpacing = 0.08f })
        permBtn = button("Notifications, Nearby devices, Bluetooth", setupCard, filled = false) { requestRuntimePermissions() }
        overlayBtn = button("Show the pill over other apps", setupCard, filled = false) {
            startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")))
        }
        batteryBtn = button("Keep running in the background", setupCard, filled = false) {
            runCatching {
                @SuppressLint("BatteryLife")
                val i = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName"))
                startActivity(i)
            }
        }

        // ---- only when something couldn't be delivered
        outboxCard = card(col)
        outboxCard.addView(label("NOT SENT YET", 12f, AMBER, bold = true).apply { letterSpacing = 0.08f })
        outboxNote = label("", 13f, MUTED).apply { setPadding(0, dp(4), 0, dp(6)) }
        outboxCard.addView(outboxNote)
        outboxList = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        outboxCard.addView(outboxList)
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; setPadding(0, dp(8), 0, 0) }
        outboxCard.addView(row)
        button("Remove all", row, filled = false) { outbox.clear(); render() }
        button("Try again", row, filled = true) {
            if (!prefs.enabled) WinDropService.start(this)
            WinDropService.instance?.connectNow()
            Toast.makeText(this, "Connecting to ${prefs.pcLabel}\u2026", Toast.LENGTH_SHORT).show()
        }

        // ---- version and updates
        val upd = card(col)
        upd.addView(label("APP", 12f, MUTED, bold = true).apply { letterSpacing = 0.08f })
        updateNote = label("", 15f).apply { setPadding(0, dp(4), 0, 0) }
        upd.addView(updateNote)
        updateBtn = button("Check for updates", upd, filled = false) { onUpdateClicked() }
        val logRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; setPadding(0, dp(10), 0, 0) }
        upd.addView(logRow)
        button("Save logs", logRow, filled = false) {
            Toast.makeText(this, "Saving logs…", Toast.LENGTH_SHORT).show()
            Thread {
                val name = Diagnostics.save(this)
                runOnUiThread {
                    Toast.makeText(this, if (name != null) "Saved to Downloads/WinDrop/$name" else "Couldn't save the logs", Toast.LENGTH_LONG).show()
                }
            }.start()
        }
        button("Clear logs", logRow, filled = false) {
            Thread {
                Logs.clear()
                Logs.i("Logs cleared. WinDrop ${Crypto.appVersion}, paired: ${prefs.paired}")
                runOnUiThread { Toast.makeText(this, "Logs cleared. Try again, then Save logs.", Toast.LENGTH_LONG).show() }
            }.start()
        }

        setContentView(ScrollView(this).apply {
            setBackgroundColor(BG)
            fitsSystemWindows = true
            addView(col)
        })
        requestRuntimePermissions()
        WinDropService.publishShareShortcut(this)
        if (intent?.getBooleanExtra(EXTRA_UPDATE, false) == true) checkForUpdate()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        if (intent.getBooleanExtra(EXTRA_UPDATE, false)) checkForUpdate()
    }

    // ---- updates ----
    private enum class UpdateState { IDLE, CHECKING, LATEST, AVAILABLE, DOWNLOADING, FAILED }

    private fun onUpdateClicked() = when (updateState) {
        UpdateState.AVAILABLE -> installUpdate()
        UpdateState.CHECKING, UpdateState.DOWNLOADING -> Unit
        else -> checkForUpdate()
    }

    private fun checkForUpdate() {
        if (updateState == UpdateState.CHECKING || updateState == UpdateState.DOWNLOADING) return
        updateState = UpdateState.CHECKING
        renderUpdate()
        Thread {
            Updater.check(this, notify = false, force = true)
            runOnUiThread {
                updateState = if (Updater.available != null) UpdateState.AVAILABLE
                else if (getSharedPreferences("windrop", MODE_PRIVATE).getLong("update_checked", 0) > System.currentTimeMillis() - 60_000) UpdateState.LATEST
                else UpdateState.FAILED
                renderUpdate()
            }
        }.start()
    }

    private fun installUpdate() {
        val r = Updater.available ?: return
        if (!Updater.canInstall(this)) {
            Toast.makeText(this, "Allow WinDrop to install updates, then tap Update again", Toast.LENGTH_LONG).show()
            return
        }
        updateState = UpdateState.DOWNLOADING
        updateProgress = 0
        renderUpdate()
        Thread {
            try {
                val apk = Updater.download(this, r) { p -> if (p != updateProgress) { updateProgress = p; runOnUiThread { renderUpdate() } } }
                Updater.install(this, apk)
                runOnUiThread { updateState = UpdateState.AVAILABLE; renderUpdate() }
            } catch (e: Exception) {
                Logs.w("update failed", e)
                runOnUiThread {
                    updateState = UpdateState.AVAILABLE
                    renderUpdate()
                    Toast.makeText(this, "Update failed: ${e.message}", Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    @SuppressLint("SetTextI18n")
    private fun renderUpdate() {
        val r = Updater.available
        if (r != null && updateState == UpdateState.IDLE) updateState = UpdateState.AVAILABLE
        updateNote.text = when (updateState) {
            UpdateState.AVAILABLE, UpdateState.DOWNLOADING -> "WinDrop ${Crypto.appVersion} · version ${r?.version} is out"
            UpdateState.LATEST -> "WinDrop ${Crypto.appVersion} · up to date"
            UpdateState.FAILED -> "WinDrop ${Crypto.appVersion} · couldn't check (offline?)"
            else -> "WinDrop ${Crypto.appVersion}"
        }
        updateBtn.text = when (updateState) {
            UpdateState.CHECKING -> "Checking…"
            UpdateState.AVAILABLE -> "Update to ${r?.version}"
            UpdateState.DOWNLOADING -> "Downloading… $updateProgress%"
            else -> "Check for updates"
        }
        val filled = updateState == UpdateState.AVAILABLE
        updateBtn.setTextColor(if (filled) Color.WHITE else TEXT)
        updateBtn.background = rounded(if (filled) ACCENT else FIELD, 14)
        updateBtn.alpha = if (updateState == UpdateState.CHECKING || updateState == UpdateState.DOWNLOADING) 0.6f else 1f
    }

    private fun runtimePermissions(): List<String> = when {
        Build.VERSION.SDK_INT >= 33 -> listOf(
            Manifest.permission.POST_NOTIFICATIONS, Manifest.permission.NEARBY_WIFI_DEVICES,
            Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT,
        )
        Build.VERSION.SDK_INT >= 31 -> listOf(
            Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT,
        )
        else -> listOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    private fun missingPermissions() = runtimePermissions().filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }

    /** The ones pairing can't do without (notifications are optional). */
    private fun missingPairPermissions() = missingPermissions().filter { it != Manifest.permission.POST_NOTIFICATIONS }
    private var pairAfterPermissions = false

    private fun requestRuntimePermissions() {
        val missing = missingPermissions()
        if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), 1)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        WinDropService.instance?.onSetupChanged()
        render()
        if (pairAfterPermissions) {
            pairAfterPermissions = false
            if (missingPairPermissions().isEmpty()) pairBtn.performClick()
            else android.widget.Toast.makeText(this, "Pairing needs the Nearby devices and Bluetooth permissions.", android.widget.Toast.LENGTH_LONG).show()
        }
    }

    override fun onResume() {
        super.onResume()
        WinDropService.listeners += refresh
        outbox.listeners += refresh
        // Coming back from the overlay settings: attach the pill now that it's allowed.
        if (prefs.enabled && Settings.canDrawOverlays(this)) WinDropService.start(this)
        render()
    }

    override fun onPause() {
        WinDropService.listeners -= refresh
        outbox.listeners -= refresh
        super.onPause()
    }

    /** One queued item: what it is, its size, and a remove button. */
    private fun outboxRow(e: OutboxManager.Entry) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        background = rounded(FIELD, 12)
        setPadding(dp(14), dp(8), dp(4), dp(8))
        val what = if (e.isText) "\u201C" + (e.text ?: "").lineSequence().first().take(60) + "\u201D" else e.name
        val texts = LinearLayout(this@MainActivity).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(label(what, 15f).apply { maxLines = 1; ellipsize = android.text.TextUtils.TruncateAt.END })
        texts.addView(label(if (e.isText) "Text \u00b7 ${humanSize(e.size)}" else humanSize(e.size), 12f, MUTED))
        addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        addView(TextView(this@MainActivity).apply {
            text = "\u2715"
            textSize = 16f
            setTextColor(MUTED)
            gravity = Gravity.CENTER
            contentDescription = "Remove"
            setOnClickListener { outbox.remove(e.id); render() }
        }, LinearLayout.LayoutParams(dp(44), dp(44)))
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(6)
        }
    }


    private var pairingNow = false

    /** Runs [f] once the service is up (it was possibly just started). */
    private fun withService(tries: Int = 40, f: (WinDropService) -> Unit) {
        val s = WinDropService.instance
        when {
            s != null -> f(s)
            tries > 0 -> pairBtn.postDelayed({ withService(tries - 1, f) }, 100)
            else -> { pairingNow = false; renderPair(); Toast.makeText(this, "WinDrop didn't start", Toast.LENGTH_SHORT).show() }
        }
    }

    private fun renderPair() {
        pairBtn.isEnabled = !pairingNow
        pairBtn.alpha = if (pairingNow) 0.4f else 1f
        pairBtn.text = when {
            pairingNow -> "Pairing…"
            prefs.paired -> "Pair again"
            else -> "Pair with PC"
        }
    }

    @SuppressLint("SetTextI18n")
    private fun render() {
        val svc = WinDropService.instance
        rendering = true
        master.isChecked = prefs.enabled  // the tile / notification may have flipped it
        rendering = false
        val (text, color) = when {
            !prefs.enabled -> "Off" to 0xFF6B6D72.toInt()
            !prefs.paired -> "On · not paired" to AMBER
            svc?.p2p?.connectedIp != null -> "Connected to ${prefs.pcLabel}" to GREEN
            else -> "On · ready" to GREEN
        }
        status.text = text
        statusDot.background = rounded(color, 4)
        pcName.text = when {
            !prefs.paired -> "Not paired. On the PC: tray → Pair phone…, then tap Pair with PC here."
            prefs.lastPcName.isNotEmpty() -> prefs.lastPcName
            else -> "Waiting for your PC"
        }
        renderPair()

        val needPerm = missingPermissions().isNotEmpty()
        val needOverlay = !Settings.canDrawOverlays(this)
        val needBattery = !getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(packageName)
        permBtn.visibility = if (needPerm) View.VISIBLE else View.GONE
        overlayBtn.visibility = if (needOverlay) View.VISIBLE else View.GONE
        batteryBtn.visibility = if (needBattery) View.VISIBLE else View.GONE
        setupCard.visibility = if (needPerm || needOverlay || needBattery) View.VISIBLE else View.GONE

        val items = outbox.list()
        outboxCard.visibility = if (items.isEmpty()) View.GONE else View.VISIBLE
        outboxNote.text = "Couldn't reach ${prefs.pcLabel}. These go out by themselves the next time it connects."
        outboxList.removeAllViews()
        for (e in items) outboxList.addView(outboxRow(e))
        renderUpdate()
    }

    companion object {
        /** Open the app and check for an update right away (update notification, version mismatch). */
        const val EXTRA_UPDATE = "check_update"

        // Night palette: near-black, grey cards, the logo's blue. Solid fills only.
        private const val BG = 0xFF0E0F11.toInt()
        private const val CARD = 0xFF1A1B1E.toInt()
        private const val FIELD = 0xFF232428.toInt()
        private const val TEXT = 0xFFECEDEF.toInt()
        private const val MUTED = 0xFF8E9096.toInt()
        private const val ACCENT = 0xFF0E7CD4.toInt()
        private const val GREEN = 0xFF2BB673.toInt()
        private const val AMBER = 0xFFF5A524.toInt()
    }
}

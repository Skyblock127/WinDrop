package com.windrop.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.graphics.drawable.Icon
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.MediaStore
import android.provider.Settings
import android.view.DragEvent
import android.webkit.MimeTypeMap
import android.widget.Toast
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.File
import java.io.IOException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread

class WinDropService : Service() {
    companion object {
        private const val TAG = "WinDrop"
        const val ACTION_START = "com.windrop.app.START"
        const val ACTION_STOP = "com.windrop.app.STOP_SERVICE"
        const val ACTION_SEND = "com.windrop.app.SEND"
        const val ACTION_DECIDE = "com.windrop.app.DECIDE"
        const val EXTRA_OFFER = "offer_id"
        const val EXTRA_ACCEPT = "accept"
        const val SHARE_CATEGORY = "com.windrop.app.category.SEND_TO_PC"

        private const val CH_SERVICE = "service"
        private const val CH_OFFERS = "offers"
        private const val CH_RECEIVED = "received"
        private const val NOTIF_SERVICE = 1
        // Keep the on-demand link this long after the last transfer, so a quick second item goes
        // straight out instead of paying for a new connection.
        private const val LINK_IDLE_MS = 60_000L
        // How long pairing looks for a PC with "Pair phone…" open (that stays open 3 minutes).
        private const val PAIR_FIND_MS = 90_000L
        private const val NOTIF_OFFER = 2
        // A link to a nearby (unpaired) PC is let go this soon after its last transfer.
        private const val OPEN_IDLE_MS = 8_000L
        private const val OWN_PC = "\u0000own"  // the chooser's id for our paired PC
        /** "Let nearby PCs send" lasts this long. */
        const val VISIBLE_MS = 10 * 60_000L

        @Volatile var instance: WinDropService? = null
            private set

        /** Items handed over by share / drop activities, drained by the service. */
        val pending = ConcurrentLinkedQueue<OutgoingItem>()
        val listeners = CopyOnWriteArrayList<() -> Unit>()

        fun start(ctx: Context) {
            Prefs(ctx).enabled = true
            ctx.startForegroundService(Intent(ctx, WinDropService::class.java).setAction(ACTION_START))
        }

        fun stop(ctx: Context) {
            Prefs(ctx).enabled = false
            ctx.stopService(Intent(ctx, WinDropService::class.java))
        }

        fun enqueue(ctx: Context, items: List<OutgoingItem>) {
            if (items.isEmpty()) return
            pending.addAll(items)
            ctx.startForegroundService(Intent(ctx, WinDropService::class.java).setAction(ACTION_SEND))
        }

        fun publishShareShortcut(ctx: Context) {
            val sm = ctx.getSystemService(ShortcutManager::class.java) ?: return
            val label = Prefs(ctx).pcLabel
            val s = ShortcutInfo.Builder(ctx, "pc")
                .setShortLabel("WinDrop")
                .setLongLabel("WinDrop to $label")
                .setIcon(Icon.createWithResource(ctx, R.drawable.ic_shortcut_pc))
                .setIntent(Intent(Intent.ACTION_VIEW).setClass(ctx, MainActivity::class.java))
                .setCategories(setOf(SHARE_CATEGORY))
                .setLongLived(true)
                .setRank(0)
                .build()
            runCatching { sm.dynamicShortcuts = listOf(s) }
        }
    }

    lateinit var prefs: Prefs; private set
    lateinit var outbox: OutboxManager; private set
    private lateinit var island: Island
    lateinit var p2p: WifiDirectLink
    private lateinit var ble: BleWatcher
    @Volatile private var lastActivity = 0L
    private lateinit var nearbyAdv: NearbyAdvertiser
    /** Until when nearby (unpaired) PCs may send to us; 0 = not visible. */
    @Volatile var visibleUntil = 0L; private set
    val visible get() = System.currentTimeMillis() < visibleUntil
    private val stopVisible = Runnable { setVisible(false) }
    // Nearby PCs that let phones send to them ("WE" beacon): name -> last heard (elapsedRealtime).
    private val nearbyPcs = java.util.concurrent.ConcurrentHashMap<String, Long>()
    @Volatile private var nearbyError: String? = null  // why the last nearby send failed, for the pill
    private val receiving = AtomicInteger(0)
    private var bleCooldownUntil = 0L

    /** Permissions granted or pairing code changed. */
    fun onSetupChanged() { ble.start(); ble.reset(); p2p.repair() }

    /** Open the link now (e.g. "Try again" on queued items). */
    fun connectNow() {
        if (p2p.connectedIp != null) return
        if (!Radios.wifiOn(this)) { wifiWarnedAt = 0; warnWifiOff("Turn it on to send what's waiting"); return }
        p2p.want()
        showConnecting()
    }

    /** Close the on-demand link once nothing has moved for a bit, so Android brings Wi-Fi back. */
    private val idleCheck = object : Runnable {
        override fun run() {
            if (p2p.connectedIp == null) return
            val open = p2p.openTarget != null
            val idle = System.currentTimeMillis() - lastActivity > if (open) OPEN_IDLE_MS else LINK_IDLE_MS
            if (idle && open && queued.get() == 0 && receiving.get() == 0) {
                Logs.i("nearby: done with ${p2p.openTarget}, letting the link go")
                p2p.release()
                return
            }
            // Pairing waits up to a minute on the PC's user: that link isn't idle.
            if (idle && !open && !pairing && queued.get() == 0 && receiving.get() == 0 && outbox.count == 0) {
                Logs.i("link idle for ${LINK_IDLE_MS / 1000}s, closing")
                uploadLogs()
                sender.execute {
                    sayBye()
                    main.post { p2p.release(); ble.reset() }
                }
            } else main.postDelayed(this, 3000)
        }
    }

    // What the notification and pill show while a transfer runs.
    @Volatile private var xferName = ""
    @Volatile private var xferSending = false
    private val progressTick = object : Runnable {
        override fun run() {
            if (xferName.isEmpty()) return
            updateServiceNotification()
            main.postDelayed(this, 700)
        }
    }
    private val main = Handler(Looper.getMainLooper())
    private val sender = Executors.newSingleThreadExecutor()
    private val io = Executors.newCachedThreadPool()
    private val queued = AtomicInteger(0)
    @Volatile private var stopped = false
    private var server: ServerSocket? = null

    @Volatile var xferDone = 0L; private set
    @Volatile var xferTotal = 0L; private set

    // incoming offers awaiting a tap
    private val offerSeq = AtomicLong(0)
    private val decisions = HashMap<Long, Pair<CountDownLatch, BooleanArray>>()

    // outbox flushing
    @Volatile private var flushing = false
    @Volatile private var flushAbort = false

    private val screenOn = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) = ble.start()
    }

    /** Bluetooth switched on after WinDrop started: begin listening for the PC then. */
    private val btState = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val st = i.getIntExtra(android.bluetooth.BluetoothAdapter.EXTRA_STATE, -1)
            if (st == android.bluetooth.BluetoothAdapter.STATE_ON) { Logs.i("bluetooth turned on"); ble.start() }
            else if (st == android.bluetooth.BluetoothAdapter.STATE_OFF) { Logs.w("bluetooth turned off"); ble.stop() }
            else return
            updateServiceNotification()
            notifyListeners()
        }
    }

    /** Wi-Fi carries every transfer: say when it's off, and send what waited for it once it's back. */
    private val wifiState = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            if (isInitialStickyBroadcast) return  // the current state, replayed on registering: no change
            when (i.getIntExtra(android.net.wifi.WifiManager.EXTRA_WIFI_STATE, -1)) {
                android.net.wifi.WifiManager.WIFI_STATE_ENABLED -> {
                    Logs.i("wifi turned on")
                    if (prefs.enabled && prefs.paired && outbox.count > 0) connectNow()
                }
                android.net.wifi.WifiManager.WIFI_STATE_DISABLED -> Logs.w("wifi turned off")
                else -> return
            }
            updateServiceNotification()
            notifyListeners()
        }
    }
    private var wifiWarnedAt = 0L

    /** Says (at most every 2 minutes) that something can't go out because Wi-Fi is off. */
    private fun warnWifiOff(sub: String) {
        val now = System.currentTimeMillis()
        if (now - wifiWarnedAt < 120_000) return
        wifiWarnedAt = now
        ui(Island.Msg("Wi-Fi is off", sub, Island.G_WARN, Island.AMBER, onClick = { Radios.openWifi(this) }), 4000, "Wi-Fi is off — $sub")
    }

    // ---- lifecycle ----
    override fun onCreate() {
        super.onCreate()
        Logs.init(this)
        instance = this
        prefs = Prefs(this)
        outbox = OutboxManager(this)
        island = Island(this)
        createChannels()
        goForeground()
        thread(name = "td-server", isDaemon = true) { serverLoop() }
        p2p = WifiDirectLink(this, prefs, onPcAddress = { ip ->
            // Joined (or lost) the PC's Wi-Fi Direct group: say hello over it and drain the outbox.
            if (ip != null) {
                Logs.i("link up, PC at $ip; VPN active: ${Diagnostics.vpnActive(this)}")
                Logs.i(Diagnostics.interfaces().replace('\n', ';'))
                lastActivity = System.currentTimeMillis()
                // While pairing, the old key (if any) is about to be replaced: wait for the new one.
                // A nearby PC isn't ours: nothing of ours goes to it.
                if (!pairing && p2p.openTarget == null) { flushOutbox(); uploadLogs() }
                main.removeCallbacks(idleCheck)
                main.postDelayed(idleCheck, 3000)
                if (island.attached && island.isConnecting) island.connectingDone()
            } else if (p2p.openTarget != null) {
                // The nearby PC closed the link: don't chase it. Our own PC's queue goes next.
                main.post {
                    p2p.release()
                    if (prefs.paired && outbox.count > 0) main.postDelayed({ connectNow() }, 1500)
                }
            }
            main.post { updateServiceNotification() }
            notifyListeners()
        }, onGaveUp = {
            bleCooldownUntil = System.currentTimeMillis() + 3 * 60_000L
            ble.reset()
            val n = outbox.count
            if (island.attached) island.connectingDone()
            if (n > 0) ui(Island.Msg("Couldn't reach ${prefs.pcLabel}", "$n waiting · open WinDrop to retry", Island.G_WARN, Island.AMBER), 4000,
                "Couldn't reach ${prefs.pcLabel} — $n waiting")
            main.post { updateServiceNotification() }
        })
        p2p.start()
        ble = BleWatcher(this, prefs) { pending, name, pcWifiOff ->
            main.post {
                // A PC in pairing mode: only interesting while we're pairing.
                if (name.isNotEmpty() && pairing && (name != prefs.pcP2pName || pcWifiOff != pairPcWifiOff)) {
                    Logs.i("ble: found PC in pairing mode: $name${if (pcWifiOff) " (its Wi-Fi is off)" else ""}")
                    pairPcWifiOff = pcWifiOff
                    prefs.pcP2pName = name
                    notifyListeners()
                }
                if (!pending) return@post
                if (p2p.connectedIp != null || p2p.wanted || System.currentTimeMillis() < bleCooldownUntil) return@post
                if (!Radios.wifiOn(this)) {
                    Logs.w("ble: PC has something for us, but Wi-Fi is off")
                    warnWifiOff("${prefs.pcLabel} has something for you: turn Wi-Fi on to get it")
                    return@post
                }
                Logs.i("ble: PC has something for us")
                p2p.want()
                showConnecting(incoming = true)
            }
        }
        ble.onNearby = { call, name, wifiOff -> main.post { onNearbyBeacon(call, name, wifiOff) } }
        nearbyAdv = NearbyAdvertiser(this, prefs)
        ble.start()
        registerReceiver(screenOn, IntentFilter(Intent.ACTION_SCREEN_ON))
        registerReceiver(btState, IntentFilter(android.bluetooth.BluetoothAdapter.ACTION_STATE_CHANGED))
        registerReceiver(wifiState, IntentFilter(android.net.wifi.WifiManager.WIFI_STATE_CHANGED_ACTION))
        if (prefs.enabled) island.attach()
        publishShareShortcut(this)
        Logs.i("service started (WinDrop ${Crypto.appVersion}, ${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE})")
        io.execute { Updater.check(this, notify = true) }  // at most once a day; a notification if there's a new one
        notifyListeners()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> { prefs.enabled = false; stopSelf(); return START_NOT_STICKY }
            ACTION_START -> if (!island.attached) island.attach()
            ACTION_SEND -> drainPending()
            ACTION_DECIDE -> decide(intent.getLongExtra(EXTRA_OFFER, -1), intent.getBooleanExtra(EXTRA_ACCEPT, false))
            null -> if (!prefs.enabled) { stopSelf(); return START_NOT_STICKY }
        }
        return if (prefs.enabled) START_STICKY else START_NOT_STICKY
    }

    override fun onDestroy() {
        stopped = true
        instance = null
        island.detach()
        nearbyAdv.stop()
        ble.stop()
        p2p.stop()
        main.removeCallbacksAndMessages(null)
        runCatching { server?.close() }
        runCatching { unregisterReceiver(screenOn) }
        runCatching { unregisterReceiver(btState) }
        runCatching { unregisterReceiver(wifiState) }
        synchronized(decisions) { decisions.values.forEach { it.first.countDown() } }
        sender.shutdownNow()
        io.shutdownNow()
        notifyListeners()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun notifyListeners() = main.post { listeners.forEach { it() } }

    // ---- notifications ----
    private fun createChannels() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CH_SERVICE, "WinDrop status and progress", NotificationManager.IMPORTANCE_LOW).apply {
            setShowBadge(false)
        })
        nm.createNotificationChannel(NotificationChannel(CH_OFFERS, "Incoming transfer requests", NotificationManager.IMPORTANCE_HIGH))
        nm.createNotificationChannel(NotificationChannel(CH_RECEIVED, "Received files", NotificationManager.IMPORTANCE_DEFAULT).apply {
            setSound(null, null)
        })
    }

    private fun serviceNotification(): Notification {
        val stop = PendingIntent.getService(
            this, 0, Intent(this, WinDropService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE,
        )
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val b = Notification.Builder(this, CH_SERVICE)
            .setSmallIcon(R.drawable.ic_windrop_glyph)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .addAction(Notification.Action.Builder(null, "Clipboard → PC",
                PendingIntent.getActivity(this, 3, ClipboardSendActivity.intent(this), PendingIntent.FLAG_IMMUTABLE)).build())
            .addAction(Notification.Action.Builder(null, "Turn off", stop).build())
        val name = xferName
        val pc = prefs.pcLabel
        when {
            name.isNotEmpty() -> {
                val total = xferTotal
                val done = xferDone
                val pct = if (total > 0) (done * 100 / total).toInt() else 0
                b.setContentTitle(if (xferSending) "Sending to $pc" else "Receiving from $pc")
                    .setContentText("$name · ${humanSize(done)} of ${humanSize(total)}")
                    .setProgress(100, pct, total <= 0)
            }
            linkWanted() && linkIp() == null ->
                b.setContentTitle("Connecting to $pc\u2026").setContentText(statusLine()).setProgress(0, 0, true)
            else -> b.setContentTitle(if (prefs.enabled) "WinDrop is on" else "WinDrop is sending").setContentText(statusLine())
        }
        return b.build()
    }

    private fun goForeground() {
        val n = serviceNotification()
        if (Build.VERSION.SDK_INT >= 34) startForeground(NOTIF_SERVICE, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        else startForeground(NOTIF_SERVICE, n)
    }

    private fun updateServiceNotification() {
        if (stopped) return
        getSystemService(NotificationManager::class.java).notify(NOTIF_SERVICE, serviceNotification())
    }

    private fun statusLine(): String {
        val pc = when {
            !prefs.paired -> "Not paired — open WinDrop to pair"
            linkIp() != null -> "Connected to ${prefs.pcLabel}"
            !Radios.wifiOn(this) -> "Wi-Fi is off — nothing can be sent or received"
            !Radios.bluetoothOn(this) -> "Bluetooth is off — ${prefs.pcLabel} can't reach this phone"
            else -> "Ready · ${prefs.pcLabel}"
        }
        val n = outbox.count
        val vis = if (visible) " \u00B7 nearby PCs can send" else ""
        return (if (n > 0) "$pc · $n waiting" else pc) + vis
    }

    // The notification is built before the link exists (startForeground runs first in onCreate).
    private fun linkIp() = if (::p2p.isInitialized) p2p.connectedIp else null
    private fun linkWanted() = ::p2p.isInitialized && p2p.wanted

    private fun startProgress(name: String, sending: Boolean) {
        xferName = name
        xferSending = sending
        main.removeCallbacks(progressTick)
        main.post(progressTick)
    }

    private fun endProgress() {
        xferName = ""
        main.removeCallbacks(progressTick)
        main.post { updateServiceNotification() }
    }

    /** Pill + notification while the on-demand link is coming up. */
    private fun showConnecting(incoming: Boolean = false) = main.post {
        if (island.attached) island.connecting(prefs.pcLabel, incoming)
        updateServiceNotification()
    }

    // ---- outgoing ----
    fun handleDrop(e: DragEvent) {
        val clip = e.clipData ?: return
        val uris = (0 until clip.itemCount).mapNotNull { clip.getItemAt(it).uri }
        if (uris.isEmpty()) {
            val text = (0 until clip.itemCount).joinToString("\n") { clip.getItemAt(it).coerceToText(this) }.trim()
            if (text.isNotEmpty()) { pending.add(OutgoingItem.text(text)); drainPending() }
            return
        }
        try {
            pending.addAll(uris.mapNotNull { OutgoingItem.fromUri(this, it) })
            drainPending()
        } catch (se: SecurityException) {
            // The grant has to be taken by an Activity: bounce through an invisible one.
            startActivity(
                Intent(this, DropPermissionActivity::class.java)
                    .putExtra(DropPermissionActivity.EXTRA_EVENT, e)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION),
            )
        }
    }

    // ---- nearby (unpaired) PCs ----
    /** Turns "let nearby PCs send to this phone" on (for 10 minutes) or off. False if it can't advertise. */
    fun setVisible(on: Boolean): Boolean {
        main.removeCallbacks(stopVisible)
        if (!on) {
            if (visibleUntil != 0L) Logs.i("nearby: visibility off")
            visibleUntil = 0
            nearbyAdv.stop()
        } else {
            if (!nearbyAdv.start(!Radios.wifiOn(this))) return false
            visibleUntil = System.currentTimeMillis() + VISIBLE_MS
            main.postDelayed(stopVisible, VISIBLE_MS)
        }
        updateServiceNotification()
        notifyListeners()
        return true
    }

    private fun isOwnPc(name: String) = prefs.paired &&
        (name.equals(prefs.lastPcName, ignoreCase = true) || prefs.pcP2pName.isNotEmpty() && name.startsWith(prefs.pcP2pName, ignoreCase = true))

    /** Heard [name]'s "nearby phones may send" beacon in the last [ms]. */
    private fun nearbyFresh(name: String, ms: Long) =
        nearbyPcs[name]?.let { android.os.SystemClock.elapsedRealtime() - it < ms } == true

    /** Nearby PCs that take files from any phone right now (not our own), at most 3. */
    fun nearbyPcNames(): List<String> = nearbyPcs.keys.filter { nearbyFresh(it, 9_000) && !isOwnPc(it) }.sorted().take(3)

    /** A nearby PC is visible ([call] null), or calls us because it has something for us. */
    private fun onNearbyBeacon(call: ByteArray?, name: String, pcWifiOff: Boolean) {
        if (name.isEmpty()) return
        if (call == null) {
            if (nearbyPcs.put(name, android.os.SystemClock.elapsedRealtime()) == null) Logs.i("nearby: $name takes files from nearby phones")
            return
        }
        if (!visible || !call.contentEquals(nearbyAdv.tag)) return
        if (p2p.openTarget == name && (p2p.connectedIp != null || p2p.wanted)) return  // on our way
        if (pairing || queued.get() > 0 || receiving.get() > 0) return  // busy: it keeps calling
        if (!Radios.wifiOn(this)) { warnWifiOff("$name has something for you: turn Wi-Fi on to get it"); return }
        if (pcWifiOff) Logs.w("nearby: $name calls us but its Wi-Fi is off")
        Logs.i("nearby: $name has something for us")
        startOpenLink(name, incoming = true)
    }

    /** Joins a nearby PC's link (letting go of any other link first). Main thread. */
    private fun startOpenLink(pc: String, incoming: Boolean) {
        val hadLink = p2p.connectedIp != null
        if (hadLink || p2p.wanted) p2p.release()
        val go = Runnable {
            p2p.openTarget = pc
            p2p.want()
            if (island.attached) island.connecting(pc, incoming)
            updateServiceNotification()
        }
        // The old group takes a moment to go; asking for the new link before that can mistake it.
        if (hadLink) main.postDelayed(go, 1000) else go.run()
    }

    private fun chooserTargets(): List<Pair<String, String>> =
        (if (prefs.paired) listOf(OWN_PC to "${prefs.pcLabel} \u00B7 paired") else emptyList()) +
            nearbyPcNames().map { it to "$it \u00B7 nearby" }

    // Keeps the chooser's rows current: nearby PCs come and go while it's open.
    private val chooserTick = object : Runnable {
        override fun run() {
            if (!island.choosing) return
            island.updateChooser(chooserTargets())
            main.postDelayed(this, 1000)
        }
    }

    /** "Send to which PC?" in the pill: our own PC and the nearby ones, kept up to date. */
    private fun chooseTarget() {
        val items = generateSequence { pending.poll() }.toList()
        if (items.isEmpty()) return
        val what = if (items.size == 1) items[0].name else "${items.size} items"
        var seenAny = false
        island.showChooser("Send $what to\u2026", chooserTargets().also { seenAny = it.isNotEmpty() }) { id ->
            main.removeCallbacks(chooserTick)
            when (id) {
                null -> {
                    items.forEach { it.close() }
                    if (!seenAny && nearbyPcNames().isEmpty() && !prefs.paired)
                        island.showMessage(Island.Msg("No PC found nearby", "Pair in WinDrop, or on the PC: Let nearby phones send", Island.G_WARN, Island.AMBER), 3500)
                    else island.showMessage(Island.Msg("Not sent", what, Island.G_X, Island.GRAY_BTN), 1500)
                }
                OWN_PC -> { pending.addAll(items); drainPending(choose = false) }
                else -> sendNearby(items, id)
            }
        }
        main.removeCallbacks(chooserTick)
        main.postDelayed(chooserTick, 1000)
    }

    private fun sendNearby(items: List<OutgoingItem>, pc: String) {
        if (!Radios.wifiOn(this)) {
            items.forEach { it.close() }
            warnWifiOff("Turn it on, then send again")
            return
        }
        Logs.i("nearby: sending ${items.size} item(s) to $pc")
        queued.addAndGet(items.size)
        sender.execute {
            // It may have stopped taking files a moment ago: make sure before joining it.
            if (!nearbyFresh(pc, 4_000)) {
                ui(Island.Msg("Looking for $pc\u2026", "Checking it still takes files", Island.G_UP), 3000, "Looking for $pc\u2026")
                val until = System.currentTimeMillis() + 6_000
                while (!nearbyFresh(pc, 4_000) && System.currentTimeMillis() < until && !stopped) Thread.sleep(250)
            }
            if (!nearbyFresh(pc, 4_000)) {
                Logs.w("nearby: $pc is gone")
                ui(Island.Msg("$pc isn't taking files now", "It stopped letting nearby phones send", Island.G_WARN, Island.AMBER), 3500,
                    "$pc isn't taking files now")
                items.forEach { it.close() }
                if (queued.addAndGet(-items.size) == 0) main.post { island.transferEnded(); updateServiceNotification() }
                return@execute
            }
            main.post { startOpenLink(pc, incoming = false) }
            val until = System.currentTimeMillis() + 50_000
            fun linked() = p2p.openTarget == pc && p2p.connectedIp != null
            while (!linked() && System.currentTimeMillis() < until && !stopped) {
                if (!nearbyFresh(pc, 10_000)) { nearbyError = "$pc stopped taking files from nearby phones"; break }
                Thread.sleep(250)
            }
            if (!linked()) Logs.w("nearby: couldn't reach $pc")
            for (item in items) process(item, to = pc)
        }
    }

    /** Connects to the nearby PC on the link with a fresh, unpaired key. */
    private fun openNearbyChannel(): Pair<Socket, SecureChannel>? {
        var conn: PeerResolver.Connection? = null
        val until = System.currentTimeMillis() + 10_000
        while (conn == null && System.currentTimeMillis() < until && p2p.connectedIp != null && !stopped) {
            conn = PeerResolver.connect(candidates(), 2000)
            if (conn == null) Thread.sleep(500)
        }
        val s = (conn ?: return null).socket
        tune(s)
        s.soTimeout = 15_000
        return try {
            val peer = OpenHello.client(s, prefs.deviceId, prefs.deviceName)
            if (peer == null) {
                Logs.w("nearby: the PC isn't taking files from nearby phones (any more)")
                nearbyError = "${p2p.openTarget ?: "The PC"} stopped taking files from nearby phones"
                runCatching { s.close() }
                return null
            }
            val ch = SecureChannel.client(s, peer.key, prefs.deviceId) ?: run { runCatching { s.close() }; return null }
            s.soTimeout = 75_000  // the PC's user has a minute to accept
            Logs.i("nearby: connected to ${peer.name}")
            s to ch
        } catch (e: Exception) {
            Logs.w("nearby: handshake failed", e)
            runCatching { s.close() }
            null
        }
    }

    /** Asks on the pill (or in a notification) whether to take a nearby PC's offer. */
    private fun askOffer(offer: Proto.Offer, name: String, from: String): Boolean {
        val id = offerSeq.incrementAndGet()
        val latch = CountDownLatch(1)
        val res = BooleanArray(1)
        synchronized(decisions) { decisions[id] = latch to res }
        val shown = if (offer.type == Proto.TYPE_TEXT) "text" else name
        main.post {
            if (island.attached) island.showOffer(id, shown, offer.size, from)
            else showOfferNotification(id, shown, Proto.Offer(offer.type, offer.size, shown, from, ""))
        }
        val yes = latch.await(60, TimeUnit.SECONDS) && res[0]
        synchronized(decisions) { decisions.remove(id) }
        main.post {
            island.closeOffer(id)
            getSystemService(NotificationManager::class.java).cancel(NOTIF_OFFER)
        }
        Logs.i("nearby: ${offer.name} from $from ${if (yes) "accepted" else "declined"}")
        return yes
    }

    private fun drainPending(choose: Boolean = true) {
        if (choose && pending.isNotEmpty()) {
            val nearby = nearbyPcNames()
            // Nearby PCs around, or no PC of our own: ask (the list keeps looking while it asks).
            if ((nearby.isNotEmpty() || !prefs.paired) && island.attached) { chooseTarget(); return }
            if (!prefs.paired) {
                val items = generateSequence { pending.poll() }.toList()
                // No pill to ask with: the nearby PC it is, if there's one.
                if (nearby.isNotEmpty()) { sendNearby(items, nearby[0]); return }
                items.forEach { it.close() }
                toast("Not paired with a PC. Pair in WinDrop, or on the PC: Let nearby phones send")
                return
            }
        }
        // User asked to send: open the on-demand link. With Wi-Fi off there's nothing to open: the
        // items wait in the outbox and go out by themselves once Wi-Fi is back (see wifiState).
        if (pending.isNotEmpty() && p2p.connectedIp == null) {
            if (Radios.wifiOn(this)) { p2p.want(); showConnecting() }
            else warnWifiOff("Turn it on: this sends by itself then")
        }
        while (true) {
            val item = pending.poll() ?: break
            queued.incrementAndGet()
            sender.execute { process(item) }
        }
        stopIfIdle()
    }

    private enum class Result { OK, OFFLINE, DECLINED, FAILED, NOT_PAIRED }

    /** [to]: a nearby (unpaired) PC's name; nothing for it goes to the outbox. */
    private fun process(item: OutgoingItem, to: String? = null) {
        try {
            if (item.outboxId != null && flushAbort) return
            var target = to
            var r = try { send(item, to) } catch (e: Exception) { Logs.w("send failed", e); Result.FAILED }
            if (r == Result.NOT_PAIRED) {
                // Our PC removed this phone. If it takes files from nearby phones, send it that way.
                val near = nearbyPcs.keys.firstOrNull { isOwnPc(it) && nearbyFresh(it, 9_000) }
                forgetPairing(keepLink = near != null)
                r = Result.FAILED
                if (near != null) {
                    Logs.i("nearby: $near forgot this phone; sending as a nearby phone instead")
                    p2p.openTarget = near  // same PC, same link: it just isn't ours any more
                    target = near
                    r = try { send(item, near) } catch (e: Exception) { Logs.w("send failed", e); Result.FAILED }
                }
            }
            Logs.i("send ${item.name}${if (target != null) " to $target" else ""} -> $r")
            val label = target ?: prefs.pcLabel
            val reason = nearbyError
            nearbyError = null
            if (target != null && (r == Result.OFFLINE || r == Result.FAILED)) {
                val title = reason ?: if (r == Result.OFFLINE) "Couldn't reach $target" else "Transfer failed"
                ui(Island.Msg(title, "${item.name} wasn't sent", Island.G_WARN, Island.AMBER), 3500, title)
                return
            }
            when (r) {
                Result.OK -> {
                    item.outboxId?.let { outbox.remove(it) }
                    ui(Island.Msg("Sent ${item.name}", "Delivered to $label", Island.G_CHECK, Island.GREEN), 2000, "Sent ${item.name}")
                }
                Result.OFFLINE -> if (item.outboxId == null) {
                    val entry = try {
                        if (item.isText) outbox.addText(item.open().readBytes().toString(Charsets.UTF_8))
                        else outbox.addFile(item.name, item.open())
                    } catch (e: Exception) { null }
                    val count = outbox.count
                    Logs.i("queued ${item.name} until the link is up ($count waiting)")
                    main.post {
                        // While the link is coming up the pill already says "Connecting…"; with Wi-Fi
                        // off, it already says so.
                        if (!p2p.wanted && Radios.wifiOn(this)) {
                            if (entry != null && island.attached) island.offline(entry.id, count)
                            else toast(if (entry != null) "$label not reachable — $count not sent yet" else "$label not reachable")
                        }
                        updateServiceNotification()
                    }
                } else {
                    flushAbort = true
                }
                Result.DECLINED -> {
                    item.outboxId?.let { outbox.remove(it) }
                    ui(Island.Msg("$label declined ${item.name}", glyph = Island.G_X, accent = Island.RED), 2500, "$label declined")
                }
                Result.FAILED, Result.NOT_PAIRED -> ui(Island.Msg("Transfer failed", item.name, Island.G_WARN, Island.RED), 3000, "Transfer failed")
            }
        } finally {
            item.close()
            lastActivity = System.currentTimeMillis()
            if (queued.decrementAndGet() == 0) main.post { island.transferEnded(); updateServiceNotification(); stopIfIdle() }
            endProgress()
        }
    }

    private fun send(original: OutgoingItem, to: String? = null): Result {
        // On a nearby PC's link, our own PC isn't there: keep it for later.
        if (to == null && p2p.openTarget != null) return Result.OFFLINE
        var item = original
        if (!item.isText && item.size < 0) {
            // Unknown length (some providers stream): spool so we can announce the size.
            val f = File.createTempFile("spool", null, cacheDir)
            item.open().use { src -> f.outputStream().use { src.copyTo(it, Proto.CHUNK) } }
            item = item.withSpooled(f)
            f.deleteOnExit()
        }
        val (s, ch) = if (to != null) {
            if (p2p.openTarget != to || p2p.connectedIp == null) return Result.OFFLINE
            openNearbyChannel() ?: return Result.FAILED
        } else try { openChannel() } catch (e: Incompatible) { reportIncompatible(e.peer); return Result.FAILED }
            catch (e: NotPaired) { return Result.NOT_PAIRED }
            ?: return if (prefs.paired) Result.OFFLINE else Result.FAILED
        s.use {
            val out = ch.output
            val inp = DataInputStream(ch.input)
            try {
                Proto.writeOffer(out, Proto.Offer(if (item.isText) Proto.TYPE_TEXT else Proto.TYPE_FILE, item.size,
                    if (item.isText) "text.txt" else item.name, prefs.deviceName, prefs.deviceId))
                out.flush()
            } catch (e: IOException) { return Result.OFFLINE }
            if (to != null) ui(Island.Msg("Waiting for $to to accept", item.name, Island.G_UP), 3500, "Waiting for $to to accept")

            val d = try { inp.read() } catch (e: SocketTimeoutException) { return Result.FAILED } catch (e: IOException) { -1 }
            if (d == -1) return Result.FAILED
            if (d == Proto.DECLINED) return Result.DECLINED
            if (d != Proto.ACCEPTED) return Result.FAILED

            xferTotal = item.size
            xferDone = 0
            Logs.i("sending ${item.name} (${humanSize(item.size)})")
            startProgress(item.name, true)
            main.post { island.transferStarted(true, item.name, item.outboxId != null) }
            s.soTimeout = 30_000
            item.open().use { src ->
                val buf = ByteArray(Proto.CHUNK)
                var left = item.size
                while (left > 0) {
                    val n = src.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
                    if (n < 0) break
                    out.write(buf, 0, n)
                    left -= n
                    xferDone += n
                }
                out.flush()
                if (left > 0) return Result.FAILED
            }
            return if (inp.read() == Proto.ACK_OK) Result.OK else Result.FAILED
        }
    }

    /** The PC is only ever reached over the Wi-Fi Direct link. */
    private fun candidates(): List<InetSocketAddress> =
        listOfNotNull(p2p.connectedIp).mapNotNull { runCatching { InetSocketAddress(InetAddress.getByName(it), Proto.TCP_PORT) }.getOrNull() }

    private fun tune(s: Socket) {
        s.tcpNoDelay = true
        runCatching { s.sendBufferSize = Proto.SOCK_BUF; s.receiveBufferSize = Proto.SOCK_BUF }
    }

    /**
     * Connects to the PC and proves we're its paired phone (and that it's our PC). Throws
     * [Incompatible] when the PC's WinDrop speaks another protocol.
     */
    private fun openChannel(timeoutMs: Int = 800): Pair<Socket, SecureChannel>? {
        val key = prefs.pairKey ?: return null
        val conn = PeerResolver.connect(candidates(), timeoutMs) ?: return null
        val s = conn.socket
        tune(s)
        s.soTimeout = 15_000
        val ch = try {
            SecureChannel.client(s, key, prefs.deviceId)
        } catch (e: Incompatible) {
            runCatching { s.close() }
            throw e
        } catch (e: NotPaired) {
            runCatching { s.close() }
            throw e
        } catch (e: Exception) {
            null
        }
        if (ch == null) {
            Logs.w("the PC at ${conn.address} failed the pairing check")
            runCatching { s.close() }
            return null
        }
        s.soTimeout = 35_000
        return s to ch
    }

    fun cancelOutbox(ids: List<Long>) = io.execute { ids.forEach { outbox.remove(it) }; main.post { updateServiceNotification() } }

    fun flushOutbox() {
        if (stopped || flushing || !prefs.paired || p2p.openTarget != null) return
        if (p2p.connectedIp == null) return
        val entries = outbox.list()
        if (entries.isEmpty()) return
        flushing = true
        flushAbort = false
        for (e in entries) {
            queued.incrementAndGet()
            sender.execute { process(OutgoingItem.fromOutbox(e)) }
        }
        sender.execute { flushing = false; flushAbort = false }
    }

    private fun stopIfIdle() {
        if (!prefs.enabled && queued.get() == 0 && pending.isEmpty()) stopSelf()
    }

    // ---- incoming ----
    private fun serverLoop() {
        val ss = try {
            ServerSocket().apply {
                reuseAddress = true
                runCatching { receiveBufferSize = Proto.SOCK_BUF }
                bind(InetSocketAddress(Proto.TCP_PORT))
            }
        } catch (e: IOException) {
            Logs.e("bind ${Proto.TCP_PORT} failed", e); return
        }
        server = ss
        while (!stopped) {
            val c = try { ss.accept() } catch (e: IOException) { break }
            receiving.incrementAndGet()
            try { handleIncoming(c) } catch (e: Exception) { Logs.w("incoming failed", e) } finally {
                // Close politely: unread bytes from the PC would turn the close into a reset that
                // can swallow our last reply (say, "incompatible version").
                runCatching {
                    c.shutdownOutput()
                    c.soTimeout = 300
                    val junk = ByteArray(4096)
                    val inp = c.getInputStream()
                    repeat(64) { if (inp.read(junk) < 0) return@runCatching }
                }
                runCatching { c.close() }
                lastActivity = System.currentTimeMillis()
                receiving.decrementAndGet()
            }
        }
    }

    private fun handleIncoming(c: Socket) {
        c.soTimeout = 15_000
        tune(c)
        val raw = DataInputStream(BufferedInputStream(c.getInputStream(), 256 * 1024))
        val (type, peer) = Crypto.readEnvelope(raw) ?: run {
            Logs.w("refused a connection from ${c.inetAddress.hostAddress}: not a WinDrop it can talk to")
            return
        }
        if (peer.proto != Crypto.PROTO) {
            c.getOutputStream().apply { write(Crypto.incompatibleReply()); flush() }
            reportIncompatible(peer)
            return
        }
        if (type == Crypto.HELLO_OPEN) { handleOpen(c, raw); return }
        if (type != Crypto.HELLO_SESSION) return
        val ch = SecureChannel.server(c, raw) { id -> if (prefs.paired && id == prefs.pairedPcId) prefs.pairKey else null }
        if (ch == null) {
            Logs.w("refused a connection from ${c.inetAddress.hostAddress}: not the paired PC")
            return
        }
        serveSession(c, ch, null)
    }

    /** A nearby (unpaired) PC has something for us: only while we're visible, and the user decides. */
    private fun handleOpen(c: Socket, raw: DataInputStream) {
        if (!visible) {
            Logs.w("nearby: refused a PC: not visible to nearby PCs")
            c.getOutputStream().apply { write(Crypto.STATUS_REFUSED); flush() }
            return
        }
        val peer = OpenHello.server(c, raw, prefs.deviceId, prefs.deviceName) ?: return
        val (type, pv) = Crypto.readEnvelope(raw) ?: return
        if (type != Crypto.HELLO_SESSION || pv.proto != Crypto.PROTO) return
        val ch = SecureChannel.server(c, raw) { id -> if (id == peer.id) peer.key else null } ?: return
        Logs.i("nearby: ${peer.name} connected")
        serveSession(c, ch, peer.name.ifEmpty { "A PC" })
    }

    /** One offer over an established session. [stranger]: an unpaired PC's name (its offers are asked about). */
    private fun serveSession(c: Socket, ch: SecureChannel, stranger: String?) {
        val inp = DataInputStream(ch.input)
        val out = ch.output
        val offer = Proto.readOffer(inp) ?: return
        if (offer.type == Proto.TYPE_RELEASE && stranger != null) { out.write(Proto.BUSY); out.flush(); return }
        if (offer.type == Proto.TYPE_RELEASE) {
            // The PC serves one phone at a time and another one is waiting. Let go unless we're
            // about to send something (this request itself counts as one receive).
            val free = !pairing && queued.get() == 0 && receiving.get() <= 1 && pending.isEmpty()
            out.write(if (free) Proto.ACCEPTED else Proto.DECLINED)
            out.flush()
            if (free) {
                Logs.i("PC needs the link for another phone, letting go")
                main.post { p2p.release(); ble.reset(); updateServiceNotification() }
            }
            return
        }
        if (offer.type != Proto.TYPE_FILE && offer.type != Proto.TYPE_TEXT ||
            offer.type == Proto.TYPE_TEXT && offer.size > Proto.MAX_TEXT
        ) { out.write(Proto.BUSY); out.flush(); return }
        if (stranger == null && offer.sender.isNotEmpty() && offer.sender != prefs.lastPcName) prefs.lastPcName = offer.sender
        Logs.i("offer from ${stranger ?: "the PC"}: ${offer.name} (${humanSize(offer.size)})")
        val name = offer.name.replace(Regex("[\\\\/:*?\"<>|\\x00-\\x1f]"), "_").ifBlank { "file" }
        if (stranger != null && !askOffer(offer, name, stranger)) { out.write(Proto.DECLINED); out.flush(); return }
        out.write(Proto.ACCEPTED)  // the paired PC, or the user said yes
        out.flush()

        c.soTimeout = 30_000
        if (offer.type == Proto.TYPE_TEXT) {
            val bytes = ByteArray(offer.size.toInt())
            inp.readFully(bytes)
            out.write(Proto.ACK_OK)
            out.flush()
            onTextReceived(bytes.toString(Charsets.UTF_8))
            return
        }
        receiveFile(name, offer.size, inp, out)
    }


    fun decide(id: Long, accept: Boolean) {
        synchronized(decisions) {
            decisions[id]?.let { (latch, res) -> res[0] = accept; latch.countDown() }
        }
    }

    /** The offer pill was swiped away: keep the request answerable from the notification. */
    fun offerToNotification(id: Long, name: String, size: Long, sender: String) =
        showOfferNotification(id, name, Proto.Offer(Proto.TYPE_FILE, size, name, sender, ""))

    private fun showOfferNotification(id: Long, name: String, offer: Proto.Offer) {
        fun action(accept: Boolean) = PendingIntent.getService(
            this, if (accept) 1 else 2,
            Intent(this, WinDropService::class.java).setAction(ACTION_DECIDE)
                .putExtra(EXTRA_OFFER, id).putExtra(EXTRA_ACCEPT, accept),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val n = Notification.Builder(this, CH_OFFERS)
            .setSmallIcon(R.drawable.ic_windrop_glyph)
            .setContentTitle("${offer.sender} wants to send $name")
            .setContentText(humanSize(offer.size))
            .setTimeoutAfter(30_000)
            .addAction(Notification.Action.Builder(null, "Accept", action(true)).build())
            .addAction(Notification.Action.Builder(null, "Decline", action(false)).build())
            .build()
        getSystemService(NotificationManager::class.java).notify(NOTIF_OFFER, n)
    }

    private fun receiveFile(name: String, size: Long, inp: DataInputStream, out: java.io.OutputStream) {
        val ext = name.substringAfterLast('.', "").lowercase()
        val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, name)
            put(MediaStore.Downloads.MIME_TYPE, mime)
            put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/WinDrop")
            put(MediaStore.Downloads.IS_PENDING, 1)
        }
        val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
        if (uri == null) { out.write(Proto.ACK_BAD); return }
        xferTotal = size
        xferDone = 0
        startProgress(name, false)
        main.post { island.transferStarted(false, name, false) }
        var ok = false
        try {
            contentResolver.openOutputStream(uri)!!.use { dst ->
                val buf = ByteArray(Proto.CHUNK)
                var left = size
                while (left > 0) {
                    val n = inp.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
                    if (n < 0) break
                    dst.write(buf, 0, n)
                    left -= n
                    xferDone += n
                }
                ok = left == 0L
            }
        } catch (e: IOException) {
            Logs.w("receive failed", e)
        }
        if (ok) {
            contentResolver.update(uri, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
        } else {
            contentResolver.delete(uri, null, null)
        }
        runCatching { out.write(if (ok) Proto.ACK_OK else Proto.ACK_BAD); out.flush() }
        Logs.i("received $name: ${if (ok) "ok" else "incomplete"}")
        endProgress()
        main.post {
            island.transferEnded()
            if (ok) {
                val open = { openFolder() }
                if (island.attached) island.showMessage(Island.Msg("Saved to WinDrop — tap to show", name, Island.G_CHECK, Island.GREEN, onClick = open), 3000)
                postReceivedNotification(name, uri, mime)
                // Copied on the PC: put it on this clipboard too, ready to paste.
                if (name.startsWith("Clipboard ") && mime.startsWith("image/")) {
                    getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newUri(contentResolver, "From PC", uri))
                    Logs.i("clipboard: image from PC copied")
                }
            } else if (island.attached) {
                island.showMessage(Island.Msg("Transfer failed", name, Island.G_WARN, Island.RED), 3000)
            }
        }
    }

    /** Downloads/WinDrop in the Files app; the Downloads list where that isn't supported. */
    private fun folderIntent(): Intent {
        val dir = android.provider.DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download/WinDrop")
        val i = Intent(Intent.ACTION_VIEW).setDataAndType(dir, android.provider.DocumentsContract.Document.MIME_TYPE_DIR)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        return if (i.resolveActivity(packageManager) != null) i
        else Intent(android.app.DownloadManager.ACTION_VIEW_DOWNLOADS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    }

    private fun openFolder() {
        runCatching { startActivity(folderIntent()) }.onFailure { toast("Saved to Downloads/WinDrop") }
    }

    private fun postReceivedNotification(name: String, uri: Uri, mime: String) {
        val pi = PendingIntent.getActivity(this, uri.hashCode(), folderIntent(), PendingIntent.FLAG_IMMUTABLE)
        val n = Notification.Builder(this, CH_RECEIVED)
            .setSmallIcon(R.drawable.ic_windrop_glyph)
            .setContentTitle("Received $name")
            .setContentText("Saved to Downloads/WinDrop · tap to show")
            .setContentIntent(pi)
            .setAutoCancel(true)
            .build()
        getSystemService(NotificationManager::class.java).notify(uri.hashCode(), n)
    }

    private fun onTextReceived(text: String) = main.post {
        val cm = getSystemService(ClipboardManager::class.java)
        cm.setPrimaryClip(ClipData.newPlainText("From PC", text))
        val url = text.trim().takeIf { (it.startsWith("http://") || it.startsWith("https://")) && !it.contains(Regex("\\s")) }
        val preview = text.lineSequence().first().take(80)
        if (island.attached) {
            island.showMessage(
                Island.Msg(
                    if (url != null) "Link from PC — copied" else "Text from PC — copied",
                    if (url != null) "$preview  (tap to open)" else preview,
                    if (url != null) Island.G_LINK else Island.G_CHECK, Island.BLUE,
                    onClick = url?.let { { runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(it)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) } } },
                ),
                3500,
            )
        } else {
            toast("Copied text from PC")
        }
    }

    // ---- pairing ----
    @Volatile private var pairing = false
    val isPairing: Boolean get() = pairing
    @Volatile private var pairPcWifiOff = false  // the pairing PC's beacon says its Wi-Fi is off
    // The screen showing the pairing; a reopened screen takes over (main thread only).
    private var pairOnSas: (String) -> Unit = {}
    private var pairOnDone: (Boolean, String) -> Unit = { _, _ -> }
    private var pairSas: String? = null

    /**
     * Pairs with the PC that has "Pair phone…" open: finds it by its pairing beacon, opens the
     * link, runs the key exchange, and shows the check number ([onSas]) while the user confirms
     * it on the PC. [onDone] runs on the main thread with (success, message). Called while pairing
     * is already running, it only hands the callbacks to the caller (a reopened screen).
     */
    fun startPairing(onSas: (String) -> Unit, onDone: (Boolean, String) -> Unit) {
        pairOnSas = onSas
        pairOnDone = onDone
        if (pairing) { pairSas?.let(onSas); return }
        pairing = true
        pairSas = null
        Logs.i("pairing started")
        // Learn the name fresh from the pairing beacon; put the old ones back if pairing fails, so
        // the PC we're already paired with is still found.
        val oldName = prefs.pcP2pName
        val oldAddress = prefs.pcP2pAddress
        prefs.pcP2pName = ""
        prefs.pcP2pAddress = ""
        pairPcWifiOff = false
        // A link to some PC may be up already; pairing must open a fresh one to the PC in pairing mode.
        val hadLink = p2p.connectedIp != null
        main.post { p2p.release() }
        ble.start()
        ble.reset()  // a pairing beacon seen just before the tap must count now
        io.execute {
            val (ok, msg) = runCatching { pairOverLink(hadLink) }.getOrElse { Logs.w("pairing failed", it); false to "Pairing failed: ${it.message}" }
            pairing = false
            p2p.pairingMode = false
            if (!ok && prefs.pairKey != null) {
                prefs.pcP2pName = oldName
                prefs.pcP2pAddress = oldAddress
            }
            Logs.i("pairing: $msg")
            main.post {
                if (!ok) p2p.release()  // don't keep chasing a link the next attempt has to start over
                pairSas = null
                pairOnDone(ok, msg); updateServiceNotification(); notifyListeners()
            }
        }
    }

    private fun pairOverLink(hadLink: Boolean): Pair<Boolean, String> {
        if (!Radios.bluetoothOn(this)) return false to "Bluetooth is off. Turn it on: the phone finds the PC over Bluetooth."
        if (!Radios.wifiOn(this)) return false to "Wi-Fi is off. Turn it on: the phone connects to the PC over Wi-Fi Direct."
        // 1. Find a PC with pairing open (its Bluetooth beacon carries its name).
        var deadline = System.currentTimeMillis() + PAIR_FIND_MS
        while (prefs.pcP2pName.isEmpty() && System.currentTimeMillis() < deadline && !stopped) Thread.sleep(200)
        if (prefs.pcP2pName.isEmpty()) {
            return false to "Didn't find a PC in pairing mode within ${PAIR_FIND_MS / 1000} seconds. On the PC: tray icon \u2192 " +
                "Pair phone\u2026 (it stays open 3 minutes), keep the PC's Bluetooth on, then tap Pair with PC here."
        }
        if (pairPcWifiOff) return false to "Wi-Fi is off on ${prefs.pcP2pName}. Turn it on there, then try again."
        // The old link's group takes a moment to go; asking for the new link before that can
        // mistake the dying group for it.
        if (hadLink) Thread.sleep(1000)
        // 2. Open the Wi-Fi Direct link to it.
        main.post { p2p.pairingMode = true; p2p.want(); showConnecting() }
        deadline = System.currentTimeMillis() + 150_000
        while (p2p.connectedIp == null && System.currentTimeMillis() < deadline && !stopped) Thread.sleep(300)
        if (p2p.connectedIp == null) {
            return false to (if (!Radios.wifiOn(this)) "Wi-Fi went off. Turn it on, then try again."
            else "Found ${prefs.pcP2pName} over Bluetooth but couldn't connect over Wi-Fi Direct. Keep both close with Wi-Fi " +
                "on and try again; if it keeps failing, Save logs on both.")
        }
        // The PC's side of a fresh link can take a moment; keep trying for a while before giving up.
        var conn: PeerResolver.Connection? = null
        val until = System.currentTimeMillis() + 12_000
        var tries = 0
        while (conn == null && System.currentTimeMillis() < until && !stopped && p2p.connectedIp != null) {
            tries++
            conn = PeerResolver.connect(candidates(), 2000)
            if (conn == null) {
                Logs.w("pairing: connecting to the PC at ${p2p.connectedIp}:${Proto.TCP_PORT} failed (try $tries): ${PeerResolver.lastError}")
                Thread.sleep(800)
            }
        }
        if (conn == null) {
            Logs.w("pairing: the PC never answered over the link; VPN active: ${Diagnostics.vpnActive(this)}")
            return false to "Couldn't reach your PC. A firewall or antivirus on the PC is probably blocking WinDrop" +
                (if (Diagnostics.vpnActive(this)) ", or the VPN on this phone is." else ".")
        }
        Logs.i("pairing: connected to the PC at ${conn.address}")
        conn.socket.use { s ->
            tune(s)
            s.soTimeout = 75_000  // the PC waits up to a minute for its user to confirm
            val r = try {
                Pairing.run(s, prefs.deviceId, prefs.deviceName) { sas -> main.post { pairSas = sas; pairOnSas(sas) } }
            } catch (e: Incompatible) {
                reportIncompatible(e.peer)
                return false to (if (e.peer.older) "The PC has WinDrop ${e.peer.version}, which can't pair with this app (${Crypto.appVersion}). Update WinDrop on the PC: tray icon → Update."
                else "The PC has WinDrop ${e.peer.version}, which can't pair with this app (${Crypto.appVersion}). Update this app: Check for updates, below.")
            } ?: return false to "Not paired: pairing closed on the PC, or it was declined there."
            prefs.pairKey = r.key
            prefs.pairedPcId = r.pcId
            prefs.lastPcName = r.pcName
            publishShareShortcut(this)
            lastActivity = System.currentTimeMillis()
            main.postDelayed({ flushOutbox(); uploadLogs() }, 500)
            return true to "Paired with ${r.pcName}"
        }
    }

    // ---- logs ----
    /** Ships buffered log lines to the PC (offer type 3); they land in its windrop.log. */
    private fun uploadLogs() = sender.execute {
        val text = Logs.drain()
        if (text.isEmpty()) return@execute
        val ok = runCatching {
            val (s, ch) = openChannel() ?: return@runCatching false
            s.use {
                val bytes = text.toByteArray()
                Proto.writeOffer(ch.output, Proto.Offer(Proto.TYPE_LOG, bytes.size.toLong(), "phone.log", prefs.deviceName, prefs.deviceId))
                if (ch.input.read() != Proto.ACCEPTED) return@runCatching false
                ch.output.write(bytes)
                ch.input.read() == Proto.ACK_OK
            }
        }.getOrDefault(false)
        if (!ok) Logs.restore(text)
    }

    /**
     * Tells the PC we're closing the link. Otherwise Windows notices only seconds later, and a send
     * started in that gap tries to join a group that is already going away.
     */
    private fun sayBye() {
        runCatching {
            val (s, ch) = openChannel(500) ?: return
            s.use {
                s.soTimeout = 2000
                Proto.writeOffer(ch.output, Proto.Offer(Proto.TYPE_BYE, 0, "", prefs.deviceName, prefs.deviceId))
                ch.input.read()
            }
        }
    }

    /** The PC removed this phone: stop calling it and say how to pair again. */
    /** [keepLink]: the PC takes files from nearby phones, and we go on sending to it that way. */
    private fun forgetPairing(keepLink: Boolean = false) {
        if (!prefs.paired) return
        val pc = prefs.pcLabel
        Logs.w("$pc no longer has this phone paired; forgetting the pairing")
        prefs.pairKey = null
        prefs.pairedPcId = ""
        main.post {
            if (!keepLink) p2p.release()
            if (keepLink) ui(Island.Msg("$pc removed this phone", "Sending as a nearby phone: accept it on the PC", Island.G_WARN, Island.AMBER), 3500,
                "$pc removed this phone; sending as a nearby phone")
            else ui(Island.Msg("$pc removed this phone", "Pair again from the WinDrop app", Island.G_WARN, Island.AMBER), 5000,
                "$pc removed this phone. Pair again from the WinDrop app.")
            updateServiceNotification()
            notifyListeners()
        }
    }

    // ---- versions ----
    private var lastIncompatibleAt = 0L

    /** The PC runs a WinDrop that speaks another protocol: say which side needs the update. */
    private fun reportIncompatible(peer: PeerVersion) {
        Logs.w("PC has WinDrop ${peer.version} (protocol ${peer.proto}), we have ${Crypto.appVersion} (protocol ${Crypto.PROTO})")
        val now = System.currentTimeMillis()
        if (now - lastIncompatibleAt < 60_000) return
        lastIncompatibleAt = now
        if (peer.older) {
            ui(Island.Msg("The PC's WinDrop is out of date", "It has v${peer.version}. On the PC: tray icon → Update", Island.G_WARN, Island.AMBER),
                5000, "Update WinDrop on the PC (it has v${peer.version})")
        } else {
            ui(Island.Msg("This WinDrop app is out of date", "The PC has v${peer.version}. Tap to update", Island.G_WARN, Island.AMBER,
                onClick = { startActivity(Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK).putExtra(MainActivity.EXTRA_UPDATE, true)) }),
                5000, "Update WinDrop on this phone (the PC has v${peer.version})")
            io.execute { Updater.check(this, notify = true, force = true) }
        }
    }

    // ---- ui helpers ----
    private fun ui(m: Island.Msg, ms: Long, fallback: String) = main.post {
        if (island.attached) island.showMessage(m, ms) else toast(fallback)
    }

    private fun toast(s: String) = main.post { Toast.makeText(this, s, Toast.LENGTH_SHORT).show() }

    fun overlayAllowed() = Settings.canDrawOverlays(this)
}

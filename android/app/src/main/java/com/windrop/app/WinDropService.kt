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
        private const val NOTIF_OFFER = 2

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
    private val receiving = AtomicInteger(0)
    private var bleCooldownUntil = 0L

    /** Permissions granted or pairing code changed. */
    fun onSetupChanged() { ble.start(); ble.reset(); p2p.repair() }

    /** Open the link now (e.g. "Send now" on queued items). */
    fun connectNow() { if (p2p.connectedIp == null) { p2p.want(); showConnecting() } }

    /** Close the on-demand link once nothing has moved for a bit, so Android brings Wi-Fi back. */
    private val idleCheck = object : Runnable {
        override fun run() {
            if (p2p.connectedIp == null) return
            val idle = System.currentTimeMillis() - lastActivity > LINK_IDLE_MS
            if (idle && queued.get() == 0 && receiving.get() == 0 && outbox.count == 0) {
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
            if (st == android.bluetooth.BluetoothAdapter.STATE_ON) ble.start()
            else if (st == android.bluetooth.BluetoothAdapter.STATE_OFF) { Logs.w("bluetooth turned off"); ble.stop() }
        }
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
                if (!pairing) { flushOutbox(); uploadLogs() }
                main.removeCallbacks(idleCheck)
                main.postDelayed(idleCheck, 3000)
                if (island.attached && island.isConnecting) island.connectingDone()
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
        ble = BleWatcher(this, prefs) { pending, name ->
            main.post {
                // A PC in pairing mode: only interesting while we're pairing.
                if (name.isNotEmpty() && pairing && name != prefs.pcP2pName) {
                    Logs.i("ble: found PC in pairing mode: $name")
                    prefs.pcP2pName = name
                    notifyListeners()
                }
                if (!pending) return@post
                if (p2p.connectedIp != null || p2p.wanted || System.currentTimeMillis() < bleCooldownUntil) return@post
                Logs.i("ble: PC has something for us")
                p2p.want()
                showConnecting(incoming = true)
            }
        }
        ble.start()
        registerReceiver(screenOn, IntentFilter(Intent.ACTION_SCREEN_ON))
        registerReceiver(btState, IntentFilter(android.bluetooth.BluetoothAdapter.ACTION_STATE_CHANGED))
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
        ble.stop()
        p2p.stop()
        main.removeCallbacksAndMessages(null)
        runCatching { server?.close() }
        runCatching { unregisterReceiver(screenOn) }
        runCatching { unregisterReceiver(btState) }
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
            else -> "Ready · ${prefs.pcLabel}"
        }
        val n = outbox.count
        return if (n > 0) "$pc · $n waiting" else pc
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

    private fun drainPending() {
        // User asked to send: open the on-demand link (Android may ask to pause Wi-Fi for it).
        if (pending.isNotEmpty() && p2p.connectedIp == null) { p2p.want(); showConnecting() }
        while (true) {
            val item = pending.poll() ?: break
            queued.incrementAndGet()
            sender.execute { process(item) }
        }
        stopIfIdle()
    }

    private enum class Result { OK, OFFLINE, DECLINED, FAILED }

    private fun process(item: OutgoingItem) {
        try {
            if (item.outboxId != null && flushAbort) return
            val r = try { send(item) } catch (e: Exception) { Logs.w("send failed", e); Result.FAILED }
            Logs.i("send ${item.name} -> $r")
            val label = prefs.pcLabel
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
                        // While the link is coming up the pill already says "Connecting…".
                        if (!p2p.wanted) {
                            if (entry != null && island.attached) island.offline(entry.id, count)
                            else toast(if (entry != null) "$label not reachable — saved ($count waiting)" else "$label not reachable")
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
                Result.FAILED -> ui(Island.Msg("Transfer failed", item.name, Island.G_WARN, Island.RED), 3000, "Transfer failed")
            }
        } finally {
            item.close()
            lastActivity = System.currentTimeMillis()
            if (queued.decrementAndGet() == 0) main.post { island.transferEnded(); updateServiceNotification(); stopIfIdle() }
            endProgress()
        }
    }

    private fun send(original: OutgoingItem): Result {
        var item = original
        if (!item.isText && item.size < 0) {
            // Unknown length (some providers stream): spool so we can announce the size.
            val f = File.createTempFile("spool", null, cacheDir)
            item.open().use { src -> f.outputStream().use { src.copyTo(it, Proto.CHUNK) } }
            item = item.withSpooled(f)
            f.deleteOnExit()
        }
        val (s, ch) = try { openChannel() } catch (e: Incompatible) { reportIncompatible(e.peer); return Result.FAILED }
            catch (e: NotPaired) { forgetPairing(); return Result.FAILED }
            ?: return if (prefs.paired) Result.OFFLINE else Result.FAILED
        s.use {
            val out = ch.output
            val inp = DataInputStream(ch.input)
            try {
                Proto.writeOffer(out, Proto.Offer(if (item.isText) Proto.TYPE_TEXT else Proto.TYPE_FILE, item.size,
                    if (item.isText) "text.txt" else item.name, prefs.deviceName, prefs.deviceId))
                out.flush()
            } catch (e: IOException) { return Result.OFFLINE }

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
        if (stopped || flushing || !prefs.paired) return
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
        if (type != Crypto.HELLO_SESSION) return
        val ch = SecureChannel.server(c, raw) { id -> if (prefs.paired && id == prefs.pairedPcId) prefs.pairKey else null }
        if (ch == null) {
            Logs.w("refused a connection from ${c.inetAddress.hostAddress}: not the paired PC")
            return
        }
        val inp = DataInputStream(ch.input)
        val out = ch.output
        val offer = Proto.readOffer(inp) ?: return
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
        if (offer.sender.isNotEmpty() && offer.sender != prefs.lastPcName) prefs.lastPcName = offer.sender
        Logs.i("offer from the PC: ${offer.name} (${humanSize(offer.size)})")
        val name = offer.name.replace(Regex("[\\\\/:*?\"<>|\\x00-\\x1f]"), "_").ifBlank { "file" }
        out.write(Proto.ACCEPTED)  // authenticated as the paired PC: no prompt
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

    /**
     * Pairs with the PC that has "Pair phone…" open: finds it by its pairing beacon, opens the
     * link, runs the key exchange, and shows the check number ([onSas]) while the user confirms
     * it on the PC. [onDone] runs on the main thread with (success, message).
     */
    fun startPairing(onSas: (String) -> Unit, onDone: (Boolean, String) -> Unit) {
        if (pairing) return
        pairing = true
        Logs.i("pairing started")
        prefs.pcP2pName = ""      // learn it fresh from the pairing beacon
        prefs.pcP2pAddress = ""
        ble.start()
        ble.reset()  // a pairing beacon seen just before the tap must count now
        io.execute {
            val (ok, msg) = runCatching { pairOverLink(onSas) }.getOrElse { Logs.w("pairing failed", it); false to "Pairing failed: ${it.message}" }
            pairing = false
            p2p.pairingMode = false
            Logs.i("pairing: $msg")
            main.post { onDone(ok, msg); updateServiceNotification(); notifyListeners() }
        }
    }

    private fun pairOverLink(onSas: (String) -> Unit): Pair<Boolean, String> {
        val noPc = "No PC is in pairing mode. On the PC: tray icon \u2192 Pair phone\u2026, then try again."
        // 1. Find a PC with pairing open (its Bluetooth beacon carries its name).
        var deadline = System.currentTimeMillis() + 20_000
        while (prefs.pcP2pName.isEmpty() && System.currentTimeMillis() < deadline && !stopped) Thread.sleep(200)
        if (prefs.pcP2pName.isEmpty()) return false to noPc
        // 2. Open the Wi-Fi Direct link to it.
        main.post { p2p.pairingMode = true; p2p.want(); showConnecting() }
        deadline = System.currentTimeMillis() + 120_000
        while (p2p.connectedIp == null && System.currentTimeMillis() < deadline && !stopped) Thread.sleep(300)
        if (p2p.connectedIp == null) return false to "Couldn't connect to ${prefs.pcP2pName}. Try turning the phone's Wi-Fi off and on."
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
                Pairing.run(s, prefs.deviceId, prefs.deviceName) { sas -> main.post { onSas(sas) } }
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
    private fun forgetPairing() {
        if (!prefs.paired) return
        val pc = prefs.pcLabel
        Logs.w("$pc no longer has this phone paired; forgetting the pairing")
        prefs.pairKey = null
        prefs.pairedPcId = ""
        main.post {
            p2p.release()
            ui(Island.Msg("$pc removed this phone", "Pair again from the WinDrop app", Island.G_WARN, Island.AMBER), 5000,
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

package com.windrop.app

import android.Manifest
import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.wifi.WpsInfo
import android.net.wifi.p2p.WifiP2pConfig
import android.net.wifi.p2p.WifiP2pDevice
import android.net.wifi.p2p.WifiP2pManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.Parcel

/**
 * Network-independent link to the PC, AirDrop style: the PC advertises itself over Wi-Fi Direct,
 * the phone picks it out by name (learned from the PC's BLE beacon) and asks to connect with
 * push-button WPS, and the PC accepts on its own and stays group owner.
 * Both stay on whatever Wi-Fi they're on; AP isolation doesn't matter because no AP is involved.
 */
class WifiDirectLink(
    private val ctx: Context,
    private val prefs: Prefs,
    private val onPcAddress: (String?) -> Unit,
    private val onGaveUp: () -> Unit = {},
) {
    private val mgr: WifiP2pManager? = ctx.getSystemService(WifiP2pManager::class.java)
    private val main = Handler(Looper.getMainLooper())
    private var channel: WifiP2pManager.Channel? = null
    private var pcIp: String? = null
    private var backoffMs = MIN_BACKOFF
    private var running = false
    private val poll = object : Runnable {
        // PEERS_CHANGED isn't delivered reliably on every ROM; poll while a search is running.
        override fun run() { if (searching && !connecting && pcIp == null) { requestPeers(); main.postDelayed(this, 500) } }
    }
    private var searching = false
    private var connecting = false
    private val retry = Runnable {
        // Window over without a link: stop the radio-hungry search before trying again.
        if (searching && pcIp == null) channel?.let { ch -> runCatching { mgr?.stopPeerDiscovery(ch, null) } }
        searching = false
        if (connecting && pcIp == null) {
            // A join that hasn't finished by now won't (e.g. the PC's previous group was still
            // closing). Android would keep at it for 60 s; cancel and search again right away.
            Logs.w("p2p connect timed out, retrying")
            connecting = false
            channel?.let { ch -> runCatching { mgr?.cancelConnect(ch, null) } }
            main.postDelayed({ tryConnect() }, 500)
        } else tryConnect()
    }

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            if (i.action == WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION) { if (searching) requestPeers() } else refresh()
        }
    }

    val connectedIp: String? get() = pcIp

    /**
     * The link is on demand: joining the PC usually costs the phone its Wi-Fi (different channel),
     * so Android asks the user and then reconnects Wi-Fi by itself once we [release] the group.
     */
    var wanted = false
        private set

    /** Pairing in progress: connect to the PC found in pairing mode even though we hold no key yet. */
    @Volatile var pairingMode = false

    fun want() {
        if (!wanted) Logs.i("p2p wanted")
        wanted = true
        main.removeCallbacks(giveUp)
        if (pcIp == null) main.postDelayed(giveUp, if (pairingMode) PAIRING_GIVE_UP_MS else GIVE_UP_MS)
        kick()
    }

    /** Declined Android's Wi-Fi prompt, PC out of range...: stop asking; the outbox keeps the items. */
    private val giveUp: Runnable = Runnable {
        if (pcIp != null || !wanted) return@Runnable
        // A connection being set up right now gets to finish; cancelling it mid-way can leave the
        // Wi-Fi driver confused for a while.
        if (connecting) { main.postDelayed(giveUp, 15_000); return@Runnable }
        Logs.i("p2p gave up")
        release()
        onGaveUp()
    }
    private var discoverErrors = 0
    private var bursts = 0

    fun release() {
        if (!wanted) return
        Logs.i("p2p released")
        wanted = false
        searching = false
        connecting = false
        main.removeCallbacks(retry)
        main.removeCallbacks(poll)
        main.removeCallbacks(giveUp)
        channel?.let { ch ->
            runCatching { mgr?.stopPeerDiscovery(ch, null) }
            runCatching { mgr?.cancelConnect(ch, null) }
            runCatching { mgr?.removeGroup(ch, null) }
        }
        setPc(null)
    }

    /** Drops and re-creates the framework channel, keeping what we want (wanted, etc.). */
    private fun resetChannel() {
        Logs.w("p2p: resetting the Wi-Fi Direct channel")
        val keep = wanted
        stop()
        main.postDelayed({ start(); if (keep) want() }, 1500)
    }

    fun start() {
        if (running || mgr == null) return
        running = true
        channel = mgr.initialize(ctx, Looper.getMainLooper()) {
            // Framework dropped us (Wi-Fi toggled, etc.): rebuild.
            channel = null
            if (running) { running = false; main.postDelayed({ start() }, 3000) }
        }
        val f = IntentFilter().apply {
            addAction(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION)
            addAction(WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION)
            addAction(WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION)
        }
        if (Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(receiver, f, Context.RECEIVER_NOT_EXPORTED)
        else ctx.registerReceiver(receiver, f)
        refresh()
    }

    fun stop() {
        if (!running) return
        running = false
        main.removeCallbacks(retry)
        runCatching { ctx.unregisterReceiver(receiver) }
        channel?.let { ch -> runCatching { mgr?.removeGroup(ch, null) } }
        channel?.let { ch -> if (Build.VERSION.SDK_INT >= 27) runCatching { ch.close() } }
        channel = null
        setPc(null)
    }

    /** New pairing code entered: drop any current group and join the new one. */
    fun repair() {
        backoffMs = MIN_BACKOFF
        channel?.let { ch -> runCatching { mgr?.removeGroup(ch, null) } }
        setPc(null)
        main.removeCallbacks(retry)
        if (wanted) main.postDelayed(retry, 1000)
    }

    /** Nudge: try now instead of waiting out the back-off (e.g. something is waiting to be sent). */
    fun kick() {
        if (pcIp != null || !running || !wanted) return
        backoffMs = MIN_BACKOFF
        main.removeCallbacks(retry)
        main.post(retry)
    }

    private fun setPc(ip: String?) {
        if (ip == pcIp) return
        pcIp = ip
        Logs.i("p2p link ${ip ?: "down"}")
        onPcAddress(ip)
    }

    private fun refresh() {
        val ch = channel ?: return
        mgr?.requestConnectionInfo(ch) { info ->
            if (info != null && info.groupFormed && !info.isGroupOwner && info.groupOwnerAddress != null) {
                backoffMs = MIN_BACKOFF
                searching = false
                connecting = false
                main.removeCallbacks(retry)
                main.removeCallbacks(giveUp)
                setPc(info.groupOwnerAddress.hostAddress)
            } else {
                if (info != null && info.groupFormed && info.isGroupOwner && connecting) {
                    // Our join made the phone the group owner: the PC can't work that way. Undo it.
                    Logs.w("p2p: phone became group owner; removing the group")
                    runCatching { mgr?.removeGroup(ch, null) }
                }
                setPc(null)
                // A join in progress already has its timeout queued; only start one if nothing is.
                if (wanted && !main.hasCallbacks(retry)) schedule(0)
            }
        }
    }

    private fun schedule(delay: Long = backoffMs) {
        if (!running) return
        main.removeCallbacks(retry)
        main.postDelayed(retry, delay)
    }

    private fun hasPermission() =
        if (Build.VERSION.SDK_INT >= 33) ctx.checkSelfPermission(Manifest.permission.NEARBY_WIFI_DEVICES) == PackageManager.PERMISSION_GRANTED
        else ctx.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED

    /** Step 1: look for the PC. Peers arrive via WIFI_P2P_PEERS_CHANGED_ACTION -> [onPeers]. */
    @SuppressLint("MissingPermission")
    private fun tryConnect() {
        val ch = channel ?: return
        if (!running || pcIp != null || !wanted) return
        if (!prefs.paired && !pairingMode) return  // nothing to connect to yet
        if (!hasPermission()) { Logs.w("no nearby-devices permission"); return }
        searching = true
        connecting = false
        discover(ch)
        main.removeCallbacks(rediscover)
        main.postDelayed(rediscover, REDISCOVER_MS)
        // Give discovery and the join a bounded window, then back off.
        schedule(SEARCH_TIMEOUT)
        backoffMs = minOf(backoffMs * 2, MAX_BACKOFF)
    }

    /**
     * One discovery burst. With the screen off Android's normal find is slow to hear the PC
     * (often 30 s+), so where supported only the three social channels are scanned (where the PC
     * listens), and the burst is restarted every few seconds. Every other burst is a normal find
     * over all channels: a PC whose Wi-Fi card only answers while running its own group sits on
     * that group's channel, often 5 GHz.
     */
    @SuppressLint("MissingPermission")
    private fun discover(ch: WifiP2pManager.Channel) {
        val cb = object : WifiP2pManager.ActionListener {
            override fun onFailure(reason: Int) {
                Logs.w("p2p discover failed: $reason")
                when {
                    // Busy: usually a stale connect left over (e.g. from before an app update).
                    // Clear it so the next search isn't refused too.
                    reason == WifiP2pManager.BUSY -> runCatching { mgr?.cancelConnect(ch, null) }
                    // Repeated plain errors: the framework channel is wedged; start a fresh one.
                    ++discoverErrors >= 3 -> { discoverErrors = 0; resetChannel() }
                    else -> { searching = false; backOff() }
                }
            }
            override fun onSuccess() { discoverErrors = 0; main.removeCallbacks(poll); main.postDelayed(poll, 300) }
        }
        val m = mgr ?: return
        if (bursts++ % 2 == 0 && Build.VERSION.SDK_INT >= 34 && runCatching { m.isChannelConstrainedDiscoverySupported }.getOrDefault(false)) {
            m.discoverPeersOnSocialChannels(ch, cb)
        } else {
            m.discoverPeers(ch, cb)
        }
    }

    private val rediscover = object : Runnable {
        override fun run() {
            if (!searching || connecting || pcIp != null) return
            val ch = channel ?: return
            runCatching { mgr?.stopPeerDiscovery(ch, null) }
            main.postDelayed({ if (searching && !connecting && pcIp == null) discover(ch) }, 150)
            main.postDelayed(this, REDISCOVER_MS)
        }
    }

    @SuppressLint("MissingPermission")
    private fun requestPeers() {
        val ch = channel ?: return
        mgr?.requestPeers(ch) { list -> onPeers(list?.deviceList.orEmpty()) }
    }

    private fun isOurPc(d: WifiP2pDevice): Boolean {
        if (d.deviceAddress == prefs.pcP2pAddress && d.deviceAddress.isNotEmpty()) return true
        // The PC's Wi-Fi Direct name comes from its BLE beacon (possibly cut to 17 chars), or from
        // its offers / HELLO. Android doesn't expose the PC's vendor IEs, so a name is all we get.
        val name = prefs.pcP2pName
        if (name.isNotEmpty() && d.deviceName.startsWith(name, ignoreCase = true)) return true
        return prefs.lastPcName.isNotEmpty() && d.deviceName.equals(prefs.lastPcName, ignoreCase = true)
    }

    /** Step 2: found it -> push-button connect; the PC accepts on its own and stays group owner. */
    @SuppressLint("MissingPermission")
    private fun onPeers(peers: Collection<WifiP2pDevice>) {
        val ch = channel ?: return
        if (!searching || connecting || pcIp != null) return
        val pc = peers.firstOrNull { isOurPc(it) } ?: return
        connecting = true
        Logs.i("p2p found PC ${pc.deviceName} ${pc.deviceAddress}")
        prefs.pcP2pAddress = pc.deviceAddress
        val cfg = temporaryConfig(pc.deviceAddress)
        mgr?.connect(ch, cfg, object : WifiP2pManager.ActionListener {
            override fun onSuccess() {
                Logs.i("p2p connect requested")
                main.removeCallbacks(retry)
                main.postDelayed(retry, CONNECT_TIMEOUT)
            }
            override fun onFailure(reason: Int) {
                Logs.w("p2p connect failed: $reason")
                connecting = false
                if (reason == WifiP2pManager.BUSY) runCatching { mgr.cancelConnect(ch, null) }
                // Discovery stopped with the connect: search again now, not when the window ends.
                schedule(1000)
            }
        })
    }

    private fun backOff() = schedule(backoffMs)

    /**
     * Push-button config for a fresh, non-persistent group. The default netId means "re-invoke a
     * saved group", which Android does with an invitation that Windows always refuses ("no common
     * channel", then Android offers to drop Wi-Fi). netId has no public setter outside the
     * network-name Builder, so flip it in the parcel: find it right after a sentinel GO intent.
     */
    private fun temporaryConfig(address: String): WifiP2pConfig {
        val base = WifiP2pConfig().apply {
            deviceAddress = address
            wps.setup = WpsInfo.PBC
            groupOwnerIntent = SENTINEL_INTENT
        }
        val p = Parcel.obtain()
        try {
            base.writeToParcel(p, 0)
            val n = p.dataSize()
            for (off in 0..n - 8 step 4) {
                p.setDataPosition(off)
                if (p.readInt() == SENTINEL_INTENT && p.readInt() == NET_ID_PERSISTENT) {
                    p.setDataPosition(off)
                    p.writeInt(0)  // groupOwnerIntent: let the PC own the group
                    p.writeInt(NET_ID_TEMPORARY)
                    p.setDataPosition(0)
                    return WifiP2pConfig.CREATOR.createFromParcel(p)
                }
            }
            Logs.w("p2p: netId not found in parcel; using persistent config")
        } catch (e: Exception) {
            Logs.w("p2p: temporary config failed", e)
        } finally {
            p.recycle()
        }
        return base.apply { groupOwnerIntent = 0 }
    }

    companion object {
        private const val TAG = "WinDrop"
        private const val MIN_BACKOFF = 5_000L
        private const val MAX_BACKOFF = 60_000L
        private const val SEARCH_TIMEOUT = 30_000L
        private const val REDISCOVER_MS = 6_000L
        private const val CONNECT_TIMEOUT = 15_000L
        private const val GIVE_UP_MS = 90_000L
        // Pairing: the PC's first Wi-Fi Direct attempt sometimes fails; leave room for retries.
        private const val PAIRING_GIVE_UP_MS = 150_000L
        private const val SENTINEL_INTENT = 13
        private const val NET_ID_PERSISTENT = -2
        private const val NET_ID_TEMPORARY = -1

    }
}

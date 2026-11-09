package com.windrop.app

import android.animation.Animator
import android.animation.AnimatorListenerAdapter
import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PathMeasure
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextUtils
import android.view.DragEvent
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.VelocityTracker
import android.view.View
import android.view.WindowManager
import android.view.animation.DecelerateInterpolator
import android.view.animation.OvershootInterpolator
import kotlin.math.abs
import kotlin.math.min

/**
 * The top-edge "dynamic island" overlay. Main thread only.
 * Idle it is a 2dp, untouchable strip (still receives global DRAG_STARTED); it grows when something happens.
 *
 * Gestures, the same everywhere: tap a message to read all of it (it stays until you tap
 * elsewhere, tap it again or swipe it); swipe up to put the pill away (whatever it shows carries
 * on: a question moves to a notification); swipe sideways to cancel what it shows (a send stops,
 * a question is answered "no", a message goes).
 */
class Island(private val svc: WinDropService) {
    companion object {
        const val BTN_ACCEPT = 1
        const val BTN_DECLINE = 2
        const val BTN_CANCEL = 3
        const val BTN_ACCEPT_ALL = 4  // this offer and whatever else that sender sends next
        const val BTN_DECLINE_ALL = 5
        const val BTN_MSG_ACTION = 6  // an opened message's own action (its onClick)
        const val BTN_PICK = 100  // + index of the PC picked in the chooser

        const val BLUE = 0xFF2F7CF6.toInt()
        const val GREEN = 0xFF2BB673.toInt()
        const val AMBER = 0xFFF5A524.toInt()
        const val RED = 0xFFE5484D.toInt()
        const val GRAY_BTN = 0xFF3A3A3A.toInt()

        const val G_UP = 1
        const val G_DOWN = 2
        const val G_CHECK = 3
        const val G_WARN = 4
        const val G_X = 5
        const val G_LINK = 6

        /** A pill shows this long, then gets out of the way. */
        const val PILL_MS = 3_500L
    }

    /** [glyph]: a round icon-only button (accept / decline) instead of a label. */
    class Btn(val id: Int, val label: String, val fill: Int, val glyph: Int = 0) {
        val rect = RectF()
    }

    data class Look(
        val w: Float = 180f, val h: Float = 2f, val y: Float = 0f, val idle: Boolean = true,
        val title: String = "", val sub: String = "", val glyph: Int = 0, val accent: Int = BLUE,
        val progress: Boolean = false, val highlight: Boolean = false, val clickable: Boolean = false,
        val buttons: List<Btn> = emptyList(),
        val stack: Boolean = false,  // buttons as full-width rows under the title (the PC chooser)
        val expanded: Boolean = false,  // the whole title and sub wrapped, labelled buttons underneath
        val pairs: Boolean = false,  // expanded: buttons in rows of two (the offer's four)
        // A question that runs out by itself (uptime ms): a blue line goes round inside the edge from
        // the top centre, all the way round when the time is up.
        val ringFrom: Long = 0L, val ringTo: Long = 0L,
    )

    /**
     * [onClick]: what the message offers, as a button labelled [action] once it's opened.
     * [cancels]: about a send in progress, so swiping it sideways stops that send.
     */
    class Msg(
        val title: String, val sub: String = "", val glyph: Int = 0, val accent: Int = BLUE,
        val buttons: List<Btn> = emptyList(), val onClick: (() -> Unit)? = null,
        val action: String = "Open", val cancels: Boolean = false,
    )

    private class OfferUi(val id: Long, val name: String, val size: Long, val sender: String, val since: Long, val until: Long)
    private class XferUi(val sending: Boolean, val name: String, val fromOutbox: Boolean)
    private class Deferred(val m: Msg, val ms: Long, val keepBatch: Boolean, val at: Long)

    private val wm = svc.getSystemService(WindowManager::class.java)
    private val density = svc.resources.displayMetrics.density
    private val main = Handler(Looper.getMainLooper())
    private var view: PillView? = null
    private lateinit var lp: WindowManager.LayoutParams

    private var drag = 0
    private var offer: OfferUi? = null
    private var xfer: XferUi? = null
    private var msg: Msg? = null
    private var msgOpen = false  // tapped: shown whole until it's closed
    private var deferred: Deferred? = null  // arrived while a message was open; shown once it closes
    private val outboxBatch = mutableListOf<Long>()
    private var chooser: List<Pair<String, String>>? = null  // (id, label) per row
    private var chooserTitle = ""
    private var chooserHidden = false  // swiped up: asked in a notification instead
    private var onPick: ((String?) -> Unit)? = null
    private var offerExpanded = false
    private val chooserTimeout = Runnable { pick(null) }

    var look = Look(); private set
    private var cw = 180f
    private var ch = 2f
    private var cy = 0f
    private var anim: ValueAnimator? = null
    private var slide: ValueAnimator? = null
    private val expire = Runnable { if (!msgOpen) { msg = null; outboxBatch.clear(); refresh() } }
    private var xferHidden = false
    private var offerHidden = false
    private val hideXfer = Runnable { if (xfer != null) { xferHidden = true; refresh() } }

    // Sits just under the status bar so it never hides behind it.
    private val topInset: Int by lazy {
        val id = svc.resources.getIdentifier("status_bar_height", "dimen", "android")
        (if (id > 0) svc.resources.getDimensionPixelSize(id) else px(24f)) + px(4f)
    }

    val attached get() = view != null

    private fun px(dp: Float) = (dp * density + 0.5f).toInt()

    @SuppressLint("ClickableViewAccessibility")
    fun attach() {
        if (view != null || !Settings.canDrawOverlays(svc)) return
        val v = PillView(svc, this)
        lp = WindowManager.LayoutParams(
            px(180f), px(2f),
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            idleFlags(),
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
            y = topInset
            layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            title = "WinDrop"
        }
        v.setOnDragListener { _, e -> onDrag(e) }
        try {
            wm.addView(v, lp)
            view = v
        } catch (e: Exception) {
            Logs.w("addView failed", e)
        }
    }

    /** Fully removes the window so banking apps stop seeing an obscuring overlay. */
    fun detach() {
        anim?.cancel()
        slide?.cancel()
        main.removeCallbacks(expire)
        view?.let { runCatching { wm.removeViewImmediate(it) } }
        view = null
    }

    private fun idleFlags() = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
        WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
        WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS

    // Watches touches outside too: a tap elsewhere closes an opened message.
    private fun activeFlags() = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
        WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
        WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH or
        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
        WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS

    // ---- drag & drop from other apps (needs the source to use DRAG_FLAG_GLOBAL) ----
    private fun onDrag(e: DragEvent): Boolean {
        when (e.action) {
            DragEvent.ACTION_DRAG_STARTED -> { drag = 1; refresh() }
            DragEvent.ACTION_DRAG_ENTERED -> {
                drag = 2
                view?.performHapticFeedback(HapticFeedbackConstants.CONFIRM)
                refresh()
            }
            DragEvent.ACTION_DRAG_EXITED -> { drag = 1; refresh() }
            DragEvent.ACTION_DROP -> {
                drag = 0
                val choose = svc.nearbyPcNames().isNotEmpty() || !svc.prefs.paired  // then the pill asks where to
                svc.handleDrop(e)
                if (!choose) showMessage(Msg("Sending to ${svc.prefs.pcLabel}…", "Connecting", G_UP, cancels = true), 1500)
                return true
            }
            DragEvent.ACTION_DRAG_ENDED -> if (drag != 0) { drag = 0; refresh() }
        }
        return true
    }

    // ---- events from the service ----
    /**
     * "Send to which PC?": one row per (id, label). [pick] gets the id, or null when cancelled or
     * left unanswered. [updateChooser] keeps the rows current while it's open.
     */
    fun showChooser(title: String, targets: List<Pair<String, String>>, pick: (String?) -> Unit) {
        onPick?.invoke(null)  // an older question is moot now
        if (chooserHidden) svc.closeChooserNotification()
        chooser = targets
        chooserTitle = title
        chooserHidden = false
        onPick = pick
        main.removeCallbacks(chooserTimeout)
        main.postDelayed(chooserTimeout, 20_000)
        view?.performHapticFeedback(HapticFeedbackConstants.CONFIRM)
        refresh()
    }

    fun updateChooser(targets: List<Pair<String, String>>) {
        if (chooser == null || chooser == targets) return
        chooser = targets
        if (chooserHidden) svc.showChooserNotification(chooserTitle, targets)
        refresh()
    }

    val choosing get() = chooser != null

    private fun pick(i: Int?) {
        val cb = onPick ?: return
        val id = i?.let { chooser?.getOrNull(it)?.first }
        if (chooserHidden) svc.closeChooserNotification()
        onPick = null
        chooser = null
        chooserHidden = false
        main.removeCallbacks(chooserTimeout)
        refresh()
        cb(id)
    }

    /** Picked in the chooser's notification (null: that notification was swiped away). */
    fun pickId(id: String?) {
        val i = chooser?.indexOfFirst { it.first == id }?.takeIf { it >= 0 }
        pick(i)
    }

    /** [until]: when it counts as declined by itself (uptime ms). */
    fun showOffer(id: Long, name: String, size: Long, sender: String, until: Long) {
        offer = OfferUi(id, name, size, sender, SystemClock.uptimeMillis(), until)
        offerHidden = false
        offerExpanded = false  // every request starts small; a tap shows the whole name
        refresh()
    }

    fun closeOffer(id: Long) {
        if (offer?.id == id) { offer = null; refresh() }
    }

    fun transferStarted(sending: Boolean, name: String, fromOutbox: Boolean) {
        xfer = XferUi(sending, name, fromOutbox)
        if (!msgOpen) msg = null
        // Show progress for a moment, then step aside; the notification keeps the progress bar.
        xferHidden = false
        main.removeCallbacks(hideXfer)
        main.postDelayed(hideXfer, PILL_MS)
        refresh()
    }

    fun transferEnded() { xfer = null; main.removeCallbacks(hideXfer); refresh() }

    /** Swiped up: get out of the way now. Nothing is cancelled: a question moves to a notification. */
    fun dismiss() {
        main.removeCallbacks(expire)
        msg = null
        msgOpen = false
        deferred = null
        outboxBatch.clear()
        val c = chooser
        if (c != null && !chooserHidden) {
            chooserHidden = true
            main.removeCallbacks(chooserTimeout)
            main.postDelayed(chooserTimeout, 60_000)  // the notification's own timeout
            svc.showChooserNotification(chooserTitle, c)
        }
        if (xfer != null) xferHidden = true
        offer?.let { o ->
            if (!offerHidden) { offerHidden = true; svc.offerToNotification(o.id, o.name, o.size, o.sender) }
        }
        refresh()
    }

    /**
     * Swiped sideways: cancel what the pill is asking or getting ready (picking the PC, connecting,
     * waiting for it to accept). A transfer already under way is never cancelled this way: its pill
     * just steps aside, like swiping up.
     */
    private fun swipedAway() {
        val o = offer
        val x = xfer
        val m = msg
        val running = x != null
        when {
            o != null && !offerHidden -> {
                Logs.i("pill: offer swiped away (declined)")
                svc.decide(o.id, false)
                offer = null
                refresh()
            }
            chooser != null && !chooserHidden -> pick(null)
            !running && m != null && outboxBatch.isNotEmpty() -> onButton(BTN_CANCEL)
            !running && m != null && (m.cancels || m === connectingMsg && connectingOut) -> {
                svc.cancelTransfers()
                connectingMsg = null
                closeMsg(showNext = false)
                showMessage(Msg("Cancelled", "Nothing more is sent", G_X, GRAY_BTN), 1500)
            }
            m != null && (msgOpen || x == null || xferHidden) -> closeMsg()
            x != null && !xferHidden -> { xferHidden = true; refresh() }
            else -> refresh()
        }
    }

    /** A tap outside the pill: an opened message closes. */
    fun outside() {
        if (msgOpen && msg != null && offer == null && chooser == null) closeMsg()
    }

    private var connectingMsg: Msg? = null
    private var connectingOut = false
    val isConnecting get() = connectingMsg != null && msg === connectingMsg

    /** The on-demand link is coming up (Android may ask about Wi-Fi meanwhile). */
    fun connecting(pc: String, incoming: Boolean) {
        val m = Msg(
            "Connecting to $pc…",
            if (incoming) "$pc has something for you" else "Sends as soon as it connects",
            if (incoming) G_DOWN else G_UP, BLUE,
        )
        connectingMsg = m
        connectingOut = !incoming
        showMessage(m, PILL_MS)
    }

    fun connectingDone() {
        if (isConnecting && !msgOpen) { msg = null; refresh() }
        connectingMsg = null
    }

    fun offline(outboxId: Long, count: Int) {
        outboxBatch += outboxId
        showMessage(
            Msg("${svc.prefs.pcLabel} not reachable — $count not sent yet", "Goes out by itself when it's back", G_WARN, AMBER,
                listOf(Btn(BTN_CANCEL, "Cancel", GRAY_BTN))),
            4000, keepBatch = true,
        )
    }

    fun showMessage(m: Msg, ms: Long, keepBatch: Boolean = false) {
        if (msgOpen && msg != null) {
            // Being read: this one waits until that's closed.
            deferred = Deferred(m, ms, keepBatch, SystemClock.uptimeMillis())
            return
        }
        if (!keepBatch) outboxBatch.clear()
        msg = m
        msgOpen = false
        main.removeCallbacks(expire)
        main.postDelayed(expire, minOf(ms, PILL_MS))
        refresh()
    }

    /** The message goes; one that waited meanwhile shows now, if it's still news. */
    private fun closeMsg(showNext: Boolean = true) {
        val d = deferred
        deferred = null
        msg = null
        msgOpen = false
        main.removeCallbacks(expire)
        if (showNext && d != null && SystemClock.uptimeMillis() - d.at < 8_000) showMessage(d.m, d.ms, d.keepBatch)
        else { outboxBatch.clear(); refresh() }
    }

    fun onButton(id: Int) {
        when (id) {
            in BTN_PICK until BTN_PICK + 10 -> pick(id - BTN_PICK)
            BTN_ACCEPT, BTN_DECLINE, BTN_ACCEPT_ALL, BTN_DECLINE_ALL -> offer?.let {
                svc.decide(it.id, id == BTN_ACCEPT || id == BTN_ACCEPT_ALL, all = id == BTN_ACCEPT_ALL || id == BTN_DECLINE_ALL)
                offer = null
                refresh()
            }
            BTN_CANCEL -> {
                val ids = outboxBatch.toList()
                outboxBatch.clear()
                svc.cancelOutbox(ids)
                msgOpen = false
                showMessage(Msg(if (ids.size > 1) "Cancelled ${ids.size} items" else "Cancelled", "Won't be sent", G_X, GRAY_BTN), 1500)
            }
            BTN_MSG_ACTION -> {
                val click = msg?.onClick
                closeMsg(showNext = false)
                click?.invoke()
            }
        }
    }

    fun onBodyClick() {
        if (offer != null && !offerHidden && drag == 0 && chooser == null) {
            offerExpanded = !offerExpanded  // show (or fold away) the whole name
            refresh()
            return
        }
        if (msg == null || drag != 0 || chooser != null && !chooserHidden) return
        if (msgOpen) { closeMsg(); return }
        msgOpen = true  // the whole message, until it's closed
        main.removeCallbacks(expire)
        refresh()
    }

    // ---- swiping sideways: the pill follows the finger, and goes if let go far enough ----
    fun dragX(dx: Float) {
        val v = view ?: return
        if (look.idle) return
        slide?.cancel()
        lp.x = dx.toInt()
        v.alpha = slideAlpha(dx)
        runCatching { wm.updateViewLayout(v, lp) }
    }

    fun releaseX(dx: Float, vx: Float) {
        val v = view ?: return
        val w = px(look.w).toFloat()
        val away = !look.idle && (abs(dx) > w * 0.35f || abs(vx) > 1000 * density && vx * dx > 0)
        val screen = svc.resources.displayMetrics.widthPixels
        val to = if (!away) 0 else if (dx > 0) screen else -screen
        slide?.cancel()
        slide = ValueAnimator.ofInt(lp.x, to).apply {
            duration = if (away) 160 else 220
            interpolator = DecelerateInterpolator(1.4f)
            addUpdateListener {
                lp.x = it.animatedValue as Int
                v.alpha = slideAlpha(lp.x.toFloat())
                runCatching { wm.updateViewLayout(v, lp) }
            }
            addListener(object : AnimatorListenerAdapter() {
                private var cancelled = false
                override fun onAnimationCancel(animation: Animator) { cancelled = true }
                override fun onAnimationEnd(animation: Animator) {
                    if (cancelled) return
                    if (away) {
                        v.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                        // Back in the middle as a sliver too small to draw, so whatever comes next grows from there.
                        cw = look.w * 0.55f; ch = 4f
                        lp.x = 0; lp.width = px(cw); lp.height = px(ch)
                        v.alpha = 1f
                        runCatching { wm.updateViewLayout(v, lp) }
                        swipedAway()
                        return
                    }
                    lp.x = 0
                    v.alpha = 1f
                    runCatching { wm.updateViewLayout(v, lp) }
                }
            })
            start()
        }
    }

    private fun slideAlpha(dx: Float) = (1f - abs(dx) / (px(look.w) * 0.9f)).coerceIn(0.1f, 1f)

    // ---- layout + animation ----
    private var lastKey = ""

    /** Where the drop pill sits: at the top, or below a drag panel the phone opens up there. */
    private fun dragY(): Float =
        if (svc.prefs.dropLow) svc.resources.displayMetrics.heightPixels / density * 0.3f else 8f

    private fun refresh() {
        val v = view ?: return
        val screenDp = svc.resources.displayMetrics.widthPixels / density
        val wide = min(screenDp - 24f, 380f)
        val textW = px(wide - 12f - 34f - 10f - 12f)
        val o = offer
        val x = xfer
        val m = msg
        val choose = chooser?.takeIf { !chooserHidden }
        look = when {
            drag != 0 -> Look(
                if (drag == 2) 256f else 240f, if (drag == 2) 60f else 56f, dragY(), false,
                if (drag == 2) "Release to send" else "Drop to send",
                svc.nearbyPcNames().let { n ->
                    when {
                        n.isEmpty() && svc.prefs.paired -> "To ${svc.prefs.pcLabel}"
                        n.isEmpty() -> "Looks for nearby PCs next"
                        svc.prefs.paired -> "Pick the PC next"
                        else -> "To a nearby PC: pick it next"
                    }
                },
                G_UP, BLUE,
                highlight = drag == 2,
            )
            o != null && !offerHidden && offerExpanded -> {
                val sub = "From ${o.sender} · ${humanSize(o.size)}"
                val th = v.textHeightDp(o.name, true, textW, 5)
                val sh = v.textHeightDp(sub, false, textW, 2)
                Look(
                    wide, 16f + th + 4f + sh + 14f + 36f + 8f + 36f + 14f, 8f, false, o.name, sub,
                    G_DOWN, BLUE, clickable = true, expanded = true, pairs = true,
                    // "... all" (the rest from this sender too) on its own row above.
                    buttons = listOf(
                        Btn(BTN_DECLINE_ALL, "Decline all", GRAY_BTN), Btn(BTN_ACCEPT_ALL, "Accept all", GRAY_BTN),
                        Btn(BTN_DECLINE, "Decline", GRAY_BTN), Btn(BTN_ACCEPT, "Accept", BLUE),
                    ),
                    ringFrom = o.since, ringTo = o.until,
                )
            }
            o != null && !offerHidden -> Look(
                wide, 60f, 8f, false, o.name, "From ${o.sender} · ${humanSize(o.size)}", G_DOWN, BLUE, clickable = true,
                buttons = listOf(Btn(BTN_DECLINE, "", GRAY_BTN, G_X), Btn(BTN_ACCEPT, "", BLUE, G_CHECK)),
                ringFrom = o.since, ringTo = o.until,
            )
            choose != null -> Look(
                wide, 64f + 48f * choose.size + if (choose.isEmpty()) 0f else 4f, 8f, false, chooserTitle,
                "Looking for nearby PCs…", G_UP, BLUE,
                buttons = choose.mapIndexed { i, t -> Btn(BTN_PICK + i, t.second, GRAY_BTN) }, stack = true,
            )
            x != null && !xferHidden && !(msgOpen && m != null) -> Look(
                min(wide, 320f), 60f, 8f, false,
                if (!x.sending) "Receiving ${x.name}" else if (x.fromOutbox) "${svc.prefs.pcLabel} is back — sending ${x.name}" else "Sending ${x.name}",
                "", if (x.sending) G_UP else G_DOWN, BLUE, progress = true,
            )
            m != null && msgOpen -> {
                val btns = m.buttons + (m.onClick?.let { listOf(Btn(BTN_MSG_ACTION, m.action, BLUE)) } ?: emptyList())
                val th = v.textHeightDp(m.title, true, textW, 6)
                val sh = if (m.sub.isEmpty()) 0f else 4f + v.textHeightDp(m.sub, false, textW, 10)
                Look(
                    wide, 16f + maxOf(th + sh, 34f) + (if (btns.isEmpty()) 16f else 14f + 36f + 14f), 8f, false,
                    m.title, m.sub, m.glyph, m.accent, clickable = true, buttons = btns, expanded = true,
                )
            }
            m != null -> Look(
                if (m.buttons.isEmpty()) min(wide, 320f) else wide, 60f, 8f, false, m.title, m.sub, m.glyph, m.accent,
                clickable = true, buttons = m.buttons,
            )
            else -> Look()
        }
        // New content fades in; the size animates by itself.
        val key = "${look.title}\u0001${look.sub}\u0001${look.buttons.size}\u0001${look.expanded}"
        if (key != lastKey) { lastKey = key; v.fadeIn() }
        lp.flags = if (look.idle) idleFlags() else activeFlags()
        animateTo(look.w, look.h, look.y, v)
        v.setProgressTicking(look.progress || look.ringTo > 0)
    }

    private fun animateTo(tw: Float, th: Float, ty: Float, v: PillView) {
        anim?.cancel()
        val sw = cw; val sh = ch; val sy = cy
        val growing = th > sh
        anim = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = if (growing) 340 else 240
            interpolator = if (growing) OvershootInterpolator(0.9f) else DecelerateInterpolator(1.6f)
            addUpdateListener {
                val f = it.animatedValue as Float
                cw = sw + (tw - sw) * f
                ch = (sh + (th - sh) * f).coerceAtLeast(1f)
                cy = sy + (ty - sy) * f
                lp.width = px(cw); lp.height = px(ch); lp.y = topInset + px(cy)
                runCatching { wm.updateViewLayout(v, lp) }
                v.invalidate()
            }
            start()
        }
    }
}

@SuppressLint("ViewConstructor")
class PillView(ctx: Context, private val island: Island) : View(ctx) {
    private val d = resources.displayMetrics.density
    private val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xF51E1E1E.toInt() }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 2.5f * d; strokeCap = Paint.Cap.ROUND; strokeJoin = Paint.Join.ROUND; color = Island.BLUE
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val glyph = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 2.2f * d; strokeCap = Paint.Cap.ROUND; strokeJoin = Paint.Join.ROUND; color = Color.WHITE
    }
    private val title = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE; textSize = 14.5f * d; typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }
    private val sub = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFB4B4B4.toInt(); textSize = 12.5f * d }
    private val btnText = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE; textSize = 13.5f * d; typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL); textAlign = Paint.Align.CENTER
    }
    private val rect = RectF()
    private val oval = RectF()
    private val path = Path()
    private val ringPath = Path()
    private val ringPart = Path()
    private val measure = PathMeasure()
    private var ticking = false
    private var pressed = 0
    private var downX = 0f
    private var downY = 0f
    private var mode = 0  // 0 a tap so far, 1 sliding sideways, 2 flicked up
    private var velocity: VelocityTracker? = null
    private var fadeAt = 0L

    /** Content changed: fade it in over the next moment. */
    fun fadeIn() { fadeAt = SystemClock.uptimeMillis(); postInvalidateOnAnimation() }

    private fun layout(text: String, bold: Boolean, widthPx: Int, maxLines: Int): StaticLayout =
        StaticLayout.Builder.obtain(text, 0, text.length, if (bold) title else sub, widthPx.coerceAtLeast(1))
            .setAlignment(Layout.Alignment.ALIGN_NORMAL).setMaxLines(maxLines).setEllipsize(TextUtils.TruncateAt.END).build()

    /** Height in dp that [text] takes wrapped at [widthPx] (the title font if [bold], else the sub's). */
    fun textHeightDp(text: String, bold: Boolean, widthPx: Int, maxLines: Int): Float =
        if (text.isEmpty()) 0f else layout(text, bold, widthPx, maxLines).height / d

    fun setProgressTicking(on: Boolean) {
        ticking = on
        if (on) postInvalidateOnAnimation()
    }

    /** A rounded rectangle's outline that starts at its top centre and runs clockwise. */
    private fun outline(p: Path, l: Float, t: Float, r: Float, b: Float, radius: Float) {
        val rr = radius.coerceIn(0f, min(r - l, b - t) / 2)
        val cx = (l + r) / 2
        p.reset()
        p.moveTo(cx, t)
        p.lineTo(r - rr, t)
        oval.set(r - 2 * rr, t, r, t + 2 * rr); p.arcTo(oval, 270f, 90f, false)
        p.lineTo(r, b - rr)
        oval.set(r - 2 * rr, b - 2 * rr, r, b); p.arcTo(oval, 0f, 90f, false)
        p.lineTo(l + rr, b)
        oval.set(l, b - 2 * rr, l + 2 * rr, b); p.arcTo(oval, 90f, 90f, false)
        p.lineTo(l, t + rr)
        oval.set(l, t, l + 2 * rr, t + 2 * rr); p.arcTo(oval, 180f, 90f, false)
        p.lineTo(cx, t)
    }

    override fun onDraw(c: Canvas) {
        val L = island.look
        val w = width.toFloat()
        val h = height.toFloat()
        if (L.idle || h < 6 * d) return
        rect.set(0f, 0f, w, h)
        // Single-row pills have fully round ends; the tall ones get rounded corners.
        val r = if (L.stack || L.expanded) min(h / 2, 30 * d) else h / 2
        c.drawRoundRect(rect, r, r, bg)
        stroke.strokeWidth = if (L.highlight) 2 * d else 1 * d
        stroke.color = if (L.highlight) L.accent else 0x28FFFFFF
        rect.inset(stroke.strokeWidth / 2, stroke.strokeWidth / 2)
        c.drawRoundRect(rect, r, r, stroke)

        val a = ((h - 30 * d) / (L.h * d - 30 * d)).coerceIn(0f, 1f) * ((w - L.w * d * 0.6f) / (L.w * d * 0.4f)).coerceIn(0f, 1f)
        if (a < 0.05f) return
        val fade = ((SystemClock.uptimeMillis() - fadeAt) / 160f).coerceIn(0f, 1f)
        if (fade < 1f) postInvalidateOnAnimation()
        val alpha = (a * fade * 255).toInt()
        if (alpha < 13) return
        if (L.ringTo > L.ringFrom) {
            val now = SystemClock.uptimeMillis()
            val frac = ((now - L.ringFrom).toFloat() / (L.ringTo - L.ringFrom)).coerceIn(0f, 1f)
            val ins = 3 * d
            outline(ringPath, ins, ins, w - ins, h - ins, r - ins)
            measure.setPath(ringPath, false)
            ringPart.reset()
            if (frac > 0.002f && measure.getSegment(0f, measure.length * frac, ringPart, true)) {
                ring.alpha = alpha
                c.drawPath(ringPart, ring)
            }
        }
        val pad = 12 * d
        if (L.stack) { drawStacked(c, L, w, alpha, pad); return }
        if (L.expanded) { drawExpanded(c, L, w, h, alpha, pad); if (ticking) postInvalidateOnAnimation(); return }
        val cy = h / 2

        // icon bubble
        val ir = 17 * d
        fill.color = L.accent; fill.alpha = alpha
        c.drawCircle(pad + ir, cy, ir, fill)
        glyph.alpha = alpha
        drawGlyph(c, L.glyph, pad + ir, cy, 7 * d)

        // buttons
        var right = w - pad
        for (b in L.buttons.asReversed()) {
            val bw = if (b.glyph != 0) 38 * d else btnText.measureText(b.label) + 30 * d
            val bh = if (b.glyph != 0) 38 * d else 34 * d
            b.rect.set(right - bw, cy - bh / 2, right, cy + bh / 2)
            fill.color = if (pressed == b.id) lighten(b.fill) else b.fill; fill.alpha = alpha
            c.drawRoundRect(b.rect, bh / 2, bh / 2, fill)
            if (b.glyph != 0) {
                glyph.alpha = alpha
                drawGlyph(c, b.glyph, b.rect.centerX(), b.rect.centerY(), 7 * d)
            } else {
                btnText.alpha = alpha
                c.drawText(b.label, b.rect.centerX(), b.rect.centerY() - (btnText.descent() + btnText.ascent()) / 2, btnText)
            }
            right -= bw + 8 * d
        }

        val tx = pad + ir * 2 + 10 * d
        val tw = (right - tx - 4 * d).coerceAtLeast(10f)
        title.alpha = alpha
        sub.alpha = alpha
        if (L.progress) {
            val done = svc().xferDone
            val total = svc().xferTotal
            val frac = if (total > 0) (done.toFloat() / total).coerceIn(0f, 1f) else 1f
            val pct = "${(frac * 100).toInt()}%"
            val pw = sub.measureText(pct) + 6 * d
            c.drawText(ellipsize(L.title, title, tw - pw), tx, cy - 6 * d, title)
            c.drawText(pct, tx + tw - pw + 6 * d, cy - 6 * d, sub)
            val by = cy + 4 * d
            val bh = 5 * d
            fill.color = 0x40FFFFFF; fill.alpha = (0x40 * a).toInt()
            rect.set(tx, by, tx + tw, by + bh)
            c.drawRoundRect(rect, bh / 2, bh / 2, fill)
            fill.color = L.accent; fill.alpha = alpha
            rect.set(tx, by, tx + maxOf(bh, tw * frac), by + bh)
            c.drawRoundRect(rect, bh / 2, bh / 2, fill)
            sub.textSize = 11 * d
            c.drawText("${humanSize(done)} of ${humanSize(total)}", tx, by + bh + 13 * d, sub)
            sub.textSize = 12.5f * d
        } else if (L.sub.isEmpty()) {
            c.drawText(ellipsize(L.title, title, tw), tx, cy - (title.descent() + title.ascent()) / 2, title)
        } else {
            c.drawText(ellipsize(L.title, title, tw), tx, cy - 3 * d, title)
            c.drawText(ellipsize(L.sub, sub, tw), tx, cy + 15 * d, sub)
        }
        if (ticking) postInvalidateOnAnimation()
    }

    /** Opened: the whole title and sub wrapped, labelled buttons along the bottom. */
    private fun drawExpanded(c: Canvas, L: Island.Look, w: Float, h: Float, alpha: Int, pad: Float) {
        val ir = 17 * d
        val top = 16 * d
        fill.color = L.accent; fill.alpha = alpha
        c.drawCircle(pad + ir, top + ir, ir, fill)
        glyph.alpha = alpha
        drawGlyph(c, L.glyph, pad + ir, top + ir, 7 * d)
        val tx = pad + ir * 2 + 10 * d
        val tw = (w - tx - pad).toInt().coerceAtLeast(1)
        title.alpha = alpha
        sub.alpha = alpha
        val tl = layout(L.title, true, tw, 6)
        c.save()
        c.translate(tx, top)
        tl.draw(c)
        if (L.sub.isNotEmpty()) {
            c.translate(0f, tl.height + 4 * d)
            layout(L.sub, false, tw, 10).draw(c)
        }
        c.restore()
        // Buttons right-aligned along the bottom; the offer's four in rows of two, the last pair lowest.
        var right = w - pad
        val bh = 36 * d
        var by = h - 14 * d - bh
        for ((n, b) in L.buttons.asReversed().withIndex()) {
            if (L.pairs && n > 0 && n % 2 == 0) { right = w - pad; by -= bh + 8 * d }
            val bw = btnText.measureText(b.label) + 40 * d
            b.rect.set(right - bw, by, right, by + bh)
            fill.color = if (pressed == b.id) lighten(b.fill) else b.fill; fill.alpha = alpha
            c.drawRoundRect(b.rect, bh / 2, bh / 2, fill)
            btnText.alpha = alpha
            c.drawText(b.label, b.rect.centerX(), b.rect.centerY() - (btnText.descent() + btnText.ascent()) / 2, btnText)
            right -= bw + 8 * d
        }
    }

    /** Icon and title on top, then one full-width row per button. */
    private fun drawStacked(c: Canvas, L: Island.Look, w: Float, alpha: Int, pad: Float) {
        val ir = 17 * d
        val cy = 32 * d
        fill.color = L.accent; fill.alpha = alpha
        c.drawCircle(pad + ir, cy, ir, fill)
        glyph.alpha = alpha
        drawGlyph(c, L.glyph, pad + ir, cy, 7 * d)
        val tx = pad + ir * 2 + 10 * d
        title.alpha = alpha
        sub.alpha = alpha
        c.drawText(ellipsize(L.title, title, w - tx - pad), tx, cy - 3 * d, title)
        c.drawText(ellipsize(L.sub, sub, w - tx - pad), tx, cy + 15 * d, sub)
        var top = 60 * d
        val bh = 40 * d
        for (b in L.buttons) {
            b.rect.set(pad, top, w - pad, top + bh)
            fill.color = if (pressed == b.id) lighten(b.fill) else b.fill; fill.alpha = alpha
            c.drawRoundRect(b.rect, bh / 2, bh / 2, fill)
            btnText.alpha = alpha
            c.drawText(ellipsize(b.label, btnText, b.rect.width() - 24 * d), b.rect.centerX(),
                b.rect.centerY() - (btnText.descent() + btnText.ascent()) / 2, btnText)
            top += bh + 8 * d
        }
    }

    private fun svc() = WinDropService.instance ?: error("service gone")

    private fun ellipsize(s: String, p: TextPaint, w: Float) = TextUtils.ellipsize(s, p, w, TextUtils.TruncateAt.END).toString()

    private fun lighten(c: Int) = Color.rgb(
        (Color.red(c) + 255) / 2 - 40, (Color.green(c) + 255) / 2 - 40, (Color.blue(c) + 255) / 2 - 40,
    )

    private fun drawGlyph(c: Canvas, g: Int, x: Float, y: Float, s: Float) {
        path.reset()
        when (g) {
            Island.G_UP -> { path.moveTo(x, y + s); path.lineTo(x, y - s); path.moveTo(x - s * 0.7f, y - s * 0.3f); path.lineTo(x, y - s); path.lineTo(x + s * 0.7f, y - s * 0.3f) }
            Island.G_DOWN -> { path.moveTo(x, y - s); path.lineTo(x, y + s); path.moveTo(x - s * 0.7f, y + s * 0.3f); path.lineTo(x, y + s); path.lineTo(x + s * 0.7f, y + s * 0.3f) }
            Island.G_CHECK -> { path.moveTo(x - s * 0.8f, y); path.lineTo(x - s * 0.2f, y + s * 0.6f); path.lineTo(x + s * 0.9f, y - s * 0.6f) }
            Island.G_WARN -> { path.moveTo(x, y - s); path.lineTo(x, y + s * 0.3f); path.moveTo(x, y + s * 0.9f); path.lineTo(x, y + s * 0.95f) }
            Island.G_X -> { path.moveTo(x - s * 0.7f, y - s * 0.7f); path.lineTo(x + s * 0.7f, y + s * 0.7f); path.moveTo(x + s * 0.7f, y - s * 0.7f); path.lineTo(x - s * 0.7f, y + s * 0.7f) }
            Island.G_LINK -> { rect.set(x - s, y - s * 0.45f, x + s, y + s * 0.45f); path.addRoundRect(rect, s * 0.45f, s * 0.45f, Path.Direction.CW) }
        }
        c.drawPath(path, glyph)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val hit = island.look.buttons.firstOrNull { it.rect.contains(e.x, e.y) }?.id ?: 0
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                pressed = hit; downX = e.rawX; downY = e.rawY; mode = 0
                velocity?.recycle()
                velocity = VelocityTracker.obtain().also { it.addMovement(e) }
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                velocity?.addMovement(e)
                val dx = e.rawX - downX
                val dy = e.rawY - downY
                if (mode == 0 && abs(dx) > 10 * d && abs(dx) > abs(dy)) {
                    mode = 1  // sideways: the pill follows the finger
                    pressed = 0
                    invalidate()
                } else if (mode == 0 && -dy > 12 * d && -dy > abs(dx)) {
                    // Flick up: out of the way (nothing is cancelled).
                    mode = 2
                    pressed = 0
                    performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                    island.dismiss()
                }
                if (mode == 1) island.dragX(dx)
            }
            MotionEvent.ACTION_UP -> {
                val m = mode
                mode = 0
                if (m == 2) return true
                if (m == 1) {
                    val vt = velocity
                    vt?.addMovement(e)
                    vt?.computeCurrentVelocity(1000)
                    island.releaseX(e.rawX - downX, vt?.xVelocity ?: 0f)
                    return true
                }
                val p = pressed
                pressed = 0
                invalidate()
                if (p != 0 && p == hit) {
                    performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                    island.onButton(p)
                } else if (p == 0) {
                    island.onBodyClick()
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                if (mode == 1) island.releaseX(0f, 0f)
                mode = 0
                pressed = 0
                invalidate()
            }
            MotionEvent.ACTION_OUTSIDE -> { pressed = 0; invalidate(); island.outside() }
        }
        return true
    }
}

fun humanSize(b: Long): String {
    if (b < 1024) return "$b B"
    val units = arrayOf("KB", "MB", "GB", "TB")
    var v = b / 1024.0
    var u = 0
    while (v >= 1024 && u < units.size - 1) { v /= 1024; u++ }
    return String.format("%.1f %s", v, units[u])
}

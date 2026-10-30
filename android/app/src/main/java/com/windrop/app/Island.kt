package com.windrop.app

import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.text.TextPaint
import android.text.TextUtils
import android.view.DragEvent
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.view.animation.DecelerateInterpolator
import android.view.animation.OvershootInterpolator
import kotlin.math.min

/**
 * The top-edge "dynamic island" overlay. Main thread only.
 * Idle it is a 2dp, untouchable strip (still receives global DRAG_STARTED); it grows when something happens.
 */
class Island(private val svc: WinDropService) {
    companion object {
        const val BTN_ACCEPT = 1
        const val BTN_DECLINE = 2
        const val BTN_CANCEL = 3

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

    class Btn(val id: Int, val label: String, val fill: Int) {
        val rect = RectF()
    }

    data class Look(
        val w: Float = 180f, val h: Float = 2f, val y: Float = 0f, val idle: Boolean = true,
        val title: String = "", val sub: String = "", val glyph: Int = 0, val accent: Int = BLUE,
        val progress: Boolean = false, val highlight: Boolean = false, val clickable: Boolean = false,
        val buttons: List<Btn> = emptyList(),
    )

    class Msg(
        val title: String, val sub: String = "", val glyph: Int = 0, val accent: Int = BLUE,
        val buttons: List<Btn> = emptyList(), val onClick: (() -> Unit)? = null,
    )

    private class OfferUi(val id: Long, val name: String, val size: Long, val sender: String)
    private class XferUi(val sending: Boolean, val name: String, val fromOutbox: Boolean)

    private val wm = svc.getSystemService(WindowManager::class.java)
    private val density = svc.resources.displayMetrics.density
    private val main = Handler(Looper.getMainLooper())
    private var view: PillView? = null
    private lateinit var lp: WindowManager.LayoutParams

    private var drag = 0
    private var offer: OfferUi? = null
    private var xfer: XferUi? = null
    private var msg: Msg? = null
    private val outboxBatch = mutableListOf<Long>()

    var look = Look(); private set
    private var cw = 180f
    private var ch = 2f
    private var cy = 0f
    private var anim: ValueAnimator? = null
    private val expire = Runnable { msg = null; outboxBatch.clear(); refresh() }
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
        main.removeCallbacks(expire)
        view?.let { runCatching { wm.removeViewImmediate(it) } }
        view = null
    }

    private fun idleFlags() = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
        WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
        WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS

    private fun activeFlags() = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
        WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
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
                svc.handleDrop(e)
                showMessage(Msg("Sending to ${svc.prefs.pcLabel}…", "Connecting", G_UP), 1500)
                return true
            }
            DragEvent.ACTION_DRAG_ENDED -> if (drag != 0) { drag = 0; refresh() }
        }
        return true
    }

    // ---- events from the service ----
    fun showOffer(id: Long, name: String, size: Long, sender: String) {
        offer = OfferUi(id, name, size, sender); offerHidden = false; refresh()
    }

    fun closeOffer(id: Long) {
        if (offer?.id == id) { offer = null; refresh() }
    }

    fun transferStarted(sending: Boolean, name: String, fromOutbox: Boolean) {
        xfer = XferUi(sending, name, fromOutbox); msg = null
        // Show progress for a moment, then step aside; the notification keeps the progress bar.
        xferHidden = false
        main.removeCallbacks(hideXfer)
        main.postDelayed(hideXfer, PILL_MS)
        refresh()
    }

    fun transferEnded() { xfer = null; main.removeCallbacks(hideXfer); refresh() }

    /** Swiped up: get out of the way now. A pending request moves to the notification. */
    fun dismiss() {
        main.removeCallbacks(expire)
        msg = null
        outboxBatch.clear()
        if (xfer != null) xferHidden = true
        offer?.let { o ->
            if (!offerHidden) { offerHidden = true; svc.offerToNotification(o.id, o.name, o.size, o.sender) }
        }
        refresh()
    }

    private var connectingMsg: Msg? = null
    val isConnecting get() = connectingMsg != null && msg === connectingMsg

    /** The on-demand link is coming up (Android may ask about Wi-Fi meanwhile). */
    fun connecting(pc: String, incoming: Boolean) {
        val m = Msg(
            "Connecting to $pc\u2026",
            if (incoming) "$pc has something for you" else "Sends as soon as it connects",
            if (incoming) G_DOWN else G_UP, BLUE,
        )
        connectingMsg = m
        showMessage(m, PILL_MS)
    }

    fun connectingDone() {
        if (isConnecting) { msg = null; refresh() }
        connectingMsg = null
    }

    fun offline(outboxId: Long, count: Int) {
        outboxBatch += outboxId
        showMessage(
            Msg("${svc.prefs.pcLabel} offline — saved to Outbox ($count)", "Sends automatically when it's back", G_WARN, AMBER,
                listOf(Btn(BTN_CANCEL, "Cancel", GRAY_BTN))),
            4000, keepBatch = true,
        )
    }

    fun showMessage(m: Msg, ms: Long, keepBatch: Boolean = false) {
        if (!keepBatch) outboxBatch.clear()
        msg = m
        main.removeCallbacks(expire)
        main.postDelayed(expire, minOf(ms, PILL_MS))
        refresh()
    }

    fun onButton(id: Int) {
        when (id) {
            BTN_ACCEPT, BTN_DECLINE -> offer?.let { svc.decide(it.id, id == BTN_ACCEPT); offer = null; refresh() }
            BTN_CANCEL -> {
                val ids = outboxBatch.toList()
                outboxBatch.clear()
                svc.cancelOutbox(ids)
                showMessage(Msg(if (ids.size > 1) "Removed ${ids.size} items from Outbox" else "Removed from Outbox", glyph = G_X, accent = GRAY_BTN), 1500)
            }
        }
    }

    fun onBodyClick() {
        val m = msg ?: return
        val click = m.onClick ?: return
        msg = null
        refresh()
        click()
    }

    // ---- layout + animation ----
    private fun refresh() {
        val v = view ?: return
        val screenDp = svc.resources.displayMetrics.widthPixels / density
        val wide = min(screenDp - 24f, 380f)
        val o = offer
        val x = xfer
        val m = msg
        look = when {
            drag != 0 -> Look(
                if (drag == 2) 256f else 240f, if (drag == 2) 60f else 56f, 8f, false,
                if (drag == 2) "Release to send" else "Drop to send to PC", svc.prefs.pcLabel, G_UP, BLUE,
                highlight = drag == 2,
            )
            o != null && !offerHidden -> Look(
                wide, 72f, 8f, false, "PC sending ${o.name}", "${humanSize(o.size)} · from ${o.sender}", G_DOWN, BLUE,
                buttons = listOf(Btn(BTN_DECLINE, "Decline", GRAY_BTN), Btn(BTN_ACCEPT, "Accept", BLUE)),
            )
            x != null && !xferHidden -> Look(
                min(wide, 320f), 60f, 8f, false,
                if (!x.sending) "Receiving ${x.name}" else if (x.fromOutbox) "${svc.prefs.pcLabel} is back — sending ${x.name}" else "Sending ${x.name}",
                "", if (x.sending) G_UP else G_DOWN, BLUE, progress = true,
            )
            m != null -> Look(
                if (m.buttons.isEmpty()) min(wide, 320f) else wide, 60f, 8f, false, m.title, m.sub, m.glyph, m.accent,
                clickable = m.onClick != null, buttons = m.buttons,
            )
            else -> Look()
        }
        lp.flags = if (look.idle) idleFlags() else activeFlags()
        animateTo(look.w, look.h, look.y, v)
        v.setProgressTicking(look.progress)
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
    private val path = Path()
    private var ticking = false
    private var pressed = 0
    private var downY = 0f
    private var swiped = false

    fun setProgressTicking(on: Boolean) {
        ticking = on
        if (on) postInvalidateOnAnimation()
    }

    override fun onDraw(c: Canvas) {
        val L = island.look
        val w = width.toFloat()
        val h = height.toFloat()
        if (L.idle || h < 6 * d) return
        rect.set(0f, 0f, w, h)
        val r = min(h / 2, 30 * d)
        c.drawRoundRect(rect, r, r, bg)
        stroke.strokeWidth = if (L.highlight) 2 * d else 1 * d
        stroke.color = if (L.highlight) L.accent else 0x28FFFFFF
        rect.inset(stroke.strokeWidth / 2, stroke.strokeWidth / 2)
        c.drawRoundRect(rect, r, r, stroke)

        val a = ((h - 30 * d) / (L.h * d - 30 * d)).coerceIn(0f, 1f) * ((w - L.w * d * 0.6f) / (L.w * d * 0.4f)).coerceIn(0f, 1f)
        if (a < 0.05f) return
        val alpha = (a * 255).toInt()
        val pad = 12 * d
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
            val bw = btnText.measureText(b.label) + 30 * d
            val bh = 34 * d
            b.rect.set(right - bw, cy - bh / 2, right, cy + bh / 2)
            fill.color = if (pressed == b.id) lighten(b.fill) else b.fill; fill.alpha = alpha
            c.drawRoundRect(b.rect, bh / 2, bh / 2, fill)
            btnText.alpha = alpha
            c.drawText(b.label, b.rect.centerX(), b.rect.centerY() - (btnText.descent() + btnText.ascent()) / 2, btnText)
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
            MotionEvent.ACTION_DOWN -> { pressed = hit; downY = e.rawY; swiped = false; invalidate() }
            MotionEvent.ACTION_MOVE -> if (!swiped && downY - e.rawY > 12 * d) {
                // Flick up: away it goes.
                swiped = true
                pressed = 0
                performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                island.dismiss()
            }
            MotionEvent.ACTION_UP -> {
                if (swiped) return true
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
            MotionEvent.ACTION_CANCEL, MotionEvent.ACTION_OUTSIDE -> { pressed = 0; invalidate() }
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

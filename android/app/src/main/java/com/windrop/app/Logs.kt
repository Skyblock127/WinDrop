package com.windrop.app

import android.util.Log
import java.text.SimpleDateFormat
import java.util.ArrayDeque
import java.util.Date
import java.util.Locale

/**
 * Logcat plus an in-memory ring of recent lines. The ring is shipped to the PC whenever a link is up
 * (offer type 3), so both sides' history ends up in the PC's windrop.log.
 */
object Logs {
    private const val TAG = "WinDrop"
    private const val MAX_LINES = 3000
    private val ring = ArrayDeque<String>()
    private val fmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)

    fun d(msg: String) { Log.d(TAG, msg); keep("D", msg) }
    fun i(msg: String) { Log.i(TAG, msg); keep("I", msg) }
    fun w(msg: String, t: Throwable? = null) { Log.w(TAG, msg, t); keep("W", msg + (t?.let { " — $it" } ?: "")) }
    fun e(msg: String, t: Throwable? = null) { Log.e(TAG, msg, t); keep("E", msg + (t?.let { " — $it" } ?: "")) }

    private fun keep(level: String, msg: String) = synchronized(ring) {
        ring.addLast("${fmt.format(Date())} $level $msg")
        while (ring.size > MAX_LINES) ring.removeFirst()
    }

    /** Takes everything buffered so far (empty if nothing new). */
    fun drain(): String = synchronized(ring) {
        val s = ring.joinToString("\n")
        ring.clear()
        s
    }

    /** Puts lines back after a failed upload, ahead of anything logged since. */
    fun restore(text: String) = synchronized(ring) {
        if (text.isEmpty()) return
        text.split('\n').asReversed().forEach { ring.addFirst(it) }
        while (ring.size > MAX_LINES) ring.removeFirst()
    }
}

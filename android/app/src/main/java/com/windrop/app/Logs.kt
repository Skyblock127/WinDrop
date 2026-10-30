package com.windrop.app

import android.content.Context
import android.util.Log
import java.io.File
import java.text.SimpleDateFormat
import java.util.ArrayDeque
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

/**
 * Logcat, a log file on the phone (for Save logs, about 2 MB kept), and an in-memory ring of recent
 * lines. The ring is shipped to the PC whenever a link is up (offer type 3), so both sides' history
 * also ends up in the PC's windrop.log.
 */
object Logs {
    private const val TAG = "WinDrop"
    private const val MAX_LINES = 3000
    private const val FILE_MAX = 1L shl 20  // per file; the previous one is kept as .1
    private val ring = ArrayDeque<String>()
    private val fmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)
    private val disk = Executors.newSingleThreadExecutor()
    @Volatile private var file: File? = null

    /** Starts writing to the log file; safe to call more than once. */
    fun init(ctx: Context) {
        if (file != null) return
        file = File(ctx.applicationContext.filesDir, "logs").apply { mkdirs() }.let { File(it, "windrop.log") }
    }

    fun d(msg: String) { Log.d(TAG, msg); keep("D", msg) }
    fun i(msg: String) { Log.i(TAG, msg); keep("I", msg) }
    fun w(msg: String, t: Throwable? = null) { Log.w(TAG, msg, t); keep("W", msg + (t?.let { " — $it" } ?: "")) }
    fun e(msg: String, t: Throwable? = null) { Log.e(TAG, msg, t); keep("E", msg + (t?.let { " — $it" } ?: "")) }

    private fun keep(level: String, msg: String) {
        val line = "${fmt.format(Date())} $level $msg"
        synchronized(ring) {
            ring.addLast(line)
            while (ring.size > MAX_LINES) ring.removeFirst()
        }
        val f = file ?: return
        disk.execute {
            runCatching {
                if (f.length() > FILE_MAX) f.renameTo(File(f.path + ".1"))
                f.appendText(line + "\n")
            }
        }
    }

    /** Everything in the log files, oldest first. */
    fun fileText(): String {
        val f = file ?: return ""
        disk.submit {}.get()  // let pending writes land first
        return listOf(File(f.path + ".1"), f).filter { it.exists() }.joinToString("") { it.readText() }
    }

    /** Deletes the log files (and the unsent ring), for a fresh log of one attempt. */
    fun clear() {
        synchronized(ring) { ring.clear() }
        val f = file ?: return
        disk.submit { f.delete(); File(f.path + ".1").delete() }.get()
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

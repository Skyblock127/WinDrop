package com.windrop.app

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.util.concurrent.CopyOnWriteArrayList

/**
 * Items that couldn't be delivered because the PC was unreachable.
 * Files are copied into app storage (content:// grants don't survive), the index lives in SharedPreferences.
 */
class OutboxManager(context: Context) {
    data class Entry(val id: Long, val name: String, val isText: Boolean, val text: String?, val path: String?, val size: Long)

    private val sp = context.getSharedPreferences("outbox", Context.MODE_PRIVATE)
    private val dir = File(context.filesDir, "outbox").apply { mkdirs() }

    // The service and the app screen each make their own OutboxManager over the same storage, so
    // the lock and the change listeners are shared: a send the service finishes updates the screen.
    val listeners get() = sharedListeners

    companion object {
        private val sharedListeners = CopyOnWriteArrayList<() -> Unit>()
        private val lock = Any()
    }

    fun list(): List<Entry> = synchronized(lock) {
        val arr = JSONArray(sp.getString("items", "[]"))
        (0 until arr.length()).map { i ->
            val o = arr.getJSONObject(i)
            Entry(
                o.getLong("id"), o.getString("name"), o.optBoolean("text"),
                o.optString("body").takeIf { o.has("body") }, o.optString("path").takeIf { o.has("path") },
                o.optLong("size"),
            )
        }
    }

    val count: Int get() = list().size

    private fun save(items: List<Entry>) {
        val arr = JSONArray()
        for (e in items) {
            arr.put(JSONObject().apply {
                put("id", e.id); put("name", e.name); put("text", e.isText); put("size", e.size)
                e.text?.let { put("body", it) }
                e.path?.let { put("path", it) }
            })
        }
        synchronized(lock) { sp.edit().putString("items", arr.toString()).apply() }
        sharedListeners.forEach { it() }
    }

    private fun nextId() = synchronized(lock) {
        val id = sp.getLong("next_id", 1)
        sp.edit().putLong("next_id", id + 1).apply()
        id
    }

    fun addText(text: String): Entry {
        val e = Entry(nextId(), "Text", true, text, null, text.toByteArray().size.toLong())
        synchronized(lock) { save(list() + e) }
        return e
    }

    /** Copies [input] into app storage; slow for big files, so call off the main thread. */
    fun addFile(name: String, input: InputStream): Entry {
        val id = nextId()
        val f = File(dir, "${id}_" + name.replace(Regex("[/\\\\]"), "_"))
        input.use { src -> f.outputStream().use { src.copyTo(it, Proto.CHUNK) } }
        val e = Entry(id, name, false, null, f.absolutePath, f.length())
        synchronized(lock) { save(list() + e) }
        return e
    }

    fun remove(id: Long) = synchronized(lock) {
        val items = list()
        items.find { it.id == id }?.path?.let { File(it).delete() }
        save(items.filter { it.id != id })
    }

    fun clear() = synchronized(lock) {
        list().forEach { e -> e.path?.let { File(it).delete() } }
        save(emptyList())
    }
}

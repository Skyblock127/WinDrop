package com.windrop.app

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.Settings
import android.view.DragEvent
import android.widget.Toast

@Suppress("DEPRECATION")
private inline fun <reified T : android.os.Parcelable> Intent.parcelable(key: String): T? = getParcelableExtra(key) as? T

@Suppress("DEPRECATION")
private inline fun <reified T : android.os.Parcelable> Intent.parcelableList(key: String): List<T>? =
    getParcelableArrayListExtra<android.os.Parcelable>(key)?.filterIsInstance<T>()

/** Share-sheet target: grabs the streams and hands them to the service, then vanishes. */
class ShareReceiverActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val items = mutableListOf<OutgoingItem>()
        val uris = LinkedHashSet<Uri>()
        when (intent.action) {
            Intent.ACTION_SEND -> intent.parcelable<Uri>(Intent.EXTRA_STREAM)?.let { uris += it }
            Intent.ACTION_SEND_MULTIPLE -> intent.parcelableList<Uri>(Intent.EXTRA_STREAM)?.let { uris += it }
        }
        intent.clipData?.let { c -> for (i in 0 until c.itemCount) c.getItemAt(i).uri?.let { uris += it } }
        for (u in uris) runCatching { OutgoingItem.fromUri(this, u) }.getOrNull()?.let { items += it }
        if (uris.isEmpty()) {
            val text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT)?.toString()?.trim().orEmpty()
            if (text.isNotEmpty()) items += OutgoingItem.text(text)
        }
        val prefs = Prefs(this)
        when {
            items.isEmpty() -> Toast.makeText(this, "Nothing to send", Toast.LENGTH_SHORT).show()
            !prefs.paired -> {
                Toast.makeText(this, "Pair WinDrop with your PC first", Toast.LENGTH_LONG).show()
                startActivity(Intent(this, MainActivity::class.java))
            }
            else -> {
                // Sharing to WinDrop while it's switched off turns it on: that's clearly what you want.
                val wasOff = !prefs.enabled || WinDropService.instance == null
                if (wasOff) {
                    Logs.i("share while off: turning WinDrop on")
                    WinDropService.start(this)
                }
                WinDropService.enqueue(this, items)
                if (wasOff || !Settings.canDrawOverlays(this))
                    Toast.makeText(this, if (wasOff) "WinDrop on \u00b7 sending to ${prefs.pcLabel}\u2026" else "Sending to ${prefs.pcLabel}\u2026", Toast.LENGTH_SHORT).show()
            }
        }
        finish()
        overridePendingTransition(0, 0)
    }
}

/** Takes the DragAndDropPermissions an overlay window can't take itself, opens the files, and finishes. */
class DropPermissionActivity : Activity() {
    companion object {
        const val EXTRA_EVENT = "event"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val ev = intent.parcelable<DragEvent>(EXTRA_EVENT)
        val perms = ev?.let { runCatching { requestDragAndDropPermissions(it) }.getOrNull() }
        val clip = ev?.clipData
        val items = mutableListOf<OutgoingItem>()
        if (clip != null) {
            for (i in 0 until clip.itemCount) {
                val u = clip.getItemAt(i).uri ?: continue
                runCatching { OutgoingItem.fromUri(this, u) }.getOrNull()?.let { items += it }
            }
        }
        perms?.release()  // the file descriptors we opened stay valid
        if (items.isEmpty()) Toast.makeText(this, "That app didn't allow sharing this file by drag", Toast.LENGTH_LONG).show()
        else WinDropService.enqueue(this, items)
        finish()
        overridePendingTransition(0, 0)
    }
}

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val p = Prefs(context)
        if (p.enabled) runCatching { WinDropService.start(context) }
    }
}

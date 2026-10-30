package com.windrop.app

import android.annotation.SuppressLint
import android.app.Activity
import android.app.PendingIntent
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import android.widget.Toast

/**
 * Sends whatever is on the phone's clipboard to the PC, where it lands on the PC's clipboard.
 *
 * Android only lets the app in front read the clipboard, so this is an invisible activity: it
 * comes to the front for a moment, reads the clipboard once it has focus, queues it, and closes.
 * Started from the "Clipboard → PC" tile, the WinDrop notification and the app screen.
 */
class ClipboardSendActivity : Activity() {
    private var done = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        overridePendingTransition(0, 0)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || done) return
        done = true
        send()
        finish()
        overridePendingTransition(0, 0)
    }

    private fun send() {
        val prefs = Prefs(this)
        if (!prefs.paired) {
            Toast.makeText(this, "Pair WinDrop with your PC first", Toast.LENGTH_LONG).show()
            startActivity(Intent(this, MainActivity::class.java))
            return
        }
        val clip = getSystemService(ClipboardManager::class.java).primaryClip?.takeIf { it.itemCount > 0 }
        val first = clip?.getItemAt(0)
        // An image (or file) on the clipboard goes as "Clipboard <date>.<ext>": the PC then puts
        // it on its own clipboard as well as saving it.
        val item = first?.uri?.let { runCatching { OutgoingItem.fromUri(this, it) }.getOrNull() }
            ?.let { it.renamed(clipName(it.name)) }
            ?: first?.coerceToText(this)?.toString()?.trim()?.takeIf { it.isNotEmpty() }?.let { OutgoingItem.text(it) }
        if (item == null) {
            Toast.makeText(this, "Clipboard is empty", Toast.LENGTH_SHORT).show()
            return
        }
        if (!prefs.enabled || WinDropService.instance == null) WinDropService.start(this)
        Logs.i("clipboard -> PC (${if (item.isText) "text" else item.name})")
        WinDropService.enqueue(this, listOf(item))
        Toast.makeText(this, "Clipboard → ${prefs.pcLabel}", Toast.LENGTH_SHORT).show()
    }

    private fun clipName(original: String): String {
        val ext = original.substringAfterLast('.', "png").lowercase().take(5)
        val stamp = java.text.SimpleDateFormat("yyyy-MM-dd HH.mm.ss", java.util.Locale.US).format(java.util.Date())
        return "Clipboard $stamp.$ext"
    }

    companion object {
        fun intent(ctx: Context) = Intent(ctx, ClipboardSendActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION)
    }
}

/** Quick Settings tile: one tap puts the phone's clipboard on the PC's clipboard. */
class ClipboardTileService : TileService() {
    override fun onStartListening() {
        val t = qsTile ?: return
        t.state = Tile.STATE_INACTIVE
        t.label = "Clipboard → PC"
        t.subtitle = Prefs(this).pcLabel
        t.updateTile()
    }

    @SuppressLint("StartActivityAndCollapseDeprecated")
    override fun onClick() {
        val i = ClipboardSendActivity.intent(this)
        if (Build.VERSION.SDK_INT >= 34) {
            startActivityAndCollapse(PendingIntent.getActivity(this, 7, i, PendingIntent.FLAG_IMMUTABLE))
        } else {
            @Suppress("DEPRECATION")
            startActivityAndCollapse(i)
        }
    }
}

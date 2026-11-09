package com.windrop.app

import android.app.Activity
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.widget.Toast

/**
 * Sends whatever is on the phone's clipboard to a PC, where it lands on the PC's clipboard. Like a
 * drop: straight to our PC, or the pill asks which one when nearby PCs take files too (or none is
 * paired).
 *
 * Android only lets the app in front read the clipboard, so this is an invisible activity: it
 * comes to the front for a moment, reads the clipboard once it has focus, queues it, and closes.
 * Started from the WinDrop notification and the app screen.
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
        val wasOff = !prefs.enabled || WinDropService.instance == null
        if (wasOff) WinDropService.start(this)
        Logs.i("clipboard -> PC (${if (item.isText) "text" else item.name})")
        WinDropService.enqueue(this, listOf(item))
        // The pill shows where it goes (or asks); without it, say so here.
        if (!Settings.canDrawOverlays(this) && prefs.paired)
            Toast.makeText(this, "Clipboard → ${prefs.pcLabel}", Toast.LENGTH_SHORT).show()
        else if (wasOff)
            Toast.makeText(this, "WinDrop is on", Toast.LENGTH_SHORT).show()
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

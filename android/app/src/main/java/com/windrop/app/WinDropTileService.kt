package com.windrop.app

import android.annotation.SuppressLint
import android.app.PendingIntent
import android.content.Intent
import android.os.Build
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService

/** The on/off switch: one tap turns WinDrop (service, pill, receiving) fully on or off. Long-press opens the app. */
class WinDropTileService : TileService() {
    private val listener: () -> Unit = { render() }

    override fun onStartListening() {
        WinDropService.listeners += listener
        render()
    }

    override fun onStopListening() {
        WinDropService.listeners -= listener
    }

    override fun onClick() {
        val p = Prefs(this)
        if (!p.paired) {
            // Nothing to talk to yet: open the app to pair.
            launch(Intent(this, MainActivity::class.java))
            return
        }
        if (WinDropService.instance != null && p.enabled) WinDropService.stop(this)
        else WinDropService.start(this)
        render(p.enabled)
    }

    @SuppressLint("StartActivityAndCollapseDeprecated")
    private fun launch(i: Intent) {
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (Build.VERSION.SDK_INT >= 34) {
            startActivityAndCollapse(PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE))
        } else {
            @Suppress("DEPRECATION")
            startActivityAndCollapse(i)
        }
    }

    private fun render(on: Boolean = WinDropService.instance != null && Prefs(this).enabled) {
        val t = qsTile ?: return
        t.state = if (on) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
        t.label = "WinDrop"
        t.subtitle = if (on) Prefs(this).pcLabel else "Off"
        t.icon = android.graphics.drawable.Icon.createWithResource(this, R.drawable.ic_windrop_glyph)
        t.updateTile()
    }
}

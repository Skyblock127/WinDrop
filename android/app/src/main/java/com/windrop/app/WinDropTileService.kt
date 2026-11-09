package com.windrop.app

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

    // Paired or not: unpaired, WinDrop still sends to and takes files from nearby PCs.
    override fun onClick() {
        val p = Prefs(this)
        if (WinDropService.instance != null && p.enabled) WinDropService.stop(this)
        else WinDropService.start(this)
        render(p.enabled)
    }

    private fun render(on: Boolean = WinDropService.instance != null && Prefs(this).enabled) {
        val t = qsTile ?: return
        t.state = if (on) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
        t.label = "WinDrop"
        t.subtitle = if (!on) "Off" else Prefs(this).let { if (it.paired) it.pcLabel else "On" }
        t.icon = android.graphics.drawable.Icon.createWithResource(this, R.drawable.ic_tile)
        t.updateTile()
    }
}

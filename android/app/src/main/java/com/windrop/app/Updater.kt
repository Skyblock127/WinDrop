package com.windrop.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.provider.Settings
import android.widget.Toast
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * In-app updates from the project's GitHub releases. The latest release carries WinDrop.apk; it's
 * downloaded over HTTPS, checked against the SHA-256 GitHub publishes for it, and handed to the
 * system installer, which shows its own confirmation. Android only accepts it if it's signed with
 * the same key as the installed app.
 */
object Updater {
    private const val LATEST = "https://api.github.com/repos/Skyblock127/WinDrop/releases/latest"
    private const val ASSET = "WinDrop.apk"
    private const val CH_UPDATES = "updates"
    private const val NOTIF_UPDATE = 40
    private const val CHECK_EVERY_MS = 24 * 60 * 60 * 1000L

    class Release(val version: String, val url: String, val sha256: String?)

    /** Newest release found by the last check, if it's newer than this app. */
    @Volatile var available: Release? = null
        private set

    /** a > b for dotted numeric versions. */
    fun newer(a: String, b: String): Boolean {
        fun parts(v: String) = v.split('.').map { it.takeWhile(Char::isDigit).toIntOrNull() ?: 0 } + List(4) { 0 }
        val x = parts(a)
        val y = parts(b)
        for (i in 0 until 4) if (x[i] != y[i]) return x[i] > y[i]
        return false
    }

    private fun get(url: String): HttpURLConnection = (URL(url).openConnection() as HttpURLConnection).apply {
        connectTimeout = 10_000
        readTimeout = 20_000
        setRequestProperty("Accept", "application/vnd.github+json")
        setRequestProperty("User-Agent", "WinDrop/${Crypto.appVersion}")
    }

    /**
     * Blocking. Asks GitHub for the latest release; null when this app is already the newest.
     * Throws when the check itself fails (offline).
     */
    fun fetchLatest(): Release? {
        val c = get(LATEST)
        try {
            if (c.responseCode != 200) throw java.io.IOException("GitHub answered ${c.responseCode}")
            val json = JSONObject(c.inputStream.bufferedReader().readText())
            val tag = json.getString("tag_name").removePrefix("v").removePrefix("V")
            if (!newer(tag, Crypto.appVersion)) return null
            val assets = json.getJSONArray("assets")
            for (i in 0 until assets.length()) {
                val a = assets.getJSONObject(i)
                if (a.getString("name") != ASSET) continue
                val url = a.getString("browser_download_url")
                if (!url.startsWith("https://github.com/")) throw java.io.IOException("unexpected download address")
                val digest = a.optString("digest").takeIf { it.startsWith("sha256:") }?.removePrefix("sha256:")
                return Release(tag, url, digest)
            }
            return null
        } finally {
            c.disconnect()
        }
    }

    /**
     * Blocking, for background use: checks at most once a day (unless [force]) and, when [notify],
     * posts a notification about a new version. Never throws.
     */
    fun check(ctx: Context, notify: Boolean, force: Boolean = false) {
        val sp = ctx.getSharedPreferences("windrop", Context.MODE_PRIVATE)
        val now = System.currentTimeMillis()
        if (!force && now - sp.getLong("update_checked", 0) < CHECK_EVERY_MS) {
            if (notify) available?.let { notifyAvailable(ctx, it) }
            return
        }
        val r = runCatching { fetchLatest() }.onFailure { Logs.w("update check failed: ${it.message}") }
        if (r.isFailure) return
        sp.edit().putLong("update_checked", now).apply()
        available = r.getOrNull()
        Logs.i(available?.let { "update: ${it.version} available" } ?: "update: up to date (${Crypto.appVersion})")
        if (notify) available?.let { notifyAvailable(ctx, it) }
    }

    private fun notifyAvailable(ctx: Context, r: Release) {
        val sp = ctx.getSharedPreferences("windrop", Context.MODE_PRIVATE)
        if (sp.getString("update_notified", "") == r.version) return
        sp.edit().putString("update_notified", r.version).apply()
        val nm = ctx.getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CH_UPDATES, "App updates", NotificationManager.IMPORTANCE_DEFAULT))
        val open = PendingIntent.getActivity(
            ctx, 40,
            Intent(ctx, MainActivity::class.java).putExtra(MainActivity.EXTRA_UPDATE, true).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        nm.notify(NOTIF_UPDATE, Notification.Builder(ctx, CH_UPDATES)
            .setSmallIcon(R.drawable.ic_windrop_glyph)
            .setContentTitle("WinDrop ${r.version} is available")
            .setContentText("Tap to update (you have ${Crypto.appVersion})")
            .setContentIntent(open)
            .setAutoCancel(true)
            .build())
    }

    /** True if Android lets this app install updates; otherwise opens the switch for it. */
    fun canInstall(ctx: Context): Boolean {
        if (ctx.packageManager.canRequestPackageInstalls()) return true
        ctx.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${ctx.packageName}"))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        return false
    }

    /** Blocking. Downloads and verifies the APK; [progress] gets 0..100. */
    fun download(ctx: Context, r: Release, progress: (Int) -> Unit): File {
        val dir = File(ctx.cacheDir, "update").apply { mkdirs() }
        dir.listFiles()?.forEach { it.delete() }
        val f = File(dir, "WinDrop-${r.version}.apk")
        val c = get(r.url)  // follows GitHub's redirect to its download host (https to https)
        try {
            if (c.responseCode != 200) throw java.io.IOException("download failed (${c.responseCode})")
            val total = c.contentLengthLong
            val md = MessageDigest.getInstance("SHA-256")
            c.inputStream.use { src ->
                f.outputStream().use { dst ->
                    val buf = ByteArray(64 * 1024)
                    var done = 0L
                    while (true) {
                        val n = src.read(buf)
                        if (n < 0) break
                        dst.write(buf, 0, n)
                        md.update(buf, 0, n)
                        done += n
                        if (total > 0) progress((done * 100 / total).toInt())
                    }
                }
            }
            val hex = md.digest().joinToString("") { "%02x".format(it) }
            if (r.sha256 != null && !hex.equals(r.sha256, ignoreCase = true)) {
                f.delete()
                throw java.io.IOException("the download didn't match its checksum")
            }
            return f
        } finally {
            c.disconnect()
        }
    }

    /** Hands the APK to the system installer; it asks the user to confirm, then replaces the app. */
    fun install(ctx: Context, apk: File) {
        val pi = ctx.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(ctx.packageName)
        }
        val id = pi.createSession(params)
        pi.openSession(id).use { s ->
            apk.inputStream().use { src -> s.openWrite("WinDrop.apk", 0, apk.length()).use { dst -> src.copyTo(dst); s.fsync(dst) } }
            val status = PendingIntent.getBroadcast(
                ctx, id, Intent(ctx, UpdateResultReceiver::class.java),
                PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            s.commit(status.intentSender)
        }
        Logs.i("update: handed ${apk.name} to the installer")
    }
}

/** Status from the system installer: asks the user to confirm, or reports the outcome. */
class UpdateResultReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        when (val st = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                @Suppress("DEPRECATION")
                val confirm = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT) ?: return
                ctx.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            }
            PackageInstaller.STATUS_SUCCESS -> Logs.i("update: installed")
            else -> {
                val msg = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "status $st"
                Logs.w("update: install failed: $msg")
                if (st != PackageInstaller.STATUS_FAILURE_ABORTED) Toast.makeText(ctx, "Update failed: $msg", Toast.LENGTH_LONG).show()
            }
        }
    }
}

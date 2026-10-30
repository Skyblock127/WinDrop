package com.windrop.app

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.File
import java.io.FileInputStream
import java.io.InputStream

/** Something queued for the PC. Streams are opened up front so URI grants can lapse afterwards. */
class OutgoingItem private constructor(
    val name: String,
    val size: Long,
    val text: String?,
    private val pfd: ParcelFileDescriptor?,
    private val file: File?,
    val outboxId: Long?,
) {
    val isText get() = text != null

    fun open(): InputStream = when {
        text != null -> text.toByteArray().inputStream()
        pfd != null -> ParcelFileDescriptor.AutoCloseInputStream(pfd)
        else -> FileInputStream(file!!)
    }

    fun withSpooled(f: File) = OutgoingItem(name, f.length(), null, null, f, outboxId)

    /** Same content under another name (e.g. "Clipboard …" so the PC also copies it). */
    fun renamed(n: String) = OutgoingItem(n, size, text, pfd, file, outboxId)

    fun close() = runCatching { pfd?.close() }

    companion object {
        fun text(t: String) = OutgoingItem("Text", t.toByteArray().size.toLong(), t, null, null, null)

        fun fromOutbox(e: OutboxManager.Entry) =
            if (e.isText) OutgoingItem("Text", e.size, e.text ?: "", null, null, e.id)
            else OutgoingItem(e.name, File(e.path!!).length(), null, null, File(e.path), e.id)

        /** Throws SecurityException if the caller holds no read grant for [uri]. */
        fun fromUri(ctx: Context, uri: Uri): OutgoingItem? {
            var name: String? = null
            var size = -1L
            runCatching {
                ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)
                    ?.use { c ->
                        if (c.moveToFirst()) {
                            name = c.getString(0)
                            if (!c.isNull(1)) size = c.getLong(1)
                        }
                    }
            }.onFailure { if (it is SecurityException) throw it }
            val pfd = ctx.contentResolver.openFileDescriptor(uri, "r") ?: return null
            if (size < 0 && pfd.statSize >= 0) size = pfd.statSize
            val n = name ?: uri.lastPathSegment?.substringAfterLast('/') ?: "file"
            return OutgoingItem(n, size, null, pfd, null, null)
        }
    }
}

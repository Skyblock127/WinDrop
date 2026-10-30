package com.windrop.app

import android.os.SystemClock
import java.io.DataInputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/**
 * Wire protocol shared with the Windows app (all integers big-endian):
 *   OFFER    "TDRP" | ver u8=1 | type u8 (1 file, 2 text) | size u64 | nameLen u16 | senderLen u16 | idLen u16
 *            | name | sender | deviceId           (client -> server)
 *   DECISION u8: 1 accepted, 2 declined, 3 busy/error  (server -> client)
 *   STREAM   `size` raw bytes                       (client -> server)
 *   ACK      u8: 0 ok, 0xFF incomplete              (server -> client)
 */
object Proto {
    const val TCP_PORT = 53318



    /** BLE "PC has something for you" beacon; must match windows/src/WifiDirectHost.cpp. */
    const val BLE_COMPANY_ID = 0xFFFF

    const val TYPE_FILE = 1
    const val TYPE_TEXT = 2
    const val TYPE_LOG = 3  // phone log lines for the PC's windrop.log
    const val TYPE_BYE = 4  // phone is closing the link: PC drops its side at once
    const val ACCEPTED = 1
    const val DECLINED = 2
    const val BUSY = 3
    const val ACK_OK = 0
    const val ACK_BAD = 0xFF
    const val CHUNK = 1 shl 20  // big reads/writes keep the link busy
    const val SOCK_BUF = 4 shl 20
    const val MAX_TEXT = 16L shl 20

    class Offer(val type: Int, val size: Long, val name: String, val sender: String, val deviceId: String)

    fun writeOffer(out: OutputStream, o: Offer) {
        val n = o.name.toByteArray()
        val s = o.sender.toByteArray()
        val id = o.deviceId.toByteArray()
        val b = ByteBuffer.allocate(20 + n.size + s.size + id.size)
        b.put("TDRP".toByteArray()).put(1).put(o.type.toByte()).putLong(o.size)
        b.putShort(n.size.toShort()).putShort(s.size.toShort()).putShort(id.size.toShort())
        b.put(n).put(s).put(id)
        out.write(b.array())
    }

    fun readOffer(input: DataInputStream): Offer? {
        val magic = ByteArray(4)
        input.readFully(magic)
        if (String(magic) != "TDRP" || input.readUnsignedByte() != 1) return null
        val type = input.readUnsignedByte()
        val size = input.readLong()
        val nl = input.readUnsignedShort()
        val sl = input.readUnsignedShort()
        val il = input.readUnsignedShort()
        if (nl > 4096 || sl > 1024 || il > 256 || size < 0) return null
        fun str(len: Int) = ByteArray(len).also { input.readFully(it) }.toString(Charsets.UTF_8)
        return Offer(type, size, str(nl), str(sl), str(il))
    }

}

/** Probes every candidate address in parallel and returns the first socket that connects. */
object PeerResolver {
    class Connection(val socket: Socket, val address: InetSocketAddress)

    fun connect(candidates: List<InetSocketAddress>, timeoutMs: Int = 800): Connection? {
        if (candidates.isEmpty()) return null
        val results = LinkedBlockingQueue<Pair<InetSocketAddress, Socket?>>()
        for (a in candidates) {
            thread(isDaemon = true, name = "probe-$a") {
                val s = Socket()
                try {
                    s.connect(a, timeoutMs)
                    results.put(a to s)
                } catch (e: Exception) {
                    runCatching { s.close() }
                    results.put(a to null)
                }
            }
        }
        var winner: Connection? = null
        var remaining = candidates.size
        val deadline = SystemClock.elapsedRealtime() + timeoutMs + 150
        while (remaining > 0 && winner == null) {
            val left = deadline - SystemClock.elapsedRealtime()
            if (left <= 0) break
            val (addr, sock) = results.poll(left, TimeUnit.MILLISECONDS) ?: break
            remaining--
            if (sock == null) continue
            winner = Connection(sock, addr)
        }
        val chosen = winner
        if (remaining > 0) {
            val left = remaining
            thread(isDaemon = true) {
                repeat(left) { results.poll(2, TimeUnit.SECONDS)?.second?.close() }
            }
        }
        return chosen
    }
}

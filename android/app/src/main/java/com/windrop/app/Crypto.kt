package com.windrop.app

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.InputStream
import java.io.OutputStream
import java.math.BigInteger
import java.net.Socket
import java.nio.ByteBuffer
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.KeyGenerator
import javax.crypto.Mac
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * Pairing keys and encrypted connections. Must match windows/src/Crypto.cpp byte for byte.
 *
 * Pairing (once, while "Pair phone…" is open on the PC): P-256 ECDH over the Wi-Fi Direct link;
 * both screens show a 6-digit check number, the user confirms on the PC. Result: a 32-byte key K.
 * Every connection: both prove they hold K (HMAC challenge-response), then everything is
 * AES-256-GCM records keyed per connection from K and both sides' nonces.
 */
object Crypto {
    /**
     * Version envelope; its shape never changes so any two releases can tell they don't match
     * (same as windows/src/Crypto.h):
     *   C->S "WDRP" | proto u8 | type u8 | verLen u8 | app version | type's body
     *   S->C status u8: 01 go on, 00 refused, FE incompatible + proto u8 | verLen u8 | version.
     * [PROTO] changes only when the bytes on the wire change.
     */
    const val MAGIC = "WDRP"
    const val PROTO = 3
    const val HELLO_SESSION = 1
    const val HELLO_PAIR = 2
    const val STATUS_REFUSED = 0
    const val STATUS_GO = 1
    const val STATUS_INCOMPATIBLE = 0xFE
    const val RECORD_MAX = 1 shl 20
    val appVersion: String = BuildConfig.VERSION_NAME

    fun envelope(type: Int): ByteArray {
        val v = appVersion.toByteArray()
        return MAGIC.toByteArray() + PROTO.toByte() + type.toByte() + v.size.toByte() + v
    }

    /** Server: reads the envelope. Null if it isn't a WinDrop header at all. */
    fun readEnvelope(inp: DataInputStream): Pair<Int, PeerVersion>? {
        val h = ByteArray(7).also { inp.readFully(it) }
        if (String(h, 0, 4) != MAGIC) return null
        val ver = String(ByteArray(h[6].toInt() and 0xFF).also { inp.readFully(it) })
        return (h[5].toInt() and 0xFF) to PeerVersion(h[4].toInt() and 0xFF, ver)
    }

    fun incompatibleReply(): ByteArray {
        val v = appVersion.toByteArray()
        return byteArrayOf(STATUS_INCOMPATIBLE.toByte(), PROTO.toByte(), v.size.toByte()) + v
    }

    /** Client: the server's status byte; throws [Incompatible] when it speaks another protocol. */
    fun readStatus(inp: DataInputStream): Int {
        val st = inp.readUnsignedByte()
        if (st == STATUS_INCOMPATIBLE) {
            val proto = inp.readUnsignedByte()
            val ver = String(ByteArray(inp.readUnsignedByte()).also { inp.readFully(it) })
            throw Incompatible(PeerVersion(proto, ver))
        }
        return st
    }

    private val rng = SecureRandom()

    fun random(n: Int) = ByteArray(n).also { rng.nextBytes(it) }
    fun sha256(vararg parts: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").run { parts.forEach { update(it) }; digest() }

    fun hmac(key: ByteArray, vararg parts: ByteArray): ByteArray = Mac.getInstance("HmacSHA256").run {
        init(SecretKeySpec(key, "HmacSHA256"))
        parts.forEach { update(it) }
        doFinal()
    }

    /** RFC 5869 with SHA-256, up to 32 bytes. */
    fun hkdf(ikm: ByteArray, salt: ByteArray, info: String, len: Int): ByteArray {
        val prk = hmac(if (salt.isEmpty()) ByteArray(32) else salt, ikm)
        return hmac(prk, info.toByteArray(), byteArrayOf(1)).copyOf(len)
    }

    fun same(a: ByteArray, b: ByteArray) = MessageDigest.isEqual(a, b)

    /** Rotating BLE tag: HMAC(K, "WDR2 ble" || window)[0..6]. */
    fun beaconTag(key: ByteArray, window: Long): ByteArray =
        hmac(key, "WDR2 ble".toByteArray(), ByteBuffer.allocate(8).putLong(window).array()).copyOf(6)

    fun beaconWindow(): Long = System.currentTimeMillis() / 1000 / 600

    // ---------------- ECDH P-256 ----------------
    class Ecdh {
        private val kp = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }.generateKeyPair()

        /** 65 bytes: 04 | X | Y */
        val publicKey: ByteArray = (kp.public as ECPublicKey).w.let { byteArrayOf(4) + fixed32(it.affineX) + fixed32(it.affineY) }

        fun agree(peer: ByteArray): ByteArray? = runCatching {
            require(peer.size == 65 && peer[0] == 4.toByte())
            val pt = ECPoint(BigInteger(1, peer.copyOfRange(1, 33)), BigInteger(1, peer.copyOfRange(33, 65)))
            val pub = KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(pt, (kp.public as ECPublicKey).params))
            KeyAgreement.getInstance("ECDH").run { init(kp.private); doPhase(pub, true); generateSecret() }
        }.getOrNull()

        private fun fixed32(v: BigInteger): ByteArray {
            val b = v.toByteArray()
            return when {
                b.size == 32 -> b
                b.size > 32 -> b.copyOfRange(b.size - 32, b.size)
                else -> ByteArray(32 - b.size) + b
            }
        }
    }

    // ---------------- the pairing key at rest ----------------
    private const val WRAP_ALIAS = "windrop_pair_wrap"

    private fun wrapKey(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(WRAP_ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(
                KeyGenParameterSpec.Builder(WRAP_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .build(),
            )
            generateKey()
        }
    }

    /** Encrypts K with a hardware-backed Keystore key; only this app on this phone can unwrap it. */
    fun protect(key: ByteArray): String {
        val c = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, wrapKey()) }
        return Base64.encodeToString(c.iv + c.doFinal(key), Base64.NO_WRAP)
    }

    fun unprotect(stored: String): ByteArray? = runCatching {
        val b = Base64.decode(stored, Base64.NO_WRAP)
        Cipher.getInstance("AES/GCM/NoPadding").run {
            init(Cipher.DECRYPT_MODE, wrapKey(), GCMParameterSpec(128, b.copyOfRange(0, 12)))
            doFinal(b.copyOfRange(12, b.size))
        }
    }.getOrNull()?.takeIf { it.size == 32 }
}

/** The other side's protocol number and app version, as it reported them. */
data class PeerVersion(val proto: Int, val version: String) {
    /** True when the other side is the older one (it needs the update, not us). */
    val older: Boolean get() = proto < Crypto.PROTO
}

/** The PC answered but doesn't know this phone any more (removed there, or paired anew). */
class NotPaired : java.io.IOException("this PC no longer has this phone paired")

/** The other side runs a WinDrop that speaks a different protocol: one of the two needs updating. */
class Incompatible(val peer: PeerVersion) : java.io.IOException("WinDrop ${peer.version} (protocol ${peer.proto}) doesn't match ${Crypto.appVersion} (protocol ${Crypto.PROTO})")

/**
 * An authenticated, encrypted connection: [input]/[output] carry the plain protocol, the socket
 * carries length-prefixed AES-256-GCM records (nonce = direction label + counter).
 */
class SecureChannel private constructor(socket: Socket, rawIn: InputStream, key: ByteArray, nc: ByteArray, ns: ByteArray, client: Boolean) {
    // Reads through the stream the handshake used, so nothing it already buffered is lost.
    private val raw = rawIn as? DataInputStream ?: DataInputStream(rawIn)
    private val rawOut = socket.getOutputStream()
    private val txKey: SecretKeySpec
    private val rxKey: SecretKeySpec
    private val txDir = (if (client) "c2s" else "s2c").toByteArray() + 0
    private val rxDir = (if (client) "s2c" else "c2s").toByteArray() + 0
    private var txSeq = 0L
    private var rxSeq = 0L

    init {
        val salt = nc + ns
        val c2s = Crypto.hkdf(key, salt, "WDR2 c2s", 32)
        val s2c = Crypto.hkdf(key, salt, "WDR2 s2c", 32)
        txKey = SecretKeySpec(if (client) c2s else s2c, "AES")
        rxKey = SecretKeySpec(if (client) s2c else c2s, "AES")
    }

    private fun nonce(dir: ByteArray, seq: Long) = dir + ByteBuffer.allocate(8).putLong(seq).array()

    private fun sendRecord(p: ByteArray, off: Int, n: Int) {
        val hdr = ByteBuffer.allocate(4).putInt(n).array()
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, txKey, GCMParameterSpec(128, nonce(txDir, txSeq++)))
        c.updateAAD(hdr)
        rawOut.write(hdr)
        rawOut.write(c.doFinal(p, off, n))
    }

    private fun recvRecord(): ByteArray {
        val hdr = ByteArray(4).also { raw.readFully(it) }
        val n = ByteBuffer.wrap(hdr).int
        if (n < 0 || n > Crypto.RECORD_MAX) throw java.io.IOException("bad record")
        val ct = ByteArray(n + 16).also { raw.readFully(it) }
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.DECRYPT_MODE, rxKey, GCMParameterSpec(128, nonce(rxDir, rxSeq++)))
        c.updateAAD(hdr)
        return c.doFinal(ct)  // throws if tampered with
    }

    val output: OutputStream = object : OutputStream() {
        private val buf = ByteArrayOutputStream(Crypto.RECORD_MAX)
        override fun write(b: Int) = write(byteArrayOf(b.toByte()), 0, 1)
        override fun write(b: ByteArray, off: Int, len: Int) {
            var o = off
            var left = len
            while (left > 0) {
                val take = minOf(left, Crypto.RECORD_MAX - buf.size())
                buf.write(b, o, take)
                o += take
                left -= take
                if (buf.size() == Crypto.RECORD_MAX) flush()
            }
        }
        override fun flush() {
            if (buf.size() == 0) return
            sendRecord(buf.toByteArray(), 0, buf.size())
            buf.reset()
            rawOut.flush()
        }
    }

    val input: InputStream = object : InputStream() {
        private var cur = ByteArray(0)
        private var pos = 0
        private fun fill(): Boolean {
            output.flush()  // never wait for a reply with our own bytes still buffered
            while (pos == cur.size) {
                cur = try { recvRecord() } catch (e: java.io.EOFException) { return false }
                pos = 0
            }
            return true
        }
        override fun read(): Int = if (!fill()) -1 else cur[pos++].toInt() and 0xFF
        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (len == 0) return 0
            if (!fill()) return -1
            val n = minOf(len, cur.size - pos)
            System.arraycopy(cur, pos, b, off, n)
            pos += n
            return n
        }
    }

    companion object {
        /**
         * Client side: proves we hold [key]. Null if the other end isn't our paired PC; throws
         * [Incompatible] if it runs a WinDrop with another protocol.
         */
        fun client(s: Socket, key: ByteArray, myId: String): SecureChannel? {
            val out = s.getOutputStream()
            val inp = DataInputStream(s.getInputStream())
            val nc = Crypto.random(16)
            val id = myId.toByteArray()
            out.write(Crypto.envelope(Crypto.HELLO_SESSION) + id.size.toByte() + id + nc)
            out.flush()
            when (Crypto.readStatus(inp)) {
                Crypto.STATUS_GO -> Unit
                Crypto.STATUS_REFUSED -> throw NotPaired()
                else -> return null
            }
            val ns = ByteArray(16).also { inp.readFully(it) }
            val macS = ByteArray(32).also { inp.readFully(it) }
            if (!Crypto.same(macS, Crypto.hmac(key, "WDR2 srv".toByteArray(), nc, ns))) return null
            out.write(Crypto.hmac(key, "WDR2 cli".toByteArray(), nc, ns))
            out.flush()
            // Nothing arrives until we send, so buffering from here on loses nothing.
            return SecureChannel(s, DataInputStream(s.getInputStream().buffered(256 * 1024)), key, nc, ns, true)
        }

        /**
         * Server side, after the caller read an envelope saying HELLO_SESSION with our protocol.
         * [keyFor] maps the client's id to its key (null = not paired: refuse).
         */
        fun server(s: Socket, inp: DataInputStream, keyFor: (String) -> ByteArray?): SecureChannel? {
            val idLen = inp.readUnsignedByte()
            if (idLen == 0) return null
            val id = String(ByteArray(idLen).also { inp.readFully(it) })
            val nc = ByteArray(16).also { inp.readFully(it) }
            val out = s.getOutputStream()
            val key = keyFor(id) ?: run { out.write(Crypto.STATUS_REFUSED); out.flush(); return null }
            val ns = Crypto.random(16)
            out.write(byteArrayOf(Crypto.STATUS_GO.toByte()) + ns + Crypto.hmac(key, "WDR2 srv".toByteArray(), nc, ns))
            out.flush()
            val macC = ByteArray(32).also { inp.readFully(it) }
            if (!Crypto.same(macC, Crypto.hmac(key, "WDR2 cli".toByteArray(), nc, ns))) return null
            return SecureChannel(s, inp, key, nc, ns, false)
        }
    }
}

/** Phone side of pairing; see windows NetworkEngine::HandlePair. */
object Pairing {
    class Result(val key: ByteArray, val pcId: String, val pcName: String)

    /**
     * Runs the exchange over [s]. [onSas] gets the 6-digit check number to show while the PC
     * asks its user to confirm. Returns null if refused (pairing not open on the PC, or declined);
     * throws [Incompatible] if the PC's WinDrop speaks another protocol.
     */
    fun run(s: Socket, myId: String, myName: String, onSas: (String) -> Unit): Result? {
        val out = s.getOutputStream()
        val inp = DataInputStream(s.getInputStream())
        val ec = Crypto.Ecdh()
        val nP = Crypto.random(16)
        val id = myId.toByteArray().copyOf(minOf(myId.length, 255))
        val name = myName.toByteArray().let { if (it.size > 255) it.copyOf(255) else it }
        val m1 = ec.publicKey + nP + id.size.toByte() + id + name.size.toByte() + name
        out.write(Crypto.envelope(Crypto.HELLO_PAIR) + m1)
        out.flush()

        if (Crypto.readStatus(inp) != Crypto.STATUS_GO) return null  // the PC isn't in pairing mode
        val pubS = ByteArray(65).also { inp.readFully(it) }
        val nS = ByteArray(16).also { inp.readFully(it) }
        val pcId = ByteArray(inp.readUnsignedByte()).also { inp.readFully(it) }
        val pcName = ByteArray(inp.readUnsignedByte()).also { inp.readFully(it) }
        val m2 = pubS + nS + pcId.size.toByte() + pcId + pcName.size.toByte() + pcName

        val t = Crypto.sha256("WDR2 pair".toByteArray(), m1, m2)
        val z = ec.agree(pubS) ?: return null
        val key = Crypto.hkdf(z, t, "WDR2 key", 32)
        val sas = ByteBuffer.wrap(Crypto.hkdf(z, t, "WDR2 sas", 4)).int.toLong().and(0xFFFFFFFFL) % 1_000_000
        onSas("%06d".format(sas))

        if (inp.readUnsignedByte() != 1) return null  // declined on the PC
        val pcMac = ByteArray(32).also { inp.readFully(it) }
        if (!Crypto.same(pcMac, Crypto.hmac(key, "WDR2 pc-ok".toByteArray(), t))) return null
        out.write(Crypto.hmac(key, "WDR2 phone-ok".toByteArray(), t))
        out.flush()
        if (inp.readUnsignedByte() != 0) return null  // PC didn't store it
        return Result(key, String(pcId), String(pcName))
    }
}

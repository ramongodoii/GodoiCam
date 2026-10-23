package com.godoi.godoican.net

import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.io.IOException
import java.nio.ByteBuffer

fun ByteArray.hex(): String = joinToString("") { "%02x".format(it) }

fun hexToBytes(s: String): ByteArray =
    ByteArray(s.length / 2) { s.substring(it * 2, it * 2 + 2).toInt(16).toByte() }

/** Monta payloads big-endian. */
class Writer {
    private val bytes = ByteArrayOutputStream()
    private val out = DataOutputStream(bytes)

    fun u8(v: Int) = apply { out.writeByte(v) }
    fun u16(v: Int) = apply { out.writeShort(v) }
    fun u32(v: Int) = apply { out.writeInt(v) }
    fun i64(v: Long) = apply { out.writeLong(v) }
    fun bytes(b: ByteArray) = apply { out.write(b) }
    fun str(s: String) = apply {
        val b = s.toByteArray(Charsets.UTF_8).let { if (it.size > 1024) it.copyOf(1024) else it }
        out.writeShort(b.size)
        out.write(b)
    }

    fun build(): ByteArray = bytes.toByteArray()
}

/** Lê payloads big-endian, lançando IOException se o payload acabar antes da hora. */
class Reader(data: ByteArray) {
    private val buf = ByteBuffer.wrap(data)

    private fun need(n: Int) {
        if (buf.remaining() < n) throw IOException("Mensagem truncada")
    }

    fun u8(): Int { need(1); return buf.get().toInt() and 0xff }
    fun u16(): Int { need(2); return buf.short.toInt() and 0xffff }
    fun u32(): Int { need(4); return buf.int }
    fun i64(): Long { need(8); return buf.long }
    fun bytes(n: Int): ByteArray { need(n); return ByteArray(n).also { buf.get(it) } }
    fun str(): String = String(bytes(u16()), Charsets.UTF_8)
}

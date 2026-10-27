package com.godoi.godoicam.net

import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.Socket

/** Lê e escreve quadros `u8 tipo | u32 tamanho | payload`, cifrando depois da autenticação. */
class FrameIO(socket: Socket) {
    class Frame(val type: Int, val data: ByteArray)

    private val input = DataInputStream(BufferedInputStream(socket.getInputStream(), 64 * 1024))
    private val output = socket.getOutputStream()
    private val writeLock = Any()

    @Volatile var sendCipher: Aead? = null
    @Volatile var recvCipher: Aead? = null

    fun send(type: Int, payload: ByteArray) {
        synchronized(writeLock) {
            // Cabeçalho e corpo no mesmo array: uma única escrita no socket por quadro.
            val frame = sendCipher?.seal(type, payload, 5)
                ?: ByteArray(5 + payload.size).also { payload.copyInto(it, 5) }
            val len = frame.size - 5
            frame[0] = type.toByte()
            frame[1] = (len ushr 24).toByte()
            frame[2] = (len ushr 16).toByte()
            frame[3] = (len ushr 8).toByte()
            frame[4] = len.toByte()
            output.write(frame)
            output.flush()
        }
    }

    fun sendError(code: Int, message: String) {
        send(Proto.ERROR, byteArrayOf(code.toByte()) + message.toByteArray(Charsets.UTF_8))
    }

    fun receive(): Frame {
        val type = input.readUnsignedByte()
        val len = input.readInt()
        if (len < 0 || len > Proto.MAX_FRAME) throw IOException("Quadro inválido ($len bytes)")
        val data = ByteArray(len)
        input.readFully(data)
        return Frame(type, recvCipher?.open(type, data) ?: data)
    }
}

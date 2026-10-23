package com.godoi.godoican.net

import java.io.IOException
import java.net.Socket
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread

/**
 * Uma sessão autenticada com um PC. Os quadros de vídeo passam por uma fila curta: se a
 * rede não der conta, os quadros velhos são descartados e o próximo enviado é um
 * quadro-chave, para a imagem continuar atual em vez de acumular atraso.
 */
class ClientSession(
    private val socket: Socket,
    private val io: FrameIO,
    val pcId: ByteArray,
    val pcName: String,
) {
    interface Listener {
        fun onKeyframeRequest()
        fun onCongestion()
    }

    private class Packet(val type: Int, val data: ByteArray)

    @Volatile var listener: Listener? = null
    /** Relógio dos timestamps do vídeo, usado no PONG para o PC estimar o atraso. */
    @Volatile var clock: () -> Long = { System.nanoTime() / 1000 }
    @Volatile var closed = false
        private set

    val bytesSent = AtomicLong()
    val framesSent = AtomicLong()
    val framesDropped = AtomicLong()

    private val lock = Object()
    private val queue = ArrayDeque<Packet>()
    private var queuedVideo = 0
    private var waitingKeyframe = false

    private val sender = thread(name = "GodoiCan-send", isDaemon = true) { sendLoop() }

    fun sendConfig(payload: ByteArray) = enqueue(Packet(Proto.CONFIG, payload), urgent = false)

    fun sendReport(text: String) = enqueue(Packet(Proto.REPORT, text.toByteArray(Charsets.UTF_8)), urgent = false)

    fun sendStats(payload: ByteArray) = enqueue(Packet(Proto.STATS, payload), urgent = false)

    /** Quadros de vídeo esperando na fila de envio agora. */
    fun queuedFrames(): Int = synchronized(lock) { queuedVideo }

    /** [payload] = flags (1) + pts (8) + H.264. Chamado pela thread do encoder. */
    fun offerVideo(payload: ByteArray) {
        val flags = payload[0].toInt()
        val isKey = flags and Proto.VIDEO_KEY != 0
        val isConfig = flags and Proto.VIDEO_CONFIG != 0
        var congested = false
        synchronized(lock) {
            if (closed) return
            if (!isConfig) {
                if (waitingKeyframe && !isKey) {
                    framesDropped.incrementAndGet()
                    return
                }
                if (queuedVideo >= MAX_QUEUED_FRAMES) {
                    val before = queue.size
                    queue.removeAll { it.type == Proto.VIDEO && it.data[0].toInt() and Proto.VIDEO_CONFIG == 0 }
                    framesDropped.addAndGet((before - queue.size).toLong())
                    queuedVideo = 0
                    congested = true
                    if (!isKey) {
                        waitingKeyframe = true
                        framesDropped.incrementAndGet()
                    }
                }
                if (isKey) waitingKeyframe = false
            }
            if (!waitingKeyframe || isConfig) {
                queue.addLast(Packet(Proto.VIDEO, payload))
                if (!isConfig) queuedVideo++
                lock.notifyAll()
            }
        }
        if (congested) listener?.onCongestion()
    }

    private fun enqueue(p: Packet, urgent: Boolean) {
        synchronized(lock) {
            if (closed) return
            if (urgent) queue.addFirst(p) else queue.addLast(p)
            lock.notifyAll()
        }
    }

    private fun sendLoop() {
        try {
            while (true) {
                val p = synchronized(lock) {
                    while (queue.isEmpty() && !closed) lock.wait()
                    if (closed) return
                    queue.removeFirst().also {
                        if (it.type == Proto.VIDEO && it.data[0].toInt() and Proto.VIDEO_CONFIG == 0) queuedVideo--
                    }
                }
                io.send(p.type, p.data)
                bytesSent.addAndGet(p.data.size.toLong())
                if (p.type == Proto.VIDEO) framesSent.incrementAndGet()
            }
        } catch (_: IOException) {
            close()
        } catch (_: InterruptedException) {
            close()
        }
    }

    /** Lê mensagens do PC até a conexão cair. Roda na thread que aceitou o socket. */
    fun readLoop() {
        try {
            while (!closed) {
                val f = io.receive()
                when (f.type) {
                    Proto.PING -> {
                        val pcTime = Reader(f.data).i64()
                        enqueue(Packet(Proto.PONG, Writer().i64(pcTime).i64(clock()).build()), urgent = true)
                    }
                    Proto.KEYFRAME_REQ -> listener?.onKeyframeRequest()
                    Proto.BYE -> return
                }
            }
        } catch (_: Exception) {
            // Conexão caiu, tempo esgotado ou falha de autenticação do quadro.
        } finally {
            close()
        }
    }

    fun close() {
        synchronized(lock) {
            if (closed) return
            closed = true
            queue.clear()
            lock.notifyAll()
        }
        try {
            socket.close()
        } catch (_: IOException) {
        }
    }

    companion object {
        private const val MAX_QUEUED_FRAMES = 3
    }
}

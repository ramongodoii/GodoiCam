package com.godoi.godoican.net

import android.os.SystemClock
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

    private class Packet(val type: Int, val data: ByteArray) {
        val queuedAt = SystemClock.elapsedRealtime()
    }

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
                var stale = false
                val p = synchronized(lock) {
                    while (queue.isEmpty() && !closed) lock.wait()
                    if (closed) return
                    val next = queue.removeFirst()
                    val flags = next.data[0].toInt()
                    if (next.type == Proto.VIDEO && flags and Proto.VIDEO_CONFIG == 0) {
                        queuedVideo--
                        // Quadro velho demais: descarta e recomeça num quadro-chave.
                        if (flags and Proto.VIDEO_KEY == 0 &&
                            SystemClock.elapsedRealtime() - next.queuedAt > STALE_MS
                        ) {
                            stale = true
                            waitingKeyframe = true
                            framesDropped.incrementAndGet()
                        }
                    }
                    next
                }
                if (stale) {
                    signalCongestion()
                    continue
                }
                io.send(p.type, p.data)
                bytesSent.addAndGet(p.data.size.toLong())
                if (p.type == Proto.VIDEO) {
                    framesSent.incrementAndGet()
                    // Com o buffer do sistema pequeno, um envio lento indica rede lenta.
                    if (SystemClock.elapsedRealtime() - p.queuedAt > SLOW_SEND_MS) signalCongestion()
                }
            }
        } catch (_: IOException) {
            close()
        } catch (_: InterruptedException) {
            close()
        }
    }

    @Volatile private var lastCongestionSignal = 0L

    /** Avisa a rede lenta no máximo uma vez por segundo (baixa bitrate e pede quadro-chave). */
    private fun signalCongestion() {
        val now = SystemClock.elapsedRealtime()
        if (now - lastCongestionSignal < 1000) return
        lastCongestionSignal = now
        listener?.onCongestion()
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
        /** Quadro que esperou mais que isso na fila é descartado. */
        private const val STALE_MS = 200L
        /** Envio que levou mais que isso indica congestionamento. */
        private const val SLOW_SEND_MS = 150L
        /** Buffer de envio do sistema: ~50 ms de vídeo a 10 Mbps, para o atraso não se esconder nele. */
        const val SOCKET_SEND_BUFFER = 64 * 1024
    }
}

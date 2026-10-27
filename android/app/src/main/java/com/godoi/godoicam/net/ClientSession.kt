package com.godoi.godoicam.net

import android.os.SystemClock
import java.io.IOException
import java.net.Socket
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread

/**
 * Uma sessão autenticada com um PC. Os quadros de vídeo passam por uma fila curta: se a
 * rede não der conta, os quadros velhos são descartados e o próximo enviado é um
 * quadro-chave, para a imagem continuar atual em vez de acumular atraso.
 *
 * Com [frameAcks] o PC confirma cada quadro recebido. Aí o envio limita o vídeo em trânsito
 * (janela proporcional ao bitrate) e o congestionamento é medido pelo atraso real até o PC,
 * em vez de adivinhado pelo tamanho da fila.
 */
class ClientSession(
    private val socket: Socket,
    private val io: FrameIO,
    val pcId: ByteArray,
    val pcName: String,
    val frameAcks: Boolean,
) {
    interface Listener {
        fun onKeyframeRequest()
        /** [severe]: quadros foram descartados (não só atraso subindo). */
        fun onCongestion(severe: Boolean)
        /** O PC mudou um ajuste da câmera ([Proto].CTL_*). */
        fun onRemoteControl(key: Int, value: Int) {}
    }

    private class Packet(val type: Int, val data: ByteArray) {
        val queuedAt = SystemClock.elapsedRealtime()
    }

    private class InFlight(val pts: Long, val bytes: Int, val sentAt: Long)

    @Volatile var listener: Listener? = null
    /** Relógio dos timestamps do vídeo, usado no PONG para o PC estimar o atraso. */
    @Volatile var clock: () -> Long = { System.nanoTime() / 1000 }
    /** Bitrate atual do encoder, para dimensionar a janela de envio. */
    @Volatile var bitrate = 10_000_000
    @Volatile var closed = false
        private set

    val bytesSent = AtomicLong()
    val framesSent = AtomicLong()
    val framesDropped = AtomicLong()

    private val lock = Object()
    private val queue = ArrayDeque<Packet>()
    private var queuedVideo = 0
    private var waitingKeyframe = false

    private val inFlight = ArrayDeque<InFlight>()
    private var inFlightBytes = 0L
    /** Atraso suavizado envio → confirmação do PC (ms); -1 antes da primeira confirmação. */
    private var smoothDelayMs = -1.0
    private var delaySumMs = 0L
    private var delayCount = 0

    private val sender = thread(name = "GodoiCam-send", isDaemon = true) { sendLoop() }

    fun sendConfig(payload: ByteArray) = enqueue(Packet(Proto.CONFIG, payload), urgent = false)

    fun sendReport(text: String) = enqueue(Packet(Proto.REPORT, text.toByteArray(Charsets.UTF_8)), urgent = false)

    fun sendStats(payload: ByteArray) = enqueue(Packet(Proto.STATS, payload), urgent = false)

    fun sendCameraState(payload: ByteArray) = enqueue(Packet(Proto.CAMERA_STATE, payload), urgent = false)

    /** Quadros de vídeo esperando na fila de envio agora. */
    fun queuedFrames(): Int = synchronized(lock) { queuedVideo }

    /** Atraso médio envio → confirmação do PC (ms) desde a última chamada; -1 sem amostras. */
    fun takeNetDelayMs(): Int = synchronized(lock) {
        val r = if (delayCount > 0) (delaySumMs / delayCount).toInt() else -1
        delaySumMs = 0
        delayCount = 0
        r
    }

    /** [payload] = flags (1) + pts (8) + H.264. Chamado pela thread do encoder. */
    fun offerVideo(payload: ByteArray) {
        val flags = payload[0].toInt()
        val isKey = flags and Proto.VIDEO_KEY != 0
        val isConfig = flags and Proto.VIDEO_CONFIG != 0
        var congested = false
        var needKeyframe = false
        synchronized(lock) {
            if (closed) return
            if (!isConfig) {
                if (waitingKeyframe && !isKey) {
                    framesDropped.incrementAndGet()
                    return
                }
                if (!frameAcks && queuedVideo >= EARLY_WARNING_FRAMES) congested = true
                if (queuedVideo >= MAX_QUEUED_FRAMES) {
                    val before = queue.size
                    queue.removeAll { it.type == Proto.VIDEO && it.data[0].toInt() and Proto.VIDEO_CONFIG == 0 }
                    framesDropped.addAndGet((before - queue.size).toLong())
                    queuedVideo = 0
                    if (!isKey) {
                        waitingKeyframe = true
                        framesDropped.incrementAndGet()
                        needKeyframe = true
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
        if (needKeyframe) {
            signalCongestion(severe = true)
            listener?.onKeyframeRequest()
        } else if (congested) {
            signalCongestion(severe = false)
        }
    }

    private fun enqueue(p: Packet, urgent: Boolean) {
        synchronized(lock) {
            if (closed) return
            if (urgent) queue.addFirst(p) else queue.addLast(p)
            lock.notifyAll()
        }
    }

    private fun isVideo(p: Packet) = p.type == Proto.VIDEO && p.data[0].toInt() and Proto.VIDEO_CONFIG == 0

    private fun pts(p: Packet): Long {
        var v = 0L
        for (i in 1..8) v = (v shl 8) or (p.data[i].toLong() and 0xff)
        return v
    }

    /** Vídeo em trânsito permitido: ~200 ms do bitrate atual, nunca menos que [MIN_WINDOW]. */
    private fun window(): Long = maxOf(MIN_WINDOW, bitrate.toLong() / 8 * WINDOW_MS / 1000)

    /** Próximo pacote que pode sair agora (com a trava). Vídeo espera a janela abrir. */
    private fun nextSendable(): Packet? {
        val first = queue.firstOrNull() ?: return null
        if (!frameAcks || !isVideo(first) || inFlightBytes == 0L ||
            inFlightBytes + first.data.size <= window()
        ) {
            return queue.removeFirst()
        }
        // Janela cheia: mensagens pequenas de controle não esperam atrás do vídeo.
        val i = queue.indexOfFirst { it.type == Proto.PONG || it.type == Proto.STATS || it.type == Proto.CAMERA_STATE }
        return if (i >= 0) queue.removeAt(i) else null
    }

    private fun sendLoop() {
        try {
            while (true) {
                var stale = false
                val p = synchronized(lock) {
                    var next: Packet? = null
                    while (!closed) {
                        next = nextSendable()
                        if (next != null) break
                        // Fila vazia: espera um quadro. Janela cheia: espera uma confirmação.
                        if (queue.isEmpty()) lock.wait() else lock.wait(WINDOW_POLL_MS)
                        if (queue.isNotEmpty() && isVideo(queue.first()) &&
                            SystemClock.elapsedRealtime() - queue.first().queuedAt > STALE_MS
                        ) {
                            break
                        }
                    }
                    if (closed) return
                    val n = next ?: queue.removeFirst()
                    if (isVideo(n)) {
                        queuedVideo--
                        // Quadro velho demais: descarta e recomeça num quadro-chave.
                        if (n.data[0].toInt() and Proto.VIDEO_KEY == 0 &&
                            SystemClock.elapsedRealtime() - n.queuedAt > STALE_MS
                        ) {
                            stale = true
                            waitingKeyframe = true
                            framesDropped.incrementAndGet()
                            val before = queue.size
                            queue.removeAll { isVideo(it) && it.data[0].toInt() and Proto.VIDEO_KEY == 0 }
                            framesDropped.addAndGet((before - queue.size).toLong())
                            queuedVideo = queue.count(::isVideo)
                        } else if (frameAcks) {
                            inFlight.addLast(InFlight(pts(n), n.data.size, SystemClock.elapsedRealtime()))
                            inFlightBytes += n.data.size
                        }
                    }
                    n
                }
                if (stale) {
                    signalCongestion(severe = true)
                    listener?.onKeyframeRequest()
                    continue
                }
                io.send(p.type, p.data)
                bytesSent.addAndGet(p.data.size.toLong())
                if (isVideo(p)) {
                    framesSent.incrementAndGet()
                    val waited = SystemClock.elapsedRealtime() - p.queuedAt
                    // Sem confirmação do PC, um envio lento é o único sinal de rede lenta.
                    // Quadro-chave é grande e naturalmente demora: não conta.
                    if (!frameAcks && waited > SLOW_SEND_MS && p.data[0].toInt() and Proto.VIDEO_KEY == 0) {
                        signalCongestion(severe = false)
                    } else if (frameAcks && waited > QUEUE_WAIT_MS) {
                        signalCongestion(severe = false)
                    }
                }
            }
        } catch (_: IOException) {
            close()
        } catch (_: InterruptedException) {
            close()
        }
    }

    /** O PC confirmou ter recebido o quadro [pts] (e todos antes dele). */
    private fun onFrameAck(pts: Long) {
        var congested = false
        synchronized(lock) {
            val now = SystemClock.elapsedRealtime()
            // As confirmações chegam na ordem do envio (TCP), então basta comparar com o mais antigo.
            while (inFlight.isNotEmpty() && inFlight.first().pts <= pts) {
                val f = inFlight.removeFirst()
                inFlightBytes -= f.bytes
                val d = (now - f.sentAt).toDouble()
                delaySumMs += d.toLong()
                delayCount++
                smoothDelayMs = if (smoothDelayMs < 0) d else smoothDelayMs * 0.8 + d * 0.2
            }
            if (inFlight.isEmpty()) inFlightBytes = 0
            if (smoothDelayMs > CONGESTED_DELAY_MS) congested = true
            lock.notifyAll()
        }
        if (congested) signalCongestion(severe = false)
    }

    @Volatile private var lastCongestionSignal = 0L

    /** Avisa a rede lenta no máximo uma vez por segundo. */
    private fun signalCongestion(severe: Boolean) {
        val now = SystemClock.elapsedRealtime()
        if (now - lastCongestionSignal < 1000) return
        lastCongestionSignal = now
        listener?.onCongestion(severe)
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
                    Proto.FRAME_ACK -> onFrameAck(Reader(f.data).i64())
                    Proto.KEYFRAME_REQ -> listener?.onKeyframeRequest()
                    Proto.CONTROL -> {
                        val r = Reader(f.data)
                        listener?.onRemoteControl(r.u8(), r.u32())
                    }
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
            inFlight.clear()
            lock.notifyAll()
        }
        try {
            socket.close()
        } catch (_: IOException) {
        }
    }

    companion object {
        /** Sem confirmação do PC: a partir daqui a fila já indica congestionamento. */
        private const val EARLY_WARNING_FRAMES = 3
        /** Só com a fila cheia descarta quadros (e aí precisa de um quadro-chave). */
        private const val MAX_QUEUED_FRAMES = 8
        /** Quadro que esperou mais que isso na fila é descartado. */
        private const val STALE_MS = 300L
        /** Sem confirmação do PC: envio que levou mais que isso indica congestionamento. */
        private const val SLOW_SEND_MS = 150L
        /** Com confirmação: quadro que esperou a janela abrir por mais que isso indica congestionamento. */
        private const val QUEUE_WAIT_MS = 100L
        /** Com confirmação: atraso suavizado até o PC acima disso indica congestionamento. */
        private const val CONGESTED_DELAY_MS = 120.0
        /** Vídeo em trânsito: duração do bitrate atual e piso em bytes. */
        private const val WINDOW_MS = 200L
        private const val MIN_WINDOW = 192L * 1024
        private const val WINDOW_POLL_MS = 20L
        /** Buffer de envio do sistema sem confirmação: ~50 ms de vídeo a 10 Mbps. */
        const val SOCKET_SEND_BUFFER = 64 * 1024
        /** Com confirmação a janela já limita o atraso; o buffer só não pode travar a vazão. */
        const val SOCKET_SEND_BUFFER_ACKED = 1024 * 1024
    }
}

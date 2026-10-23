package com.godoi.godoican.net

import android.os.SystemClock
import java.io.IOException
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import kotlin.concurrent.thread

/** Servidor TCP: aceita um PC por vez, faz o pareamento/autenticação e entrega a sessão. */
class StreamServer(
    private val pairs: PairStore,
    private val phoneName: () -> String,
    private val callbacks: Callbacks,
) {
    interface Callbacks {
        fun onPairingCode(code: String?, pcName: String?)
        fun onPaired(pcName: String)
        fun onSessionStarted(session: ClientSession)
        fun onSessionEnded(session: ClientSession)
        fun onServerMessage(message: String)
    }

    @Volatile private var serverSocket: ServerSocket? = null
    @Volatile private var current: ClientSession? = null
    @Volatile private var pairingInProgress = false
    @Volatile private var lastPairFailure = 0L

    fun start() {
        val ss = ServerSocket()
        ss.reuseAddress = true
        ss.bind(InetSocketAddress(Proto.TCP_PORT))
        serverSocket = ss
        thread(name = "GodoiCan-accept", isDaemon = true) {
            while (!ss.isClosed) {
                val s = try {
                    ss.accept()
                } catch (_: IOException) {
                    break
                }
                thread(name = "GodoiCan-client", isDaemon = true) { handle(s) }
            }
        }
    }

    fun stop() {
        try {
            serverSocket?.close()
        } catch (_: IOException) {
        }
        current?.close()
    }

    private fun handle(sock: Socket) {
        var session: ClientSession? = null
        try {
            sock.tcpNoDelay = true
            sock.soTimeout = 15_000
            val io = FrameIO(sock)

            val hello = io.receive()
            if (hello.type != Proto.HELLO) return
            val r = Reader(hello.data)
            if (!r.bytes(4).contentEquals(Proto.MAGIC)) return
            if (r.u16() != Proto.VERSION) {
                io.sendError(Proto.ERR_VERSION, "Versão incompatível. Atualize o GodoiCan.")
                return
            }
            val pcId = r.bytes(16)
            val flags = r.u8()
            val pcNonce = r.bytes(16)
            val pcPub = r.bytes(65)
            val pcName = r.str().ifBlank { "PC" }

            val cur = current
            if (cur != null && !cur.closed) {
                if (cur.pcId.contentEquals(pcId)) {
                    cur.close() // o mesmo PC reconectando: derruba a sessão antiga
                } else {
                    io.sendError(Proto.ERR_BUSY, "O celular já está conectado a ${cur.pcName}.")
                    return
                }
            }

            val phoneId = pairs.phoneId
            val phoneNonce = Crypto.random(16)
            val stored = if (flags and Proto.HELLO_FORCE_PAIR == 0) pairs.key(pcId) else null

            val key = if (stored != null) {
                authenticate(io, stored, pcId, phoneId, pcNonce, phoneNonce)
            } else {
                pair(io, sock, pcId, pcName, pcPub, phoneId, pcNonce, phoneNonce)
            } ?: return

            io.sendCipher = Aead(Crypto.hmac(key, Crypto.label("GDCN-key-p2c"), pcNonce, phoneNonce))
            io.recvCipher = Aead(Crypto.hmac(key, Crypto.label("GDCN-key-c2p"), pcNonce, phoneNonce))
            sock.soTimeout = 10_000

            val s = ClientSession(sock, io, pcId, pcName)
            session = s
            current = s
            callbacks.onSessionStarted(s)
            s.readLoop()
        } catch (e: Exception) {
            if (session == null) callbacks.onServerMessage("Falha na conexão: ${e.message ?: e.javaClass.simpleName}")
        } finally {
            if (session != null) {
                session.close()
                if (current === session) current = null
                callbacks.onSessionEnded(session)
            } else {
                try {
                    sock.close()
                } catch (_: IOException) {
                }
            }
        }
    }

    private fun authenticate(
        io: FrameIO, key: ByteArray, pcId: ByteArray, phoneId: ByteArray,
        pcNonce: ByteArray, phoneNonce: ByteArray,
    ): ByteArray? {
        io.send(Proto.CHALLENGE, Writer().bytes(phoneId).bytes(phoneNonce).str(phoneName()).build())
        val reply = io.receive()
        if (reply.type != Proto.AUTH) return null // o PC perdeu a chave: ele reconecta pedindo pareamento
        val expected = Crypto.hmac(key, Crypto.label("GDCN-auth-pc"), pcId, phoneId, pcNonce, phoneNonce)
        if (!Crypto.same(reply.data, expected)) {
            io.sendError(Proto.ERR_AUTH, "Autenticação falhou. Pareie de novo.")
            return null
        }
        io.send(Proto.AUTH_OK, Crypto.hmac(key, Crypto.label("GDCN-auth-phone"), pcId, phoneId, pcNonce, phoneNonce))
        return key
    }

    private fun pair(
        io: FrameIO, sock: Socket, pcId: ByteArray, pcName: String, pcPub: ByteArray,
        phoneId: ByteArray, pcNonce: ByteArray, phoneNonce: ByteArray,
    ): ByteArray? {
        if (pairingInProgress) {
            io.sendError(Proto.ERR_BUSY, "Outro pareamento já está em andamento.")
            return null
        }
        if (SystemClock.elapsedRealtime() - lastPairFailure < 3_000) {
            io.sendError(Proto.ERR_BAD_CODE, "Aguarde alguns segundos e tente de novo.")
            return null
        }
        pairingInProgress = true
        try {
            val ecdh = Crypto.Ecdh()
            val phonePub = ecdh.publicBytes()
            val code = Crypto.randomCode()
            io.send(
                Proto.PAIR_CHALLENGE,
                Writer().bytes(phoneId).bytes(phoneNonce).bytes(phonePub).str(phoneName()).build(),
            )
            callbacks.onPairingCode(code, pcName)
            sock.soTimeout = 120_000

            val proof = io.receive()
            if (proof.type != Proto.AUTH) return null
            val z = ecdh.agree(pcPub)
            val p = Crypto.sha256(Crypto.label("GDCN-pair"), z, code.toByteArray(Charsets.US_ASCII))
            val t = pcPub + phonePub + pcNonce + phoneNonce + pcId + phoneId
            if (!Crypto.same(proof.data, Crypto.hmac(p, Crypto.label("GDCN-pair-pc"), t))) {
                lastPairFailure = SystemClock.elapsedRealtime()
                io.sendError(Proto.ERR_BAD_CODE, "Código incorreto.")
                callbacks.onServerMessage("Código incorreto digitado no PC.")
                return null
            }
            io.send(Proto.AUTH_OK, Crypto.hmac(p, Crypto.label("GDCN-pair-phone"), t))
            val key = Crypto.hmac(p, Crypto.label("GDCN-pair-key"), t)
            pairs.put(pcId, key, pcName)
            callbacks.onPaired(pcName)
            return key
        } finally {
            pairingInProgress = false
            callbacks.onPairingCode(null, null)
        }
    }
}

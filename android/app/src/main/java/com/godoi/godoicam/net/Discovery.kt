package com.godoi.godoicam.net

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import kotlin.concurrent.thread

/**
 * Descoberta na rede local: responde aos broadcasts do PC e, enquanto nenhum PC está
 * conectado, se anuncia sozinho a cada 2 s para o PC conectar assim que o app abre.
 */
class DiscoveryResponder(private val phoneIdHex: String, private val phoneName: () -> String) {
    @Volatile private var socket: DatagramSocket? = null

    /** Desligado durante uma sessão, para não gerar tráfego à toa. */
    @Volatile var announce = true

    private fun message(): ByteArray {
        val name = phoneName().replace('|', ' ')
        return "${Proto.DISCOVER_REPLY}|$phoneIdHex|${Proto.TCP_PORT}|$name".toByteArray(Charsets.UTF_8)
    }

    fun start() {
        val s = DatagramSocket(null)
        s.reuseAddress = true
        s.broadcast = true
        s.bind(InetSocketAddress(Proto.UDP_PORT))
        socket = s
        thread(name = "GodoiCam-discovery", isDaemon = true) {
            val buf = ByteArray(256)
            while (!s.isClosed) {
                try {
                    val packet = DatagramPacket(buf, buf.size)
                    s.receive(packet)
                    val text = String(packet.data, 0, packet.length, Charsets.UTF_8)
                    if (!text.startsWith(Proto.DISCOVER_REQUEST)) continue
                    val reply = message()
                    s.send(DatagramPacket(reply, reply.size, packet.socketAddress))
                } catch (_: Exception) {
                    if (s.isClosed) break
                }
            }
        }
        thread(name = "GodoiCam-announce", isDaemon = true) {
            val target = InetSocketAddress(InetAddress.getByName("255.255.255.255"), Proto.UDP_PORT)
            while (!s.isClosed) {
                if (announce) {
                    try {
                        val m = message()
                        s.send(DatagramPacket(m, m.size, target))
                    } catch (_: Exception) {
                        // Sem rede no momento; tenta de novo no próximo ciclo.
                    }
                }
                try {
                    Thread.sleep(2000)
                } catch (_: InterruptedException) {
                    break
                }
            }
        }
    }

    fun stop() {
        socket?.close()
        socket = null
    }
}

package com.godoi.godoican.net

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import kotlin.concurrent.thread

/** Responde aos broadcasts UDP do PC para ele achar o celular na rede local. */
class DiscoveryResponder(private val phoneIdHex: String, private val phoneName: () -> String) {
    @Volatile private var socket: DatagramSocket? = null

    fun start() {
        val s = DatagramSocket(null)
        s.reuseAddress = true
        s.broadcast = true
        s.bind(InetSocketAddress(Proto.UDP_PORT))
        socket = s
        thread(name = "GodoiCan-discovery", isDaemon = true) {
            val buf = ByteArray(256)
            while (!s.isClosed) {
                try {
                    val packet = DatagramPacket(buf, buf.size)
                    s.receive(packet)
                    val text = String(packet.data, 0, packet.length, Charsets.UTF_8)
                    if (!text.startsWith(Proto.DISCOVER_REQUEST)) continue
                    val name = phoneName().replace('|', ' ')
                    val reply = "${Proto.DISCOVER_REPLY}|$phoneIdHex|${Proto.TCP_PORT}|$name"
                        .toByteArray(Charsets.UTF_8)
                    s.send(DatagramPacket(reply, reply.size, packet.socketAddress))
                } catch (_: Exception) {
                    if (s.isClosed) break
                }
            }
        }
    }

    fun stop() {
        socket?.close()
        socket = null
    }
}

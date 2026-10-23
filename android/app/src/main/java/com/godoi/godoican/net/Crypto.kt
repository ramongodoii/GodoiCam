package com.godoi.godoican.net

import java.math.BigInteger
import java.nio.ByteBuffer
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

object Crypto {
    private val rng = SecureRandom()

    fun random(n: Int): ByteArray = ByteArray(n).also { rng.nextBytes(it) }

    fun randomCode(): String = "%06d".format(rng.nextInt(1_000_000))

    fun label(s: String): ByteArray = s.toByteArray(Charsets.US_ASCII)

    fun sha256(vararg parts: ByteArray): ByteArray {
        val md = MessageDigest.getInstance("SHA-256")
        parts.forEach { md.update(it) }
        return md.digest()
    }

    fun hmac(key: ByteArray, vararg parts: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        parts.forEach { mac.update(it) }
        return mac.doFinal()
    }

    fun same(a: ByteArray, b: ByteArray): Boolean = MessageDigest.isEqual(a, b)

    private fun fixed32(v: BigInteger): ByteArray {
        val raw = v.toByteArray()
        return when {
            raw.size == 32 -> raw
            raw.size > 32 -> raw.copyOfRange(raw.size - 32, raw.size)
            else -> ByteArray(32 - raw.size) + raw
        }
    }

    /** Par de chaves P-256 efêmero, usado só no pareamento. */
    class Ecdh {
        private val pair: KeyPair = KeyPairGenerator.getInstance("EC").run {
            initialize(ECGenParameterSpec("secp256r1"))
            generateKeyPair()
        }

        /** Ponto público não comprimido: 0x04 || X || Y. */
        fun publicBytes(): ByteArray {
            val w = (pair.public as ECPublicKey).w
            return byteArrayOf(4) + fixed32(w.affineX) + fixed32(w.affineY)
        }

        fun agree(peer: ByteArray): ByteArray {
            require(peer.size == 65 && peer[0] == 4.toByte()) { "Chave pública inválida" }
            val point = ECPoint(
                BigInteger(1, peer.copyOfRange(1, 33)),
                BigInteger(1, peer.copyOfRange(33, 65)),
            )
            val params = (pair.public as ECPublicKey).params
            val peerKey = KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(point, params))
            val ka = KeyAgreement.getInstance("ECDH")
            ka.init(pair.private)
            ka.doPhase(peerKey, true)
            return fixed32(BigInteger(1, ka.generateSecret()))
        }
    }
}

/**
 * AES-256-GCM com nonce de contador (4 bytes zero + u64). Cada direção tem sua própria
 * chave, então o contador nunca se repete para a mesma chave.
 */
class Aead(key: ByteArray) {
    private val key = SecretKeySpec(key, "AES")
    private val cipher = Cipher.getInstance("AES/GCM/NoPadding")
    private var counter = 0L

    private fun nonce(): ByteArray = ByteBuffer.allocate(12).putInt(0).putLong(counter++).array()

    /** Cifra [data] e devolve o quadro completo: [headerRoom] bytes livres + ciphertext + tag. */
    fun seal(type: Int, data: ByteArray, headerRoom: Int): ByteArray {
        cipher.init(Cipher.ENCRYPT_MODE, key, GCMParameterSpec(128, nonce()))
        cipher.updateAAD(byteArrayOf(type.toByte()))
        val out = ByteArray(headerRoom + data.size + 16)
        val n = cipher.doFinal(data, 0, data.size, out, headerRoom)
        check(n == data.size + 16)
        return out
    }

    fun open(type: Int, data: ByteArray): ByteArray {
        cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, nonce()))
        cipher.updateAAD(byteArrayOf(type.toByte()))
        return cipher.doFinal(data)
    }
}

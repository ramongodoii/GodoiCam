package com.godoi.godoican.media

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.view.Surface
import com.godoi.godoican.net.Proto
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Encoder H.264 de hardware alimentado por Surface, configurado para baixa latência:
 * sem B-frames, prioridade de tempo real e SPS/PPS repetidos em cada quadro-chave.
 */
class VideoEncoder(
    val width: Int,
    val height: Int,
    val fps: Int,
    bitrate: Int,
    private val onPacket: (ByteArray) -> Unit,
) {
    private val thread = HandlerThread("GodoiCan-encoder").apply { start() }
    private lateinit var codec: MediaCodec
    lateinit var inputSurface: Surface
        private set
    var bitrate = bitrate
        private set
    var codecName = ""
        private set

    private val callback = object : MediaCodec.Callback() {
        override fun onInputBufferAvailable(codec: MediaCodec, index: Int) = Unit

        override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
            try {
                val buf = codec.getOutputBuffer(index)
                if (buf != null && info.size > 0) onPacket(packet(buf, info))
                codec.releaseOutputBuffer(index, false)
            } catch (_: IllegalStateException) {
                // Encoder parado no meio do callback.
            }
        }

        override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) = Unit

        override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) = Unit
    }

    fun start() {
        try {
            configure(extras = true)
        } catch (_: Exception) {
            // Alguns encoders recusam as chaves opcionais; tenta só com o básico.
            codec.release()
            configure(extras = false)
        }
        inputSurface = codec.createInputSurface()
        codec.start()
    }

    private fun configure(extras: Boolean) {
        val info = CameraCaps.encoderInfo()
        codec = if (info != null) MediaCodec.createByCodecName(info.name)
        else MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        codecName = codec.name
        val caps = codec.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
        bitrate = caps.videoCapabilities.bitrateRange.clamp(bitrate)
        codec.setCallback(callback, Handler(thread.looper))
        codec.configure(format(caps, extras), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
    }

    private fun format(caps: MediaCodecInfo.CodecCapabilities, extras: Boolean): MediaFormat {
        val f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height)
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        f.setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
        f.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
        val enc = caps.encoderCapabilities
        f.setInteger(
            MediaFormat.KEY_BITRATE_MODE,
            if (enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR
            else MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR,
        )
        if (!extras) return f

        // Perfil High (melhor qualidade por bit) quando o encoder suporta.
        val high = caps.profileLevels.filter { it.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileHigh }
        if (high.isNotEmpty()) {
            f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileHigh)
            f.setInteger(MediaFormat.KEY_LEVEL, high.maxOf { it.level })
        }
        f.setInteger(MediaFormat.KEY_PRIORITY, 0)
        f.setInteger(MediaFormat.KEY_LATENCY, 1)
        if (Build.VERSION.SDK_INT >= 29) {
            f.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            f.setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
        }
        return f
    }

    private fun packet(buf: ByteBuffer, info: MediaCodec.BufferInfo): ByteArray {
        var flags = 0
        if (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0) flags = flags or Proto.VIDEO_KEY
        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) flags = flags or Proto.VIDEO_CONFIG
        val out = ByteArray(Proto.VIDEO_HEADER + info.size)
        ByteBuffer.wrap(out).order(ByteOrder.BIG_ENDIAN).put(flags.toByte()).putLong(info.presentationTimeUs)
        buf.position(info.offset)
        buf.limit(info.offset + info.size)
        buf.get(out, Proto.VIDEO_HEADER, info.size)
        return out
    }

    fun requestKeyframe() {
        try {
            codec.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } catch (_: IllegalStateException) {
        }
    }

    fun setBitrate(value: Int) {
        bitrate = value
        try {
            codec.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, value) })
        } catch (_: IllegalStateException) {
        }
    }

    fun stop() {
        try {
            codec.stop()
        } catch (_: Exception) {
        }
        try {
            codec.release()
        } catch (_: Exception) {
        }
        try {
            inputSurface.release()
        } catch (_: Exception) {
        }
        thread.quitSafely()
    }
}

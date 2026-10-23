package com.godoi.godoican.media

import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import kotlin.math.abs

/** Relatório em texto do que câmera e encoders deste aparelho suportam (Fase 0: diagnóstico). */
object DeviceReport {
    fun build(context: Context): String = buildString {
        appendLine("GodoiCan — relatório do aparelho")
        appendLine("Aparelho: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE}), Android ${Build.VERSION.RELEASE} / API ${Build.VERSION.SDK_INT}")
        if (Build.VERSION.SDK_INT >= 31) appendLine("SoC: ${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}")
        appendLine()
        cameras(context, this)
        appendLine()
        encoders(this)
    }

    private fun cameras(context: Context, out: StringBuilder) {
        val mgr = context.getSystemService(CameraManager::class.java)
        for (id in mgr.cameraIdList) {
            val c = try {
                mgr.getCameraCharacteristics(id)
            } catch (e: Exception) {
                out.appendLine("Câmera $id: erro ${e.message}")
                continue
            }
            val facing = when (c.get(CameraCharacteristics.LENS_FACING)) {
                CameraCharacteristics.LENS_FACING_BACK -> "traseira"
                CameraCharacteristics.LENS_FACING_FRONT -> "frontal"
                else -> "externa"
            }
            val level = when (c.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)) {
                CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY -> "LEGACY"
                CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED -> "LIMITED"
                CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_FULL -> "FULL"
                CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_3 -> "LEVEL_3"
                else -> "outro"
            }
            out.appendLine("== Câmera $id ($facing, nível $level, orientação ${c.get(CameraCharacteristics.SENSOR_ORIENTATION)}°)")
            if (Build.VERSION.SDK_INT >= 28 && c.physicalCameraIds.isNotEmpty())
                out.appendLine("  Câmeras físicas: ${c.physicalCameraIds.joinToString()}")
            c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.let { out.appendLine("  Focais: ${it.joinToString()} mm") }
            out.appendLine("  Zoom: ${CameraCaps.zoomRange(c)}")
            out.appendLine("  Faixas de FPS: ${c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)?.joinToString()}")
            out.appendLine("  Relógio: ${if (c.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME) "REALTIME" else "UNKNOWN"}")
            out.appendLine("  Estabilização digital: ${c.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES)?.joinToString()} • óptica: ${c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION)?.joinToString()}")
            out.appendLine("  Modos de foco: ${c.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES)?.joinToString()} • exposição ${c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)}")
            c.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)?.let { out.appendLine("  ISO: $it • tempo de exposição (ns): ${c.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)}") }
            val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: continue
            val sizes = map.getOutputSizes(SurfaceTexture::class.java)?.sortedByDescending { it.width * it.height } ?: emptyList()
            out.appendLine("  Saídas 16:9 (fps máx):")
            sizes.filter { abs(it.width.toFloat() / it.height - 16f / 9f) < 0.02f }.forEach {
                val ns = map.getOutputMinFrameDuration(SurfaceTexture::class.java, it)
                out.appendLine("    ${it.width}x${it.height} • ${if (ns > 0) "%.0f".format(1e9 / ns) else "?"}")
            }
            for (fps in listOf(30, 60)) out.appendLine("  Resoluções aceitas a $fps fps: ${CameraCaps.resolutions(c, fps).joinToString()}")
        }
    }

    private fun encoders(out: StringBuilder) {
        val infos = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
        for (mime in listOf(MediaFormat.MIMETYPE_VIDEO_AVC, MediaFormat.MIMETYPE_VIDEO_HEVC, MediaFormat.MIMETYPE_VIDEO_AV1)) {
            for (info in infos.filter { i -> i.isEncoder && i.supportedTypes.any { it.equals(mime, true) } }) {
                val caps = info.getCapabilitiesForType(mime)
                val v = caps.videoCapabilities
                val e = caps.encoderCapabilities
                val hw = if (Build.VERSION.SDK_INT >= 29) (if (info.isHardwareAccelerated) "hardware" else "software") else "?"
                out.appendLine("== Encoder ${info.name} ($mime, $hw)")
                out.appendLine("  Bitrate: ${v.bitrateRange} • modos: " + listOf(
                    "CQ" to MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ,
                    "VBR" to MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR,
                    "CBR" to MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR,
                ).filter { e.isBitrateModeSupported(it.second) }.joinToString { it.first })
                out.appendLine("  Tamanho máx: ${v.supportedWidths.upper}x${v.supportedHeights.upper}")
                for ((w, h) in listOf(1280 to 720, 1920 to 1080, 2560 to 1440, 3840 to 2160)) {
                    val rates = try {
                        v.getSupportedFrameRatesFor(w, h).upper.toInt().toString()
                    } catch (_: Exception) {
                        "não"
                    }
                    out.appendLine("    ${w}x$h até $rates fps")
                }
                val features = buildList {
                    if (caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_IntraRefresh)) add("intra-refresh")
                    if (Build.VERSION.SDK_INT >= 30 && caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)) add("low-latency")
                    if (Build.VERSION.SDK_INT >= 31 && caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_QpBounds)) add("qp-bounds")
                }
                out.appendLine("  Recursos: ${features.ifEmpty { listOf("nenhum extra") }.joinToString()}")
                out.appendLine("  Perfis: ${caps.profileLevels.map { it.profile }.distinct().joinToString()}")
            }
        }
    }
}

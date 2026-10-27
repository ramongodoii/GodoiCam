package com.godoi.godoicam.media

import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.util.Range
import android.util.Size
import kotlin.math.abs

/** Consulta, em tempo de execução, o que câmera e encoder deste aparelho realmente suportam. */
object CameraCaps {
    data class CamInfo(val id: String, val facing: Int, val label: String, val sensorOrientation: Int)

    data class Resolution(val width: Int, val height: Int) {
        override fun toString() = when (height) {
            720 -> "720p (1280×720)"
            1080 -> "1080p (1920×1080)"
            1440 -> "1440p (2560×1440)"
            2160 -> "4K (3840×2160)"
            else -> "${width}×$height"
        }
    }

    val CANDIDATES = listOf(
        Resolution(1280, 720),
        Resolution(1920, 1080),
        Resolution(2560, 1440),
        Resolution(3840, 2160),
    )

    fun cameras(manager: CameraManager): List<CamInfo> {
        var back = 0
        var front = 0
        var other = 0
        return manager.cameraIdList.mapNotNull { id ->
            val c = try {
                manager.getCameraCharacteristics(id)
            } catch (_: Exception) {
                return@mapNotNull null
            }
            val caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
            if (CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE !in caps) return@mapNotNull null
            val facing = c.get(CameraCharacteristics.LENS_FACING) ?: CameraCharacteristics.LENS_FACING_EXTERNAL
            val label = when (facing) {
                CameraCharacteristics.LENS_FACING_BACK -> if (back++ == 0) "Traseira" else "Traseira $back"
                CameraCharacteristics.LENS_FACING_FRONT -> if (front++ == 0) "Frontal" else "Frontal $front"
                else -> if (other++ == 0) "Externa" else "Externa $other"
            }
            CamInfo(id, facing, label, c.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0)
        }
    }

    private fun fpsRanges(c: CameraCharacteristics): Array<Range<Int>> =
        c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: arrayOf(Range(30, 30))

    fun supportedFps(c: CameraCharacteristics): List<Int> =
        listOf(24, 30, 60).filter { f -> fpsRanges(c).any { it.upper == f } }.ifEmpty { listOf(30) }

    /** Faixa de FPS da câmera. Com [lowLight], deixa o FPS cair para expor mais em ambiente escuro. */
    fun fpsRange(c: CameraCharacteristics, fps: Int, lowLight: Boolean): Range<Int> {
        val matching = fpsRanges(c).filter { it.upper == fps }
        if (matching.isEmpty()) return fpsRanges(c).maxBy { it.upper }
        return if (lowLight) matching.minBy { it.lower } else matching.maxBy { it.lower }
    }

    private fun is16by9(s: Size) = abs(s.width.toFloat() / s.height - 16f / 9f) < 0.02f

    /**
     * Menor saída 16:9 da câmera que cobre a resolução pedida no FPS pedido.
     *
     * Com [coverPortrait], sobe um degrau (1440p para 1080p) para o recorte em pé não
     * ampliar 1,78×. Não sobe até 4K: no S24 isso dobrou a codificação (73→150 ms) e,
     * somado a filtro na GPU, derrubou o FPS enviado.
     */
    fun cameraSize(c: CameraCharacteristics, width: Int, height: Int, fps: Int, coverPortrait: Boolean = false): Size {
        val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)!!
        val all = map.getOutputSizes(SurfaceTexture::class.java)?.toList() ?: emptyList()
        val frameNs = 1_000_000_000L / fps
        val usable = all.filter { is16by9(it) && map.getOutputMinFrameDuration(SurfaceTexture::class.java, it) <= frameNs }
            .ifEmpty { all.filter { is16by9(it) } }
            .ifEmpty { all }
        val covering = usable.filter { it.width >= width && it.height >= height }
            .map { it.width to it.height }
        val picked = pickCameraSize(covering, width, height, coverPortrait)
            ?: (usable.maxBy { it.width * it.height }.let { it.width to it.height })
        return Size(picked.first, picked.second)
    }

    /**
     * Escolhe (largura, altura) dentro de [covering] (já filtradas: cobrem a saída).
     * No máximo ~2,25× pixels da saída — 1440p para 1080p, 1080p para 720p; 4K fica de fora.
     */
    fun pickCameraSize(
        covering: List<Pair<Int, Int>>,
        width: Int,
        height: Int,
        coverPortrait: Boolean,
    ): Pair<Int, Int>? {
        if (covering.isEmpty()) return null
        val budget = width.toLong() * height * 9 / 4
        val within = covering.filter { it.first.toLong() * it.second <= budget }.ifEmpty { covering }
        if (!coverPortrait) return within.minBy { it.first.toLong() * it.second }
        val needH = minOf(width, height * 4 / 3)
        return within.filter { it.second >= needH }.minByOrNull { it.first.toLong() * it.second }
            ?: within.minBy { it.first.toLong() * it.second }
    }

    /** Encoder H.264 preferido (consultado uma vez: montar a MediaCodecList é lento). */
    fun encoderInfo(): MediaCodecInfo? = cachedEncoder

    private val cachedEncoder: MediaCodecInfo? by lazy { findEncoder() }

    /** Hardware primeiro, software só se não houver outro. */
    private fun findEncoder(): MediaCodecInfo? {
        val encoders = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { info ->
            info.isEncoder && info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) }
        }
        fun isHardware(i: MediaCodecInfo) =
            if (Build.VERSION.SDK_INT >= 29) i.isHardwareAccelerated
            else !i.name.startsWith("OMX.google.") && !i.name.startsWith("c2.android.")
        return encoders.firstOrNull(::isHardware) ?: encoders.firstOrNull()
    }

    private val resolutionCache = HashMap<String, List<Resolution>>()

    /** Resoluções que câmera e encoder aguentam no FPS pedido (guardado em cache por câmera). */
    fun resolutions(c: CameraCharacteristics, fps: Int): List<Resolution> =
        synchronized(resolutionCache) {
            // O objeto CameraCharacteristics muda a cada consulta; a chave usa o conteúdo.
            val key = "${c.get(CameraCharacteristics.LENS_FACING)}|" +
                "${c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)}|$fps"
            resolutionCache.getOrPut(key) { computeResolutions(c, fps) }
        }

    private fun computeResolutions(c: CameraCharacteristics, fps: Int): List<Resolution> {
        val video = encoderInfo()?.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)?.videoCapabilities
        return CANDIDATES.filter { r ->
            val cam = cameraSize(c, r.width, r.height, fps)
            val camOk = cam.width >= r.width && cam.height >= r.height
            val encOk = video?.let {
                try {
                    it.areSizeAndRateSupported(r.width, r.height, fps.toDouble())
                } catch (_: Exception) {
                    false
                }
            } ?: (r.height <= 1080)
            camOk && encOk
        }.ifEmpty { listOf(CANDIDATES.first()) }
    }

    /**
     * Controles manuais (modo Pro) desta câmera; null = indisponível.
     * [focusDiopters]: foco mais próximo em dioptrias (1/m), 0 = foco fixo.
     */
    data class Manual(
        val iso: Range<Int>?,
        val exposureNs: Range<Long>?,
        val focusDiopters: Float,
        val focusCalibrated: Boolean,
        val kelvin: Boolean,
    )

    fun manual(c: CameraCharacteristics): Manual {
        val caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
        val sensor = CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR in caps
        val afModes = c.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: intArrayOf()
        val minFocus = c.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
        val calibration = c.get(CameraCharacteristics.LENS_INFO_FOCUS_DISTANCE_CALIBRATION)
        return Manual(
            iso = if (sensor) c.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE) else null,
            exposureNs = if (sensor) c.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) else null,
            focusDiopters = if (sensor && CameraCharacteristics.CONTROL_AF_MODE_OFF in afModes) minFocus else 0f,
            focusCalibrated = calibration != null &&
                calibration != CameraCharacteristics.LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED,
            kelvin = WhiteBalance.supported(c),
        )
    }

    fun zoomRange(c: CameraCharacteristics): Range<Float> {
        if (Build.VERSION.SDK_INT >= 30) {
            c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)?.let { return it }
        }
        return Range(1f, c.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1f)
    }
}

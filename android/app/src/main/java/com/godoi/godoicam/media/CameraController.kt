package com.godoi.godoicam.media

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.PointF
import android.graphics.Rect
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.ColorSpaceTransform
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.RggbChannelVector
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Range
import android.view.Surface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Camera2 com uma única saída (a SurfaceTexture do renderizador) e os controles manuais. */
class CameraController(context: Context, private val listener: Listener) {
    interface Listener {
        fun onCameraReady()
        fun onCameraError(message: String)
        /** Os ajustes mudaram (pela tela ou pelo PC). Chamado na thread da câmera. */
        fun onControlsChanged() {}
        /** Primeiro quadro depois de abrir a câmera. Chamado na thread da câmera. */
        fun onFirstFrame() {}
    }

    /** Ajustes que o usuário pode mudar com a câmera ligada. */
    data class Controls(
        val zoom: Float = 1f,
        val exposure: Int = 0,
        val awbMode: Int = CaptureRequest.CONTROL_AWB_MODE_AUTO,
        val torch: Boolean = false,
        val stabilization: Boolean = false,
        val aeLock: Boolean = false,
        /** Ponto de foco em coordenadas normalizadas do sensor; null = foco contínuo. */
        val focusPoint: PointF? = null,
        /** Modo Pro; null = automático. */
        val iso: Int? = null,
        val exposureNs: Long? = null,
        /** Foco manual em dioptrias (1/m; 0 = infinito). Tem prioridade sobre [focusPoint]. */
        val focusDiopters: Float? = null,
        /** Balanço de branco em Kelvin. Tem prioridade sobre [awbMode]. */
        val kelvin: Int? = null,
    ) {
        val manualExposure get() = iso != null || exposureNs != null
    }

    /** Valores que a câmera está usando agora (os automáticos, enquanto não são manuais). */
    data class Live(
        val iso: Int? = null,
        val exposureNs: Long? = null,
        val focusDiopters: Float? = null,
        val awbGains: RggbChannelVector? = null,
        val awbTransform: ColorSpaceTransform? = null,
    )

    private val manager = context.getSystemService(CameraManager::class.java)
    private val thread = HandlerThread("GodoiCam-camera").apply { start() }
    private val handler = Handler(thread.looper)

    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var builder: CaptureRequest.Builder? = null
    private var chars: CameraCharacteristics? = null
    private var generation = 0

    @Volatile var controls = Controls()
        private set
    @Volatile var live = Live()
        private set
    private var firstFramePending = false
    /** Duração de um quadro no FPS pedido: teto do tempo de exposição manual. */
    @Volatile var frameNs = 1_000_000_000L / 30
        private set

    private val results = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(s: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
            if (firstFramePending) {
                firstFramePending = false
                listener.onFirstFrame()
            }
            val ctl = controls
            val l = live
            // Só os valores automáticos interessam: viram o ponto de partida do modo manual.
            live = Live(
                iso = if (ctl.manualExposure) l.iso else result.get(CaptureResult.SENSOR_SENSITIVITY) ?: l.iso,
                exposureNs = if (ctl.manualExposure) l.exposureNs
                else result.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: l.exposureNs,
                focusDiopters = if (ctl.focusDiopters != null) l.focusDiopters
                else result.get(CaptureResult.LENS_FOCUS_DISTANCE) ?: l.focusDiopters,
                awbGains = if (ctl.kelvin != null) l.awbGains else result.get(CaptureResult.COLOR_CORRECTION_GAINS) ?: l.awbGains,
                awbTransform = if (ctl.kelvin != null && l.awbTransform != null) l.awbTransform
                else result.get(CaptureResult.COLOR_CORRECTION_TRANSFORM) ?: l.awbTransform,
            )
            // Kelvin salvo de antes: só dá para aplicar depois da primeira matriz de cor.
            if (ctl.kelvin != null && l.awbTransform == null && live.awbTransform != null) apply(triggerFocus = false)
        }
    }

    @SuppressLint("MissingPermission")
    fun open(cameraId: String, target: Surface, fpsRange: Range<Int>, initial: Controls) {
        handler.post {
            closeInternal()
            val gen = ++generation
            controls = initial
            live = Live()
            firstFramePending = true
            frameNs = 1_000_000_000L / maxOf(1, fpsRange.upper)
            try {
                chars = manager.getCameraCharacteristics(cameraId)
                manager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                    override fun onOpened(d: CameraDevice) {
                        if (gen != generation) {
                            d.close()
                            return
                        }
                        device = d
                        startSession(d, target, fpsRange, gen)
                    }

                    override fun onDisconnected(d: CameraDevice) {
                        d.close()
                        if (device === d) {
                            device = null
                            listener.onCameraError("A câmera foi tomada por outro app.")
                        }
                    }

                    override fun onError(d: CameraDevice, error: Int) {
                        d.close()
                        if (device === d || device == null) {
                            device = null
                            listener.onCameraError(
                                when (error) {
                                    ERROR_CAMERA_IN_USE, ERROR_MAX_CAMERAS_IN_USE -> "A câmera está em uso por outro app."
                                    ERROR_CAMERA_DISABLED -> "A câmera está bloqueada por uma política do aparelho."
                                    else -> "Erro na câmera ($error)."
                                },
                            )
                        }
                    }
                }, handler)
            } catch (e: Exception) {
                listener.onCameraError("Não foi possível abrir a câmera: ${e.message}")
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun startSession(d: CameraDevice, target: Surface, fpsRange: Range<Int>, gen: Int) {
        try {
            // RECORD: no S24 o PREVIEW não reduziu o atraso (~98 ms contra ~86 ms) e a imagem do
            // RECORD é melhor (docs/MEDICOES.md, 24/10/2026).
            val b = d.createCaptureRequest(CameraDevice.TEMPLATE_RECORD)
            b.addTarget(target)
            b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)
            // Nada de a câmera girar/recortar sozinha (modo de compatibilidade de telas grandes):
            // a rotação é feita na GPU.
            if (Build.VERSION.SDK_INT >= 31) {
                val modes = chars?.get(CameraCharacteristics.SCALER_AVAILABLE_ROTATE_AND_CROP_MODES) ?: intArrayOf()
                if (CaptureRequest.SCALER_ROTATE_AND_CROP_NONE in modes) {
                    b.set(CaptureRequest.SCALER_ROTATE_AND_CROP, CaptureRequest.SCALER_ROTATE_AND_CROP_NONE)
                }
            }
            builder = b
            d.createCaptureSession(listOf(target), object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    if (gen != generation) return
                    session = s
                    apply(triggerFocus = false)
                    listener.onCameraReady()
                }

                override fun onConfigureFailed(s: CameraCaptureSession) {
                    listener.onCameraError("A câmera recusou esta combinação de resolução e FPS.")
                }
            }, handler)
        } catch (e: Exception) {
            listener.onCameraError("Falha ao iniciar a câmera: ${e.message}")
        }
    }

    fun update(transform: (Controls) -> Controls) {
        handler.post {
            val before = controls
            controls = transform(before)
            if (controls == before) return@post
            apply(triggerFocus = controls.focusPoint != null && controls.focusPoint != before.focusPoint)
            listener.onControlsChanged()
        }
    }

    private fun apply(triggerFocus: Boolean) {
        val b = builder ?: return
        val s = session ?: return
        val c = chars ?: return
        val ctl = controls
        try {
            b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)

            val zoomRange = CameraCaps.zoomRange(c)
            val zoom = ctl.zoom.coerceIn(zoomRange.lower, zoomRange.upper)
            if (Build.VERSION.SDK_INT >= 30 && c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) != null) {
                b.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom)
            } else {
                b.set(CaptureRequest.SCALER_CROP_REGION, cropFor(c, zoom))
            }

            val manual = CameraCaps.manual(c)
            applyExposure(b, c, ctl, manual)

            val flash = c.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true
            b.set(
                CaptureRequest.FLASH_MODE,
                if (flash && ctl.torch) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF,
            )

            applyWhiteBalance(b, c, ctl, manual)

            val eis = c.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES) ?: intArrayOf()
            if (CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON in eis) {
                b.set(
                    CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                    if (ctl.stabilization) CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON
                    else CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF,
                )
            }
            val ois = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION) ?: intArrayOf()
            if (CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON in ois) {
                // Estabilização óptica não adiciona atraso, então fica sempre ligada.
                b.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
            }

            applyProcessing(b, c)

            val manualFocus = ctl.focusDiopters != null && manual.focusDiopters > 0f
            if (manualFocus) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                b.set(CaptureRequest.LENS_FOCUS_DISTANCE, ctl.focusDiopters!!.coerceIn(0f, manual.focusDiopters))
            } else {
                applyFocus(b, c, ctl.focusPoint, zoom)
            }
            s.setRepeatingRequest(b.build(), results, handler)

            if (triggerFocus && ctl.focusPoint != null && !manualFocus) {
                b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
                s.capture(b.build(), null, handler)
                b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
            }
        } catch (e: Exception) {
            listener.onCameraError("Falha ao aplicar ajustes: ${e.message}")
        }
    }

    /**
     * O Camera2 não tem "ISO manual com obturador automático": com qualquer um dos dois manual,
     * a exposição automática desliga e o outro fica no último valor automático.
     */
    private fun applyExposure(b: CaptureRequest.Builder, c: CameraCharacteristics, ctl: Controls, manual: CameraCaps.Manual) {
        val isoRange = manual.iso
        val expRange = manual.exposureNs
        if (ctl.manualExposure && isoRange != null && expRange != null) {
            val l = live
            b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
            b.set(CaptureRequest.SENSOR_SENSITIVITY, (ctl.iso ?: l.iso ?: DEFAULT_ISO).coerceIn(isoRange.lower, isoRange.upper))
            val maxExposure = maxOf(expRange.lower, minOf(expRange.upper, frameNs))
            b.set(CaptureRequest.SENSOR_EXPOSURE_TIME, (ctl.exposureNs ?: l.exposureNs ?: (frameNs / 2)).coerceIn(expRange.lower, maxExposure))
            b.set(CaptureRequest.SENSOR_FRAME_DURATION, frameNs)
            return
        }
        b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
        val evRange = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE) ?: Range(0, 0)
        b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, ctl.exposure.coerceIn(evRange.lower, evRange.upper))
        b.set(CaptureRequest.CONTROL_AE_LOCK, ctl.aeLock)
    }

    private fun applyWhiteBalance(b: CaptureRequest.Builder, c: CameraCharacteristics, ctl: Controls, manual: CameraCaps.Manual) {
        val kelvin = ctl.kelvin
        val transform = live.awbTransform
        val gains = if (kelvin != null && manual.kelvin && transform != null) WhiteBalance.gains(c, kelvin) else null
        if (gains != null) {
            b.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_OFF)
            b.set(CaptureRequest.COLOR_CORRECTION_MODE, CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX)
            b.set(CaptureRequest.COLOR_CORRECTION_GAINS, gains)
            b.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, transform)
            return
        }
        val awbModes = c.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES) ?: intArrayOf()
        b.set(
            CaptureRequest.CONTROL_AWB_MODE,
            if (ctl.awbMode in awbModes) ctl.awbMode else CaptureRequest.CONTROL_AWB_MODE_AUTO,
        )
    }

    /**
     * Processamento de vídeo barato: NR FAST (não HIGH_QUALITY) e nitidez FAST.
     * HIGH_QUALITY e filtro na GPU foram testados no S24 e travaram o envio.
     */
    private fun applyProcessing(b: CaptureRequest.Builder, c: CameraCharacteristics) {
        // FAST: feito para vídeo. HIGH_QUALITY no S24 em 4K/1440p soma atraso no sensor.
        setPreferred(
            b, CaptureRequest.NOISE_REDUCTION_MODE,
            c.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES),
            CaptureRequest.NOISE_REDUCTION_MODE_FAST,
        )
        setPreferred(
            b, CaptureRequest.HOT_PIXEL_MODE,
            c.get(CameraCharacteristics.HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES),
            CaptureRequest.HOT_PIXEL_MODE_FAST,
        )
        setPreferred(
            b, CaptureRequest.EDGE_MODE,
            c.get(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES),
            CaptureRequest.EDGE_MODE_FAST,
            CaptureRequest.EDGE_MODE_OFF,
        )
    }

    private fun setPreferred(
        b: CaptureRequest.Builder,
        key: CaptureRequest.Key<Int>,
        available: IntArray?,
        vararg preferred: Int,
    ) {
        val pick = preferred.firstOrNull { available != null && it in available } ?: return
        b.set(key, pick)
    }

    private fun applyFocus(b: CaptureRequest.Builder, c: CameraCharacteristics, point: PointF?, zoom: Float) {
        val afModes = c.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: intArrayOf()
        val maxAf = c.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) ?: 0
        val maxAe = c.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AE) ?: 0
        if (point == null || CaptureRequest.CONTROL_AF_MODE_AUTO !in afModes) {
            if (CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO in afModes) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
            }
            if (maxAf > 0) b.set(CaptureRequest.CONTROL_AF_REGIONS, null)
            if (maxAe > 0) b.set(CaptureRequest.CONTROL_AE_REGIONS, null)
            return
        }
        val region = arrayOf(meteringRect(c, point, zoom))
        b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
        if (maxAf > 0) b.set(CaptureRequest.CONTROL_AF_REGIONS, region)
        if (maxAe > 0) b.set(CaptureRequest.CONTROL_AE_REGIONS, region)
    }

    private fun cropFor(c: CameraCharacteristics, zoom: Float): Rect {
        val a = c.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)!!
        val w = (a.width() / zoom).toInt()
        val h = (a.height() / zoom).toInt()
        val l = (a.width() - w) / 2
        val t = (a.height() - h) / 2
        return Rect(l, t, l + w, t + h)
    }

    private fun meteringRect(c: CameraCharacteristics, p: PointF, zoom: Float): MeteringRectangle {
        val a = c.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)!!
        // Com CONTROL_ZOOM_RATIO as regiões já são relativas ao campo após o zoom.
        val field = if (Build.VERSION.SDK_INT >= 30 && c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) != null) {
            Rect(0, 0, a.width(), a.height())
        } else {
            cropFor(c, zoom)
        }
        // A saída 16:9 corta a faixa central de um sensor 4:3.
        val visibleH = minOf(field.height(), field.width() * 9 / 16)
        val top = field.top + (field.height() - visibleH) / 2
        val cx = field.left + (p.x * field.width()).toInt()
        val cy = top + (p.y * visibleH).toInt()
        val half = maxOf(field.width(), visibleH) / 12
        val rect = Rect(cx - half, cy - half, cx + half, cy + half)
        rect.intersect(Rect(0, 0, a.width() - 1, a.height() - 1))
        return MeteringRectangle(rect, MeteringRectangle.METERING_WEIGHT_MAX - 1)
    }

    private fun closeInternal() {
        generation++
        try {
            session?.close()
        } catch (_: Exception) {
        }
        try {
            device?.close()
        } catch (_: Exception) {
        }
        session = null
        device = null
        builder = null
    }

    fun close() {
        val latch = CountDownLatch(1)
        handler.post {
            closeInternal()
            latch.countDown()
        }
        latch.await(3, TimeUnit.SECONDS)
    }

    fun release() {
        close()
        thread.quitSafely()
    }

    private companion object {
        const val DEFAULT_ISO = 100
    }
}

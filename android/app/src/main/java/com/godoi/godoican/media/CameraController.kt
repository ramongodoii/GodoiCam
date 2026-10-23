package com.godoi.godoican.media

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.PointF
import android.graphics.Rect
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.MeteringRectangle
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
    )

    private val manager = context.getSystemService(CameraManager::class.java)
    private val thread = HandlerThread("GodoiCan-camera").apply { start() }
    private val handler = Handler(thread.looper)

    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var builder: CaptureRequest.Builder? = null
    private var chars: CameraCharacteristics? = null
    private var generation = 0

    @Volatile var controls = Controls()
        private set

    @SuppressLint("MissingPermission")
    fun open(cameraId: String, target: Surface, fpsRange: Range<Int>, initial: Controls) {
        handler.post {
            closeInternal()
            val gen = ++generation
            controls = initial
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
            val b = d.createCaptureRequest(CameraDevice.TEMPLATE_RECORD)
            b.addTarget(target)
            b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)
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
            apply(triggerFocus = controls.focusPoint != null && controls.focusPoint != before.focusPoint)
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

            b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
            val evRange = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE) ?: Range(0, 0)
            b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, ctl.exposure.coerceIn(evRange.lower, evRange.upper))
            b.set(CaptureRequest.CONTROL_AE_LOCK, ctl.aeLock)

            val flash = c.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true
            b.set(
                CaptureRequest.FLASH_MODE,
                if (flash && ctl.torch) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF,
            )

            val awbModes = c.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES) ?: intArrayOf()
            b.set(
                CaptureRequest.CONTROL_AWB_MODE,
                if (ctl.awbMode in awbModes) ctl.awbMode else CaptureRequest.CONTROL_AWB_MODE_AUTO,
            )

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

            applyFocus(b, c, ctl.focusPoint, zoom)
            s.setRepeatingRequest(b.build(), null, handler)

            if (triggerFocus && ctl.focusPoint != null) {
                b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
                s.capture(b.build(), null, handler)
                b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
            }
        } catch (e: Exception) {
            listener.onCameraError("Falha ao aplicar ajustes: ${e.message}")
        }
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
}

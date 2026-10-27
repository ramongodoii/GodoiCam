package com.godoi.godoicam

import android.content.Context
import android.hardware.camera2.CaptureRequest
import com.godoi.godoicam.media.CameraController

/** Preferências do usuário, salvas entre execuções. */
class AppSettings(context: Context) {
    private val p = context.getSharedPreferences("settings", Context.MODE_PRIVATE)

    init {
        // Desfaz o "pouca luz" forçado: no S24 o FPS alvo caiu para 10 e a imagem travou.
        if (p.getBoolean("noiseFix1", false) && !p.getBoolean("noiseFix2", false)) {
            p.edit().putBoolean("lowLight", false).putBoolean("noiseFix2", true).apply()
        }
    }

    var cameraId: String?
        get() = p.getString("cameraId", null)
        set(v) = p.edit().putString("cameraId", v).apply()
    var width: Int
        get() = p.getInt("width", 1920)
        set(v) = p.edit().putInt("width", v).apply()
    var height: Int
        get() = p.getInt("height", 1080)
        set(v) = p.edit().putInt("height", v).apply()
    var fps: Int
        get() = p.getInt("fps", 30)
        set(v) = p.edit().putInt("fps", v).apply()
    /** 0 econômica, 1 normal, 2 alta, 3 máxima. */
    var quality: Int
        get() = p.getInt("quality", 2)
        set(v) = p.edit().putInt("quality", v).apply()
    var mirror: Boolean
        get() = p.getBoolean("mirror", false)
        set(v) = p.edit().putBoolean("mirror", v).apply()
    var fill: Boolean
        get() = p.getBoolean("fill2", true)
        set(v) = p.edit().putBoolean("fill2", v).apply()
    var autoRotate: Boolean
        get() = p.getBoolean("autoRotate", true)
        set(v) = p.edit().putBoolean("autoRotate", v).apply()
    /** Correção adicional de rotação em graus (0, 90, 180, 270). */
    var rotationOffset: Int
        get() = p.getInt("rotationOffset", 0)
        set(v) = p.edit().putInt("rotationOffset", v).apply()
    var stabilization: Boolean
        get() = p.getBoolean("stabilization", false)
        set(v) = p.edit().putBoolean("stabilization", v).apply()
    var lowLight: Boolean
        get() = p.getBoolean("lowLight", false)
        set(v) = p.edit().putBoolean("lowLight", v).apply()
    var exposure: Int
        get() = p.getInt("exposure", 0)
        set(v) = p.edit().putInt("exposure", v).apply()
    var awbMode: Int
        get() = p.getInt("awbMode", CaptureRequest.CONTROL_AWB_MODE_AUTO)
        set(v) = p.edit().putInt("awbMode", v).apply()
    var zoom: Float
        get() = p.getFloat("zoom", 1f)
        set(v) = p.edit().putFloat("zoom", v).apply()

    // Modo Pro; null = automático.
    var iso: Int?
        get() = p.getInt("iso", 0).takeIf { it > 0 }
        set(v) = p.edit().putInt("iso", v ?: 0).apply()
    var exposureNs: Long?
        get() = p.getLong("exposureNs", 0).takeIf { it > 0 }
        set(v) = p.edit().putLong("exposureNs", v ?: 0).apply()
    var focusDiopters: Float?
        get() = p.getFloat("focusDiopters", -1f).takeIf { it >= 0 }
        set(v) = p.edit().putFloat("focusDiopters", v ?: -1f).apply()
    var kelvin: Int?
        get() = p.getInt("kelvin", 0).takeIf { it > 0 }
        set(v) = p.edit().putInt("kelvin", v ?: 0).apply()

    /** Grava os ajustes de câmera que sobrevivem a reiniciar o app. */
    fun save(c: CameraController.Controls) {
        zoom = c.zoom
        exposure = c.exposure
        awbMode = c.awbMode
        iso = c.iso
        exposureNs = c.exposureNs
        focusDiopters = c.focusDiopters
        kelvin = c.kelvin
    }

    fun bitrate(): Int = bitrateFor(width, height, fps, quality)

    companion object {
        val QUALITY_NAMES = listOf("Econômica", "Normal", "Alta", "Máxima")

        fun bitrateFor(width: Int, height: Int, fps: Int, quality: Int): Int {
            // 1080p30: ~5 / 8,7 / 14,9 / 22,4 Mbps. O antigo "Alta" (~10 Mbps) deixava
            // granulado de compressão no rosto; o Wi-Fi 5 GHz e o USB aguentam o novo valor.
            val bitsPerPixel = floatArrayOf(0.08f, 0.14f, 0.24f, 0.36f)[quality.coerceIn(0, 3)]
            return (width.toLong() * height * fps * bitsPerPixel).toLong().coerceIn(500_000, 80_000_000).toInt()
        }
    }
}

package com.godoi.godoican

import android.content.Context
import android.hardware.camera2.CaptureRequest

/** Preferências do usuário, salvas entre execuções. */
class AppSettings(context: Context) {
    private val p = context.getSharedPreferences("settings", Context.MODE_PRIVATE)

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
        get() = p.getBoolean("fill", false)
        set(v) = p.edit().putBoolean("fill", v).apply()
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

    fun bitrate(): Int = bitrateFor(width, height, fps, quality)

    companion object {
        val QUALITY_NAMES = listOf("Econômica", "Normal", "Alta", "Máxima")

        fun bitrateFor(width: Int, height: Int, fps: Int, quality: Int): Int {
            val bitsPerPixel = floatArrayOf(0.06f, 0.10f, 0.16f, 0.25f)[quality.coerceIn(0, 3)]
            return (width.toLong() * height * fps * bitsPerPixel).toLong().coerceIn(500_000, 80_000_000).toInt()
        }
    }
}

package com.godoi.godoicam.media

import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.params.ColorSpaceTransform
import android.hardware.camera2.params.RggbChannelVector
import android.util.Rational
import kotlin.math.ln

/**
 * Balanço de branco por temperatura de cor (Kelvin) sem a API de CCT do Android 16: os ganhos
 * R/G/B saem da calibração de cor do sensor (as mesmas matrizes de um DNG). Para uma
 * temperatura K, o branco de K no espaço XYZ é levado ao espaço da câmera e os ganhos são o
 * inverso desse "neutro da câmera".
 */
object WhiteBalance {
    const val MIN_KELVIN = 2000
    const val MAX_KELVIN = 10000

    private class Calibration(val kelvin: Double, val matrix: DoubleArray)

    /** O aparelho aceita ganhos manuais e informa a calibração de cor do sensor. */
    fun supported(c: CameraCharacteristics): Boolean {
        val caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: return false
        return CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING in caps &&
            calibrations(c).isNotEmpty()
    }

    /** Ganhos para iluminar a cena como luz de [kelvin]; null se o sensor não tem calibração. */
    fun gains(c: CameraCharacteristics, kelvin: Int): RggbChannelVector? {
        val n = cameraNeutral(c, kelvin.toDouble()) ?: return null
        if (n[0] <= 0 || n[1] <= 0 || n[2] <= 0) return null
        val r = (n[1] / n[0]).coerceIn(MIN_GAIN, MAX_GAIN).toFloat()
        val b = (n[1] / n[2]).coerceIn(MIN_GAIN, MAX_GAIN).toFloat()
        return RggbChannelVector(r, 1f, 1f, b)
    }

    /** Temperatura que mais se aproxima dos ganhos escolhidos pelo balanço automático. */
    fun estimateKelvin(c: CameraCharacteristics, auto: RggbChannelVector): Int? {
        val g = (auto.greenEven + auto.greenOdd) / 2
        if (g <= 0 || auto.red <= 0 || auto.blue <= 0) return null
        val r = auto.red / g
        val b = auto.blue / g
        var best: Int? = null
        var bestErr = Double.MAX_VALUE
        for (k in MIN_KELVIN..MAX_KELVIN step 50) {
            val v = gains(c, k) ?: return null
            val dr = ln(r / v.red.toDouble())
            val db = ln(b / v.blue.toDouble())
            val err = dr * dr + db * db
            if (err < bestErr) {
                bestErr = err
                best = k
            }
        }
        return best
    }

    private fun cameraNeutral(c: CameraCharacteristics, kelvin: Double): DoubleArray? {
        val cal = calibrations(c)
        if (cal.isEmpty()) return null
        // Interpola as duas calibrações pelo inverso da temperatura, como no DNG.
        val m = if (cal.size == 1) cal[0].matrix else {
            val (a, b) = cal[0] to cal[1]
            val w = ((1 / kelvin - 1 / b.kelvin) / (1 / a.kelvin - 1 / b.kelvin)).coerceIn(0.0, 1.0)
            DoubleArray(9) { a.matrix[it] * w + b.matrix[it] * (1 - w) }
        }
        val xyz = whiteXyz(kelvin)
        return DoubleArray(3) { row -> (0 until 3).sumOf { m[row * 3 + it] * xyz[it] } }
    }

    /** Calibrações (temperatura do iluminante, calibração × matriz de cor) do sensor. */
    private fun calibrations(c: CameraCharacteristics): List<Calibration> {
        val list = mutableListOf<Calibration>()
        val k1 = illuminantKelvin(c.get(CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1))
        val cm1 = c.get(CameraCharacteristics.SENSOR_COLOR_TRANSFORM1)
        if (k1 != null && cm1 != null) {
            list += Calibration(k1, multiply(c.get(CameraCharacteristics.SENSOR_CALIBRATION_TRANSFORM1), cm1))
        }
        val k2 = illuminantKelvin(c.get(CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT2)?.toInt())
        val cm2 = c.get(CameraCharacteristics.SENSOR_COLOR_TRANSFORM2)
        if (k2 != null && cm2 != null && k2 != k1) {
            list += Calibration(k2, multiply(c.get(CameraCharacteristics.SENSOR_CALIBRATION_TRANSFORM2), cm2))
        }
        return list
    }

    private fun toArray(t: ColorSpaceTransform): DoubleArray {
        val r = arrayOfNulls<Rational>(9)
        t.copyElements(r, 0)
        return DoubleArray(9) { r[it]?.toDouble() ?: 0.0 }
    }

    /** cc × cm (cc ausente = identidade). */
    private fun multiply(cc: ColorSpaceTransform?, cm: ColorSpaceTransform): DoubleArray {
        val b = toArray(cm)
        if (cc == null) return b
        val a = toArray(cc)
        return DoubleArray(9) { i ->
            val row = i / 3
            val col = i % 3
            (0 until 3).sumOf { a[row * 3 + it] * b[it * 3 + col] }
        }
    }

    /** Branco do corpo negro em [kelvin], em XYZ com Y = 1 (aproximação de Kim et al.). */
    private fun whiteXyz(kelvin: Double): DoubleArray {
        val t = kelvin.coerceIn(1667.0, 25000.0)
        val x = if (t <= 4000) {
            -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t) + 0.8776956e3 / t + 0.179910
        } else {
            -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t) + 0.2226347e3 / t + 0.240390
        }
        val y = when {
            t <= 2222 -> -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683
            t <= 4000 -> -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867
            else -> 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483
        }
        return doubleArrayOf(x / y, 1.0, (1 - x - y) / y)
    }

    private fun illuminantKelvin(code: Int?): Double? = when (code) {
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_STANDARD_A,
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_TUNGSTEN -> 2856.0
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_ISO_STUDIO_TUNGSTEN -> 3200.0
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_WHITE_FLUORESCENT -> 3450.0
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_FLUORESCENT,
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_COOL_WHITE_FLUORESCENT -> 4150.0
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_STANDARD_B -> 4874.0
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_DAY_WHITE_FLUORESCENT,
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_D50 -> 5003.0
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_DAYLIGHT,
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_FLASH,
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_FINE_WEATHER,
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_D55 -> 5503.0
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_DAYLIGHT_FLUORESCENT -> 6430.0
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_CLOUDY_WEATHER,
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_D65 -> 6504.0
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_STANDARD_C -> 6774.0
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_SHADE,
        CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_D75 -> 7504.0
        else -> null
    }

    private const val MIN_GAIN = 0.25
    private const val MAX_GAIN = 8.0
}

package com.godoi.godoicam

import android.util.Range
import com.godoi.godoicam.media.WhiteBalance
import kotlin.math.abs
import kotlin.math.ln
import kotlin.math.pow
import kotlin.math.roundToInt

/** Conversão entre a posição das barras do modo Pro e os valores da câmera. */
object ProScale {
    const val ISO_STEPS = 100
    const val FOCUS_STEPS = 100
    const val KELVIN_STEP = 100
    val KELVIN_STEPS = (WhiteBalance.MAX_KELVIN - WhiteBalance.MIN_KELVIN) / KELVIN_STEP

    /** Velocidades usuais de obturador (1/x s), da mais rápida à mais lenta. */
    private val SHUTTER_DENOMINATORS = intArrayOf(
        8000, 6400, 5000, 4000, 3200, 2500, 2000, 1600, 1250, 1000, 800, 640, 500, 400, 320,
        250, 200, 160, 125, 100, 80, 60, 50, 40, 30, 25, 20, 15, 10, 8,
    )

    /** ISO em escala logarítmica: cada trecho da barra dobra o ganho por igual. */
    fun isoAt(progress: Int, range: Range<Int>): Int {
        val t = progress.coerceIn(0, ISO_STEPS) / ISO_STEPS.toDouble()
        return (range.lower * (range.upper.toDouble() / range.lower).pow(t)).roundToInt().coerceIn(range.lower, range.upper)
    }

    fun isoProgress(iso: Int, range: Range<Int>): Int {
        if (range.upper <= range.lower) return 0
        val t = ln(iso.coerceIn(range.lower, range.upper).toDouble() / range.lower) / ln(range.upper.toDouble() / range.lower)
        return (t * ISO_STEPS).roundToInt()
    }

    /** Tempos de exposição (ns, crescentes) que a câmera aceita sem baixar o FPS ([maxNs]). */
    fun shutterStops(range: Range<Long>, maxNs: Long): List<Long> {
        val top = minOf(range.upper, maxNs)
        return SHUTTER_DENOMINATORS.map { 1_000_000_000L / it }.filter { it in range.lower..top }
            .ifEmpty { listOf(top.coerceAtLeast(range.lower)) }
    }

    fun nearestStop(stops: List<Long>, ns: Long): Int =
        stops.indices.minByOrNull { abs(ln(stops[it].toDouble() / ns.coerceAtLeast(1))) } ?: 0

    fun shutterLabel(ns: Long): String {
        val seconds = ns / 1e9
        return if (seconds >= 0.3) "%.1f s".format(seconds).replace('.', ',') else "1/${(1 / seconds).roundToInt()} s"
    }

    /** Barra da esquerda (mais perto) para a direita (infinito). */
    fun focusAt(progress: Int, maxDiopters: Float): Float =
        (1f - progress.coerceIn(0, FOCUS_STEPS) / FOCUS_STEPS.toFloat()) * maxDiopters

    fun focusProgress(diopters: Float, maxDiopters: Float): Int =
        if (maxDiopters <= 0f) FOCUS_STEPS
        else ((1f - diopters.coerceIn(0f, maxDiopters) / maxDiopters) * FOCUS_STEPS).roundToInt()

    /** Distância em cm/m quando a lente é calibrada; senão só a posição relativa. */
    fun focusLabel(diopters: Float, maxDiopters: Float, calibrated: Boolean): String = when {
        diopters <= 0.01f -> "infinito"
        !calibrated -> "${(diopters / maxDiopters * 100).roundToInt()}% (perto = 100%)"
        diopters >= 1f -> "${(100 / diopters).roundToInt()} cm"
        else -> "%.1f m".format(1 / diopters).replace('.', ',')
    }

    fun kelvinAt(progress: Int): Int = WhiteBalance.MIN_KELVIN + progress.coerceIn(0, KELVIN_STEPS) * KELVIN_STEP

    fun kelvinProgress(kelvin: Int): Int =
        ((kelvin - WhiteBalance.MIN_KELVIN) / KELVIN_STEP).coerceIn(0, KELVIN_STEPS)
}

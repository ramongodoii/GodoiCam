package com.godoi.godoicam

import android.view.View
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import com.godoi.godoicam.media.CameraCaps
import com.google.android.material.button.MaterialButton
import com.google.android.material.materialswitch.MaterialSwitch

/**
 * Seção "Modo Pro" da folha de ajustes: ISO, obturador, foco e temperatura de cor. Cada
 * controle começa do valor que o automático está usando, para a imagem não dar salto.
 */
class ProPanel(private val root: View) {
    private val swIso: MaterialSwitch = root.findViewById(R.id.swIso)
    private val lblIso: TextView = root.findViewById(R.id.lblIso)
    private val sbIso: SeekBar = root.findViewById(R.id.sbIso)
    private val swShutter: MaterialSwitch = root.findViewById(R.id.swShutter)
    private val lblShutter: TextView = root.findViewById(R.id.lblShutter)
    private val sbShutter: SeekBar = root.findViewById(R.id.sbShutter)
    private val swFocus: MaterialSwitch = root.findViewById(R.id.swFocus)
    private val lblFocus: TextView = root.findViewById(R.id.lblFocus)
    private val sbFocus: SeekBar = root.findViewById(R.id.sbFocus)
    private val swKelvin: MaterialSwitch = root.findViewById(R.id.swKelvin)
    private val lblKelvin: TextView = root.findViewById(R.id.lblKelvin)
    private val sbKelvin: SeekBar = root.findViewById(R.id.sbKelvin)

    private var shutterStops = listOf<Long>()

    private val svc get() = StreamService.instance
    private fun manual(): CameraCaps.Manual? = svc?.characteristics?.let { CameraCaps.manual(it) }

    fun bind() {
        swIso.onToggle { on ->
            val range = manual()?.iso ?: return@onToggle
            val start = (svc?.liveValues()?.iso ?: DEFAULT_ISO).coerceIn(range.lower, range.upper)
            svc?.updateControls { it.copy(iso = if (on) start else null) }
        }
        sbIso.onDrag { p ->
            val range = manual()?.iso ?: return@onDrag
            svc?.updateControls { it.copy(iso = ProScale.isoAt(p, range)) }
        }
        swShutter.onToggle { on ->
            val stops = shutterStops.ifEmpty { return@onToggle }
            val live = svc?.liveValues()?.exposureNs ?: (svc?.frameNs() ?: stops.last()) / 2
            svc?.updateControls { it.copy(exposureNs = if (on) stops[ProScale.nearestStop(stops, live)] else null) }
        }
        sbShutter.onDrag { p ->
            val ns = shutterStops.getOrNull(p) ?: return@onDrag
            svc?.updateControls { it.copy(exposureNs = ns) }
        }
        swFocus.onToggle { on ->
            val max = manual()?.focusDiopters ?: return@onToggle
            val start = (svc?.liveValues()?.focusDiopters ?: 0f).coerceIn(0f, max)
            svc?.updateControls { it.copy(focusDiopters = if (on) start else null, focusPoint = null) }
        }
        sbFocus.onDrag { p ->
            val max = manual()?.focusDiopters ?: return@onDrag
            svc?.updateControls { it.copy(focusDiopters = ProScale.focusAt(p, max), focusPoint = null) }
        }
        swKelvin.onToggle { on ->
            val start = svc?.autoKelvin() ?: DEFAULT_KELVIN
            svc?.updateControls { it.copy(kelvin = if (on) start else null) }
        }
        sbKelvin.onDrag { p -> svc?.updateControls { it.copy(kelvin = ProScale.kelvinAt(p)) } }
        root.findViewById<MaterialButton>(R.id.btnAllAuto).setOnClickListener {
            svc?.updateControls {
                it.copy(
                    exposure = 0, aeLock = false, focusPoint = null,
                    iso = null, exposureNs = null, focusDiopters = null, kelvin = null,
                )
            }
        }
        refresh()
    }

    /** Reflete o estado atual (inclusive mudanças feitas pelo PC) sem disparar os listeners. */
    fun refresh() {
        val s = svc ?: return
        val m = manual() ?: return
        val ctl = s.controls()
        val live = s.liveValues()

        val iso = m.iso
        val exp = m.exposureNs
        val canExpose = iso != null && exp != null
        shutterStops = if (exp != null) ProScale.shutterStops(exp, s.frameNs()) else emptyList()

        swIso.isEnabled = canExpose
        swIso.isChecked = ctl.iso != null
        sbIso.max = ProScale.ISO_STEPS
        sbIso.isEnabled = ctl.iso != null
        if (iso != null && !sbIso.isPressed) sbIso.progress = ProScale.isoProgress(ctl.iso ?: live.iso ?: DEFAULT_ISO, iso)
        lblIso.text = when {
            !canExpose -> UNAVAILABLE
            ctl.iso != null -> "ISO ${ctl.iso}"
            ctl.exposureNs != null -> "Fixo em ISO ${live.iso ?: "—"} enquanto o obturador for manual"
            else -> "Automático" + (live.iso?.let { " (agora ISO $it)" } ?: "")
        }

        swShutter.isEnabled = canExpose
        swShutter.isChecked = ctl.exposureNs != null
        sbShutter.max = (shutterStops.size - 1).coerceAtLeast(0)
        sbShutter.isEnabled = ctl.exposureNs != null
        val shownNs = ctl.exposureNs ?: live.exposureNs
        if (!sbShutter.isPressed && shownNs != null) sbShutter.progress = ProScale.nearestStop(shutterStops, shownNs)
        lblShutter.text = when {
            !canExpose -> UNAVAILABLE
            ctl.exposureNs != null -> ProScale.shutterLabel(ctl.exposureNs)
            ctl.iso != null -> "Fixo em ${live.exposureNs?.let { ProScale.shutterLabel(it) } ?: "—"} enquanto o ISO for manual"
            else -> "Automático" + (live.exposureNs?.let { " (agora ${ProScale.shutterLabel(it)})" } ?: "")
        }

        val maxFocus = m.focusDiopters
        swFocus.isEnabled = maxFocus > 0f
        swFocus.isChecked = ctl.focusDiopters != null && maxFocus > 0f
        sbFocus.max = ProScale.FOCUS_STEPS
        sbFocus.isEnabled = swFocus.isChecked
        if (!sbFocus.isPressed) sbFocus.progress = ProScale.focusProgress(ctl.focusDiopters ?: live.focusDiopters ?: 0f, maxFocus)
        lblFocus.text = when {
            maxFocus <= 0f -> "Indisponível nesta câmera (foco fixo)"
            ctl.focusDiopters != null -> "Foco em " + ProScale.focusLabel(ctl.focusDiopters, maxFocus, m.focusCalibrated)
            else -> "Automático"
        }

        swKelvin.isEnabled = m.kelvin
        swKelvin.isChecked = ctl.kelvin != null && m.kelvin
        sbKelvin.max = ProScale.KELVIN_STEPS
        sbKelvin.isEnabled = swKelvin.isChecked
        val autoK = if (ctl.kelvin == null) s.autoKelvin() else null
        if (!sbKelvin.isPressed) sbKelvin.progress = ProScale.kelvinProgress(ctl.kelvin ?: autoK ?: DEFAULT_KELVIN)
        lblKelvin.text = when {
            !m.kelvin -> UNAVAILABLE
            ctl.kelvin != null -> "${ctl.kelvin} K"
            else -> "Automático" + (autoK?.let { " (~$it K)" } ?: "")
        }

        // Com a exposição ou o branco manuais, os ajustes automáticos equivalentes não valem.
        root.findViewById<SeekBar>(R.id.sbExposure).isEnabled = !ctl.manualExposure
        root.findViewById<MaterialSwitch>(R.id.swAeLock).isEnabled = !ctl.manualExposure
        root.findViewById<Spinner>(R.id.spAwb).isEnabled = ctl.kelvin == null
    }

    private fun MaterialSwitch.onToggle(action: (Boolean) -> Unit) =
        setOnCheckedChangeListener { b, checked -> if (b.isPressed) action(checked) }

    private fun SeekBar.onDrag(action: (Int) -> Unit) = setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
        override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) {
            if (fromUser) action(progress)
        }

        override fun onStartTrackingTouch(bar: SeekBar) = Unit
        override fun onStopTrackingTouch(bar: SeekBar) = Unit
    })

    private companion object {
        const val DEFAULT_ISO = 100
        const val DEFAULT_KELVIN = 5500
        const val UNAVAILABLE = "Indisponível nesta câmera"
    }
}

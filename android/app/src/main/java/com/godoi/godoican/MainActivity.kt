package com.godoi.godoican

import android.Manifest
import android.content.pm.PackageManager
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CaptureRequest
import android.os.Build
import android.os.Bundle
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.FrameLayout
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.isVisible
import com.godoi.godoican.media.CameraCaps
import com.google.android.material.button.MaterialButton
import com.google.android.material.materialswitch.MaterialSwitch

class MainActivity : AppCompatActivity() {
    private lateinit var settings: AppSettings

    private lateinit var preview: SurfaceView
    private lateinit var statusTitle: TextView
    private lateinit var statusDetail: TextView
    private lateinit var toast: TextView
    private lateinit var pairCard: View
    private lateinit var pairCode: TextView
    private lateinit var pairHint: TextView
    private lateinit var settingsPanel: View
    private lateinit var curtain: FrameLayout
    private lateinit var btnPower: MaterialButton
    private lateinit var btnTorch: MaterialButton

    private val statusObserver: (Hub.Status) -> Unit = { render(it) }
    private val hideToast = Runnable { toast.isVisible = false }

    private val permissionRequest = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        if (hasCamera()) StreamService.start(this)
        else showToast("Sem permissão de câmera o GodoiCan não funciona.")
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        settings = AppSettings(this)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        preview = findViewById(R.id.preview)
        statusTitle = findViewById(R.id.statusTitle)
        statusDetail = findViewById(R.id.statusDetail)
        toast = findViewById(R.id.toast)
        pairCard = findViewById(R.id.pairCard)
        pairCode = findViewById(R.id.pairCode)
        pairHint = findViewById(R.id.pairHint)
        settingsPanel = findViewById(R.id.settingsPanel)
        curtain = findViewById(R.id.curtain)
        btnPower = findViewById(R.id.btnPower)
        btnTorch = findViewById(R.id.btnTorch)

        preview.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) = Unit

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                StreamService.setPreview(StreamService.PreviewTarget(holder.surface, width, height))
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                StreamService.setPreview(null)
            }
        })
        setupGestures()
        setupButtons()
        setupSettings()
        ensurePermissionsAndStart()
    }

    override fun onStart() {
        super.onStart()
        Hub.observe(statusObserver)
    }

    override fun onStop() {
        Hub.unobserve(statusObserver)
        super.onStop()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        when {
            curtain.isVisible -> setCurtain(false)
            settingsPanel.isVisible -> settingsPanel.isVisible = false
            else -> @Suppress("DEPRECATION") super.onBackPressed()
        }
    }

    // ---------------------------------------------------------------- permissões

    private fun hasCamera() =
        ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    private fun ensurePermissionsAndStart() {
        val wanted = buildList {
            add(Manifest.permission.CAMERA)
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        }.filter { ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED }
        if (wanted.isEmpty()) StreamService.start(this) else permissionRequest.launch(wanted.toTypedArray())
    }

    // ---------------------------------------------------------------- estado

    private fun render(s: Hub.Status) {
        val (title, color) = when {
            !s.running -> "Desligado" to R.color.off
            s.pcName != null -> "Transmitindo para ${s.pcName}" to R.color.ok
            else -> "Aguardando o PC" to R.color.wait
        }
        statusTitle.text = title
        val dot = ContextCompat.getDrawable(this, R.drawable.dot)!!.mutate()
        dot.setTint(ContextCompat.getColor(this, color))
        statusTitle.setCompoundDrawablesRelativeWithIntrinsicBounds(dot, null, null, null)

        statusDetail.text = listOfNotNull(
            s.addresses.takeIf { it.isNotBlank() && s.running }?.let { "IP do celular: $it" },
            s.video.takeIf { it.isNotBlank() && s.running },
            s.stats.takeIf { it.isNotBlank() },
            s.message,
        ).joinToString("\n")

        pairCard.isVisible = s.pairingCode != null
        s.pairingCode?.let { code ->
            pairCode.text = "${code.take(3)} ${code.drop(3)}"
            pairHint.text = "Digite este código no GodoiCan do PC \"${s.pairingPc}\"."
            if (curtain.isVisible) setCurtain(false)
        }

        btnPower.text = getString(if (s.running) R.string.power_off else R.string.power_on)
        btnTorch.isEnabled = StreamService.instance?.characteristics?.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true
    }

    private fun showToast(text: String) {
        toast.text = text
        toast.isVisible = true
        toast.removeCallbacks(hideToast)
        toast.postDelayed(hideToast, 1800)
    }

    // ---------------------------------------------------------------- gestos e botões

    private fun setupGestures() {
        val scale = ScaleGestureDetector(this, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                val svc = StreamService.instance ?: return false
                val c = svc.characteristics ?: return false
                val range = CameraCaps.zoomRange(c)
                val zoom = (svc.controls().zoom * detector.scaleFactor).coerceIn(range.lower, range.upper)
                svc.updateControls { it.copy(zoom = zoom) }
                settings.zoom = zoom
                showToast("%.1f×".format(zoom))
                return true
            }
        })
        val taps = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                if (settingsPanel.isVisible) {
                    settingsPanel.isVisible = false
                    return true
                }
                if (StreamService.instance?.focusAt(e.x, e.y) == true) showToast("Foco travado aqui")
                return true
            }

            override fun onDoubleTap(e: MotionEvent): Boolean {
                StreamService.instance?.updateControls { it.copy(focusPoint = null) }
                showToast("Foco automático")
                return true
            }
        })
        preview.setOnTouchListener { _, event ->
            scale.onTouchEvent(event)
            if (!scale.isInProgress) taps.onTouchEvent(event)
            true
        }

        val curtainTaps = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDoubleTap(e: MotionEvent): Boolean {
                setCurtain(false)
                return true
            }
        })
        curtain.setOnTouchListener { _, event ->
            curtainTaps.onTouchEvent(event)
            true
        }
    }

    private fun setupButtons() {
        findViewById<MaterialButton>(R.id.btnSwitch).setOnClickListener {
            val svc = StreamService.instance ?: return@setOnClickListener
            if (svc.cameras().size < 2) {
                showToast("Este aparelho só expõe uma câmera")
            } else {
                svc.switchCamera()
                settingsPanel.isVisible = false
            }
        }
        btnTorch.setOnClickListener {
            val svc = StreamService.instance ?: return@setOnClickListener
            val on = !svc.controls().torch
            svc.updateControls { it.copy(torch = on) }
            showToast(if (on) "Lanterna ligada" else "Lanterna desligada")
        }
        findViewById<MaterialButton>(R.id.btnSettings).setOnClickListener {
            if (settingsPanel.isVisible) {
                settingsPanel.isVisible = false
            } else {
                populateSettings()
                settingsPanel.isVisible = true
            }
        }
        findViewById<MaterialButton>(R.id.btnCloseSettings).setOnClickListener { settingsPanel.isVisible = false }
        findViewById<MaterialButton>(R.id.btnCurtain).setOnClickListener { setCurtain(true) }
        btnPower.setOnClickListener {
            if (Hub.status.running) StreamService.instance?.shutdown() else ensurePermissionsAndStart()
        }
    }

    /** "Tela preta": esconde a prévia (a GPU para de desenhar nela) e baixa o brilho ao mínimo. */
    private fun setCurtain(on: Boolean) {
        curtain.isVisible = on
        preview.isVisible = !on
        settingsPanel.isVisible = false
        window.attributes = window.attributes.apply {
            screenBrightness = if (on) 0.01f else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
    }

    // ---------------------------------------------------------------- painel de ajustes

    private val rotationValues = listOf(0, 90, 180, 270)
    private var awbValues = listOf<Int>()
    private var cameraIds = listOf<String>()
    private var resolutionValues = listOf<CameraCaps.Resolution>()
    private var fpsValues = listOf<Int>()
    private var evMin = 0

    private fun Spinner.onPick(action: (Int) -> Unit) {
        onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) = action(position)
            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
    }

    private fun Spinner.fill(items: List<String>, selected: Int) {
        adapter = ArrayAdapter(context, android.R.layout.simple_spinner_dropdown_item, items)
        setSelection(selected.coerceIn(0, maxOf(0, items.size - 1)), false)
    }

    private fun setupSettings() {
        // Os listeners comparam com o valor salvo: o Spinner dispara ao ser preenchido.
        findViewById<Spinner>(R.id.spCamera).onPick { i ->
            val id = cameraIds.getOrNull(i) ?: return@onPick
            if (id != settings.cameraId) {
                settings.cameraId = id
                settings.zoom = 1f
                StreamService.instance?.restartPipeline()
                settingsPanel.postDelayed({ populateSettings() }, 600)
            }
        }
        findViewById<Spinner>(R.id.spResolution).onPick { i ->
            val r = resolutionValues.getOrNull(i) ?: return@onPick
            if (r.width != settings.width || r.height != settings.height) {
                settings.width = r.width
                settings.height = r.height
                StreamService.instance?.restartPipeline()
            }
        }
        findViewById<Spinner>(R.id.spFps).onPick { i ->
            val f = fpsValues.getOrNull(i) ?: return@onPick
            if (f != settings.fps) {
                settings.fps = f
                StreamService.instance?.restartPipeline()
                settingsPanel.postDelayed({ populateSettings() }, 600)
            }
        }
        findViewById<Spinner>(R.id.spQuality).onPick { i ->
            if (i != settings.quality) {
                settings.quality = i
                StreamService.instance?.restartPipeline()
            }
        }
        findViewById<Spinner>(R.id.spRotation).onPick { i ->
            val v = rotationValues[i]
            if (v != settings.rotationOffset) {
                settings.rotationOffset = v
                StreamService.instance?.updateTransform()
            }
        }
        findViewById<Spinner>(R.id.spAwb).onPick { i ->
            val mode = awbValues.getOrNull(i) ?: return@onPick
            if (mode != settings.awbMode) {
                settings.awbMode = mode
                StreamService.instance?.updateControls { it.copy(awbMode = mode) }
            }
        }

        fun toggle(id: Int, apply: (Boolean) -> Unit) {
            findViewById<MaterialSwitch>(id).setOnCheckedChangeListener { b, checked -> if (b.isPressed) apply(checked) }
        }
        toggle(R.id.swMirror) {
            settings.mirror = it
            StreamService.instance?.updateTransform()
        }
        toggle(R.id.swFill) {
            settings.fill = it
            StreamService.instance?.updateTransform()
        }
        toggle(R.id.swAutoRotate) { settings.autoRotate = it }
        toggle(R.id.swAeLock) { on -> StreamService.instance?.updateControls { it.copy(aeLock = on) } }
        toggle(R.id.swStabilization) { on ->
            settings.stabilization = on
            StreamService.instance?.updateControls { it.copy(stabilization = on) }
        }
        toggle(R.id.swLowLight) {
            settings.lowLight = it
            StreamService.instance?.restartPipeline()
        }

        findViewById<SeekBar>(R.id.sbExposure).setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) {
                if (!fromUser) return
                val ev = progress + evMin
                settings.exposure = ev
                updateExposureLabel(ev)
                StreamService.instance?.updateControls { it.copy(exposure = ev) }
            }

            override fun onStartTrackingTouch(bar: SeekBar) = Unit
            override fun onStopTrackingTouch(bar: SeekBar) = Unit
        })

        findViewById<MaterialButton>(R.id.btnAutoFocus).setOnClickListener {
            StreamService.instance?.updateControls { it.copy(focusPoint = null) }
            showToast("Foco automático")
        }
        findViewById<MaterialButton>(R.id.btnForget).setOnClickListener {
            StreamService.instance?.forgetPairs()
            populateSettings()
            showToast("PCs esquecidos. Pareie de novo pelo PC.")
        }
    }

    private fun updateExposureLabel(ev: Int) {
        findViewById<TextView>(R.id.lblExposure).text =
            getString(R.string.exposure) + if (ev == 0) "" else " (${if (ev > 0) "+" else ""}$ev)"
    }

    private fun populateSettings() {
        val svc = StreamService.instance
        val c = svc?.characteristics
        if (svc == null || c == null) {
            showToast("Ligue o GodoiCan primeiro")
            return
        }

        val cams = svc.cameras()
        cameraIds = cams.map { it.id }
        findViewById<Spinner>(R.id.spCamera).fill(cams.map { it.label }, cameraIds.indexOf(settings.cameraId))

        fpsValues = CameraCaps.supportedFps(c)
        findViewById<Spinner>(R.id.spFps).fill(fpsValues.map { "$it fps" }, fpsValues.indexOf(settings.fps))

        resolutionValues = CameraCaps.resolutions(c, settings.fps)
        findViewById<Spinner>(R.id.spResolution).fill(
            resolutionValues.map { it.toString() },
            resolutionValues.indexOfFirst { it.width == settings.width && it.height == settings.height },
        )

        findViewById<Spinner>(R.id.spQuality).fill(
            AppSettings.QUALITY_NAMES.mapIndexed { i, name ->
                val mbps = AppSettings.bitrateFor(settings.width, settings.height, settings.fps, i) / 1_000_000.0
                "$name (~${"%.0f".format(mbps)} Mbps)"
            },
            settings.quality,
        )

        findViewById<Spinner>(R.id.spRotation).fill(listOf("Nenhuma", "90°", "180°", "270°"), rotationValues.indexOf(settings.rotationOffset))

        val awbAvailable = c.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES)?.toSet() ?: emptySet()
        val awbNames = linkedMapOf(
            CaptureRequest.CONTROL_AWB_MODE_AUTO to "Automático",
            CaptureRequest.CONTROL_AWB_MODE_DAYLIGHT to "Luz do dia",
            CaptureRequest.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT to "Nublado",
            CaptureRequest.CONTROL_AWB_MODE_FLUORESCENT to "Fluorescente",
            CaptureRequest.CONTROL_AWB_MODE_INCANDESCENT to "Incandescente",
            CaptureRequest.CONTROL_AWB_MODE_WARM_FLUORESCENT to "Fluorescente quente",
            CaptureRequest.CONTROL_AWB_MODE_SHADE to "Sombra",
        ).filterKeys { it in awbAvailable || it == CaptureRequest.CONTROL_AWB_MODE_AUTO }
        awbValues = awbNames.keys.toList()
        findViewById<Spinner>(R.id.spAwb).fill(awbNames.values.toList(), awbValues.indexOf(settings.awbMode))

        val ev = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
        val bar = findViewById<SeekBar>(R.id.sbExposure)
        evMin = ev?.lower ?: 0
        bar.max = (ev?.upper ?: 0) - evMin
        bar.progress = settings.exposure - evMin
        bar.isEnabled = bar.max > 0
        updateExposureLabel(settings.exposure)

        findViewById<MaterialSwitch>(R.id.swMirror).isChecked = settings.mirror
        findViewById<MaterialSwitch>(R.id.swFill).isChecked = settings.fill
        findViewById<MaterialSwitch>(R.id.swAutoRotate).isChecked = settings.autoRotate
        findViewById<MaterialSwitch>(R.id.swAeLock).isChecked = svc.controls().aeLock
        findViewById<MaterialSwitch>(R.id.swStabilization).isChecked = settings.stabilization
        findViewById<MaterialSwitch>(R.id.swLowLight).isChecked = settings.lowLight

        val names = svc.pairs.names()
        findViewById<TextView>(R.id.pairedList).text =
            if (names.isEmpty()) "Nenhum PC pareado ainda." else names.joinToString("\n") { "• $it" }
    }
}

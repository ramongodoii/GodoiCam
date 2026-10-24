package com.godoi.godoican

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CaptureRequest
import android.os.Build
import android.os.Bundle
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.isVisible
import com.godoi.godoican.media.CameraCaps
import com.godoi.godoican.media.DeviceReport
import com.google.android.material.bottomsheet.BottomSheetBehavior
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.button.MaterialButton
import com.google.android.material.materialswitch.MaterialSwitch
import kotlin.math.abs

/**
 * Tela do celular: prévia em tela cheia (a área clara é o que vai para o PC), selo de estado
 * no topo, atalhos de zoom e quatro botões redondos. Os ajustes abrem numa folha inferior.
 */
class MainActivity : AppCompatActivity() {
    private lateinit var settings: AppSettings

    private lateinit var preview: SurfaceView
    private lateinit var statusChip: View
    private lateinit var statusDot: View
    private lateinit var statusTitle: TextView
    private lateinit var statusDetail: TextView
    private lateinit var toast: TextView
    private lateinit var zoomRow: LinearLayout
    private lateinit var pairCard: View
    private lateinit var pairCode: TextView
    private lateinit var pairHint: TextView
    private lateinit var curtain: FrameLayout
    private lateinit var btnTorch: ImageButton

    private var sheet: BottomSheetDialog? = null
    private var sheetView: View? = null
    private var zoomShortcuts = listOf<Float>()

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
        statusChip = findViewById(R.id.statusChip)
        statusDot = findViewById(R.id.statusDot)
        statusTitle = findViewById(R.id.statusTitle)
        statusDetail = findViewById(R.id.statusDetail)
        toast = findViewById(R.id.toast)
        zoomRow = findViewById(R.id.zoomRow)
        pairCard = findViewById(R.id.pairCard)
        pairCode = findViewById(R.id.pairCode)
        pairHint = findViewById(R.id.pairHint)
        curtain = findViewById(R.id.curtain)
        btnTorch = findViewById(R.id.btnTorch)

        // Os controles ficam longe do recorte da câmera e das barras do sistema.
        val overlay = findViewById<View>(R.id.overlay)
        ViewCompat.setOnApplyWindowInsetsListener(overlay) { v, insets ->
            val safe = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            v.setPadding(safe.left, safe.top, safe.right, safe.bottom)
            insets
        }

        preview.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) = Unit

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                StreamService.setPreview(StreamService.PreviewTarget(holder.surface, width, height, displayDegrees()))
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                StreamService.setPreview(null)
            }
        })
        setupGestures()
        setupButtons()
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

    override fun onDestroy() {
        sheet?.dismiss()
        super.onDestroy()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        when {
            curtain.isVisible -> setCurtain(false)
            statusDetail.isVisible -> statusDetail.isVisible = false
            else -> @Suppress("DEPRECATION") super.onBackPressed()
        }
    }

    @Suppress("DEPRECATION")
    private fun displayDegrees(): Int = when (windowManager.defaultDisplay.rotation) {
        Surface.ROTATION_90 -> 90
        Surface.ROTATION_180 -> 180
        Surface.ROTATION_270 -> 270
        else -> 0
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
        statusDot.background.mutate().setTint(ContextCompat.getColor(this, color))

        statusDetail.text = listOfNotNull(
            s.addresses.takeIf { it.isNotBlank() && s.running }?.let { "IP do celular: $it" },
            s.video.takeIf { it.isNotBlank() && s.running },
            s.stats.takeIf { it.isNotBlank() },
            s.message,
        ).joinToString("\n")
        // Mensagens de erro aparecem sozinhas; o resto só quando o usuário toca no selo.
        if (s.message != null && s.pcName == null) statusDetail.isVisible = true

        pairCard.isVisible = s.pairingCode != null
        s.pairingCode?.let { code ->
            pairCode.text = "${code.take(3)} ${code.drop(3)}"
            pairHint.text = "Digite este código no GodoiCan do PC \"${s.pairingPc}\"."
            if (curtain.isVisible) setCurtain(false)
        }

        val svc = StreamService.instance
        btnTorch.isEnabled = svc?.characteristics?.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true
        btnTorch.alpha = if (btnTorch.isEnabled) 1f else 0.35f
        btnTorch.isActivated = svc?.controls()?.torch == true
        refreshZoomRow()
        (sheetView?.findViewById<MaterialButton>(R.id.btnPower))?.text =
            getString(if (s.running) R.string.power_off else R.string.power_on)
    }

    private fun showToast(text: String) {
        toast.text = text
        toast.isVisible = true
        toast.removeCallbacks(hideToast)
        toast.postDelayed(hideToast, 1600)
    }

    // ---------------------------------------------------------------- zoom

    /** Atalhos 0,6× / 1× / 2× / 3×… conforme a faixa de zoom da câmera atual. */
    private fun refreshZoomRow() {
        val c = StreamService.instance?.characteristics
        val range = c?.let { CameraCaps.zoomRange(it) }
        val shortcuts = if (range == null) emptyList() else
            listOf(range.lower, 1f, 2f, 3f, 5f).filter { it >= range.lower - 0.01f && it <= range.upper + 0.01f }
                .distinctBy { "%.1f".format(it) }.take(4)
        if (shortcuts != zoomShortcuts) {
            zoomShortcuts = shortcuts
            zoomRow.removeAllViews()
            val size = (40 * resources.displayMetrics.density).toInt()
            for (z in shortcuts) {
                val tv = TextView(this).apply {
                    text = zoomLabel(z)
                    gravity = Gravity.CENTER
                    textSize = 12f
                    setTextColor(ContextCompat.getColor(context, R.color.text_primary))
                    setBackgroundResource(R.drawable.zoom_chip)
                    setOnClickListener { setZoom(z) }
                }
                zoomRow.addView(tv, LinearLayout.LayoutParams(size, size))
            }
        }
        zoomRow.isVisible = shortcuts.size > 1
        val current = StreamService.instance?.controls()?.zoom ?: 1f
        val nearest = shortcuts.minByOrNull { abs(it - current) }
        for (i in 0 until zoomRow.childCount) {
            val z = shortcuts[i]
            val tv = zoomRow.getChildAt(i) as TextView
            tv.isSelected = z == nearest
            tv.text = if (z == nearest && abs(current - z) > 0.05f) zoomLabel(current) else zoomLabel(z)
        }
    }

    private fun zoomLabel(z: Float) = if (z < 1f || z % 1f != 0f) "%.1f×".format(z).replace('.', ',') else "${z.toInt()}×"

    private fun setZoom(z: Float) {
        val svc = StreamService.instance ?: return
        svc.updateControls { it.copy(zoom = z) }
        settings.zoom = z
        zoomRow.postDelayed({ refreshZoomRow() }, 50)
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
                zoomRow.post { refreshZoomRow() }
                return true
            }
        })
        val taps = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                if (statusDetail.isVisible) {
                    statusDetail.isVisible = false
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
        statusChip.setOnClickListener { statusDetail.isVisible = !statusDetail.isVisible }
        findViewById<ImageButton>(R.id.btnSwitch).setOnClickListener {
            val svc = StreamService.instance ?: return@setOnClickListener
            if (svc.cameras().size < 2) {
                showToast("Este aparelho só expõe uma câmera")
            } else {
                svc.switchCamera()
                zoomRow.postDelayed({ refreshZoomRow() }, 800)
            }
        }
        btnTorch.setOnClickListener {
            val svc = StreamService.instance ?: return@setOnClickListener
            val on = !svc.controls().torch
            svc.updateControls { it.copy(torch = on) }
            btnTorch.isActivated = on
        }
        findViewById<ImageButton>(R.id.btnCurtain).setOnClickListener { setCurtain(true) }
        findViewById<ImageButton>(R.id.btnSettings).setOnClickListener { openSettings() }
    }

    /** "Tela preta": esconde a prévia (a GPU para de desenhar nela) e baixa o brilho ao mínimo. */
    private fun setCurtain(on: Boolean) {
        curtain.isVisible = on
        preview.isVisible = !on
        sheet?.dismiss()
        window.attributes = window.attributes.apply {
            screenBrightness = if (on) 0.01f else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
    }

    // ---------------------------------------------------------------- folha de ajustes

    private val rotationValues = listOf(0, 90, 180, 270)
    private var awbValues = listOf<Int>()
    private var cameraIds = listOf<String>()
    private var resolutionValues = listOf<CameraCaps.Resolution>()
    private var fpsValues = listOf<Int>()
    private var evMin = 0

    private fun openSettings() {
        if (StreamService.instance?.characteristics == null) {
            showToast("Ligue o GodoiCan primeiro")
            if (!Hub.status.running) ensurePermissionsAndStart()
            return
        }
        val view = layoutInflater.inflate(R.layout.settings_sheet, null)
        val dialog = BottomSheetDialog(this)
        dialog.setContentView(view)
        dialog.behavior.state = BottomSheetBehavior.STATE_EXPANDED
        dialog.behavior.skipCollapsed = true
        dialog.setOnDismissListener {
            if (sheet === dialog) {
                sheet = null
                sheetView = null
            }
        }
        sheet = dialog
        sheetView = view
        populateSettings(view)
        setupSettings(view)
        dialog.show()
    }

    private fun <T : View> View.v(id: Int): T = findViewById(id)

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

    private fun setupSettings(root: View) {
        // Os listeners comparam com o valor salvo: o Spinner dispara ao ser preenchido.
        root.v<Spinner>(R.id.spCamera).onPick { i ->
            val id = cameraIds.getOrNull(i) ?: return@onPick
            if (id != settings.cameraId) {
                settings.cameraId = id
                settings.zoom = 1f
                StreamService.instance?.restartPipeline()
                root.postDelayed({ populateSettings(root) }, 800)
            }
        }
        root.v<Spinner>(R.id.spResolution).onPick { i ->
            val r = resolutionValues.getOrNull(i) ?: return@onPick
            if (r.width != settings.width || r.height != settings.height) {
                settings.width = r.width
                settings.height = r.height
                StreamService.instance?.restartPipeline()
                root.postDelayed({ populateSettings(root) }, 800)
            }
        }
        root.v<Spinner>(R.id.spFps).onPick { i ->
            val f = fpsValues.getOrNull(i) ?: return@onPick
            if (f != settings.fps) {
                settings.fps = f
                StreamService.instance?.restartPipeline()
                root.postDelayed({ populateSettings(root) }, 800)
            }
        }
        root.v<Spinner>(R.id.spQuality).onPick { i ->
            if (i != settings.quality) {
                settings.quality = i
                StreamService.instance?.applyQuality() // muda o bitrate na hora, sem reiniciar
            }
        }
        root.v<Spinner>(R.id.spRotation).onPick { i ->
            val v = rotationValues[i]
            if (v != settings.rotationOffset) {
                settings.rotationOffset = v
                StreamService.instance?.updateTransform()
            }
        }
        root.v<Spinner>(R.id.spAwb).onPick { i ->
            val mode = awbValues.getOrNull(i) ?: return@onPick
            if (mode != settings.awbMode) {
                settings.awbMode = mode
                StreamService.instance?.updateControls { it.copy(awbMode = mode) }
            }
        }

        fun toggle(id: Int, apply: (Boolean) -> Unit) {
            root.v<MaterialSwitch>(id).setOnCheckedChangeListener { b, checked -> if (b.isPressed) apply(checked) }
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

        root.v<SeekBar>(R.id.sbExposure).setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) {
                if (!fromUser) return
                val ev = progress + evMin
                settings.exposure = ev
                updateExposureLabel(root, ev)
                StreamService.instance?.updateControls { it.copy(exposure = ev) }
            }

            override fun onStartTrackingTouch(bar: SeekBar) = Unit
            override fun onStopTrackingTouch(bar: SeekBar) = Unit
        })

        root.v<MaterialButton>(R.id.btnAutoFocus).setOnClickListener {
            StreamService.instance?.updateControls { it.copy(focusPoint = null) }
            showToast("Foco automático")
        }
        root.v<MaterialButton>(R.id.btnReport).setOnClickListener {
            showToast("Gerando relatório…")
            Thread {
                val text = DeviceReport.build(this)
                runOnUiThread {
                    val send = Intent(Intent.ACTION_SEND).setType("text/plain")
                        .putExtra(Intent.EXTRA_SUBJECT, "GodoiCan — relatório do aparelho")
                        .putExtra(Intent.EXTRA_TEXT, text)
                    startActivity(Intent.createChooser(send, getString(R.string.share_report)))
                }
            }.start()
        }
        root.v<MaterialButton>(R.id.btnForget).setOnClickListener {
            StreamService.instance?.forgetPairs()
            populateSettings(root)
            showToast("PCs esquecidos. Pareie de novo pelo PC.")
        }
        root.v<MaterialButton>(R.id.btnPower).setOnClickListener {
            sheet?.dismiss()
            if (Hub.status.running) StreamService.instance?.shutdown() else ensurePermissionsAndStart()
        }
    }

    private fun updateExposureLabel(root: View, ev: Int) {
        root.v<TextView>(R.id.lblExposure).text =
            getString(R.string.exposure) + if (ev == 0) "" else " (${if (ev > 0) "+" else ""}$ev)"
    }

    private fun populateSettings(root: View) {
        val svc = StreamService.instance ?: return
        val c = svc.characteristics ?: return

        val cams = svc.cameras()
        cameraIds = cams.map { it.id }
        root.v<Spinner>(R.id.spCamera).fill(cams.map { it.label }, cameraIds.indexOf(settings.cameraId))

        fpsValues = CameraCaps.supportedFps(c)
        root.v<Spinner>(R.id.spFps).fill(fpsValues.map { "$it fps" }, fpsValues.indexOf(settings.fps))

        resolutionValues = CameraCaps.resolutions(c, settings.fps)
        root.v<Spinner>(R.id.spResolution).fill(
            resolutionValues.map { it.toString() },
            resolutionValues.indexOfFirst { it.width == settings.width && it.height == settings.height },
        )

        root.v<Spinner>(R.id.spQuality).fill(
            AppSettings.QUALITY_NAMES.mapIndexed { i, name ->
                val mbps = AppSettings.bitrateFor(settings.width, settings.height, settings.fps, i) / 1_000_000.0
                "$name (~${"%.0f".format(mbps)} Mbps)"
            },
            settings.quality,
        )

        root.v<Spinner>(R.id.spRotation).fill(listOf("Nenhuma", "90°", "180°", "270°"), rotationValues.indexOf(settings.rotationOffset))

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
        root.v<Spinner>(R.id.spAwb).fill(awbNames.values.toList(), awbValues.indexOf(settings.awbMode))

        val ev = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
        val bar = root.v<SeekBar>(R.id.sbExposure)
        evMin = ev?.lower ?: 0
        bar.max = (ev?.upper ?: 0) - evMin
        bar.progress = settings.exposure - evMin
        bar.isEnabled = bar.max > 0
        updateExposureLabel(root, settings.exposure)

        root.v<MaterialSwitch>(R.id.swMirror).isChecked = settings.mirror
        root.v<MaterialSwitch>(R.id.swFill).isChecked = settings.fill
        root.v<MaterialSwitch>(R.id.swAutoRotate).isChecked = settings.autoRotate
        root.v<MaterialSwitch>(R.id.swAeLock).isChecked = svc.controls().aeLock
        root.v<MaterialSwitch>(R.id.swStabilization).isChecked = settings.stabilization
        root.v<MaterialSwitch>(R.id.swLowLight).isChecked = settings.lowLight

        val names = svc.pairs.names()
        root.v<TextView>(R.id.pairedList).text =
            if (names.isEmpty()) "Nenhum PC pareado ainda." else names.joinToString("\n") { "• $it" }
        root.v<MaterialButton>(R.id.btnPower).text =
            getString(if (Hub.status.running) R.string.power_off else R.string.power_on)
    }
}

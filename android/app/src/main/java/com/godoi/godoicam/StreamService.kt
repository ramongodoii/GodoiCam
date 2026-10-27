package com.godoi.godoicam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import android.provider.Settings
import android.view.OrientationEventListener
import android.view.Surface
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import com.godoi.godoicam.media.CameraCaps
import com.godoi.godoicam.media.CameraController
import com.godoi.godoicam.media.DeviceReport
import com.godoi.godoicam.media.GlRenderer
import com.godoi.godoicam.media.VideoEncoder
import com.godoi.godoicam.media.WhiteBalance
import com.godoi.godoicam.net.ClientSession
import com.godoi.godoicam.net.DiscoveryResponder
import com.godoi.godoicam.net.PairStore
import com.godoi.godoicam.net.Proto
import com.godoi.godoicam.net.StreamServer
import com.godoi.godoicam.net.Writer
import com.godoi.godoicam.net.hex
import java.net.Inet4Address
import java.net.NetworkInterface
import kotlin.math.abs
import kotlin.math.min

/**
 * Serviço em primeiro plano (tipo câmera) que mantém a webcam funcionando com a tela
 * desligada: câmera → GPU → encoder → sessão com o PC.
 */
class StreamService : Service(), StreamServer.Callbacks, ClientSession.Listener, CameraController.Listener {

    /** [displayRotation]: rotação da tela em graus (0, 90, 180, 270), como em Display.getRotation. */
    class PreviewTarget(val surface: Surface, val width: Int, val height: Int, val displayRotation: Int = 0)

    companion object {
        @Volatile var instance: StreamService? = null
            private set

        @Volatile private var preview: PreviewTarget? = null

        const val ACTION_STOP = "com.godoi.godoicam.STOP"
        private const val NOTIF_STREAM = 1
        private const val NOTIF_PAIR = 2
        private const val CH_STREAM = "stream"
        private const val CH_PAIR = "pair"
        private const val MIN_ENCODE_FPS = 10
        /** Atraso até o PC abaixo disso: a rede tem folga para recuperar o bitrate rápido. */
        private const val FAST_RECOVERY_DELAY_MS = 30

        fun start(context: Context) {
            ContextCompat.startForegroundService(context, Intent(context, StreamService::class.java))
        }

        /** Chamado pela tela. Síncrono: ao retornar, a GPU já soltou a Surface antiga. */
        fun setPreview(target: PreviewTarget?) {
            preview = target
            instance?.let { svc ->
                svc.applyPreviewTransform()
                svc.renderer?.setPreviewSurface(target?.surface, target?.width ?: 0, target?.height ?: 0)
            }
        }
    }

    lateinit var settings: AppSettings
        private set
    lateinit var pairs: PairStore
        private set

    private val ctrlThread = HandlerThread("GodoiCam-ctrl").apply { start() }
    private val ctrl = Handler(ctrlThread.looper)

    @Volatile private var renderer: GlRenderer? = null
    private var surfaceTexture: SurfaceTexture? = null
    @Volatile private var camera: CameraController? = null
    private var cameraSurface: Surface? = null
    private var server: StreamServer? = null
    private var discovery: DiscoveryResponder? = null
    @Volatile private var encoder: VideoEncoder? = null
    @Volatile private var session: ClientSession? = null
    @Volatile var camInfo: CameraCaps.CamInfo? = null
        private set
    @Volatile var characteristics: CameraCharacteristics? = null
        private set
    private var cameraCount = 1
    /** "Preencher" com que o tamanho da câmera foi escolhido (muda o tamanho ideal). */
    private var camSizeFill = true
    /** Início da última troca de câmera/resolução (0 = nenhuma em andamento), para medir o tempo. */
    @Volatile private var switchStartedAt = 0L
    @Volatile private var realtimeClock = false
    private var started = false

    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var wifiPerfLock: WifiManager.WifiLock? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    private var targetBitrate = 0
    /** Quadros por segundo codificados agora (cai com a rede ruim, volta aos poucos). */
    @Volatile private var encodeFps = 30
    private var lastCongestion = 0L
    private var statsTicks = 0

    @Volatile private var deviceOrientation = 0
    private var pendingOrientation = -1

    /**
     * Gira a imagem enviada ao PC só quando o celular fica na nova posição por 500 ms e
     * passou bem do meio-termo (60°), para não alternar perto da diagonal.
     */
    private val orientationListener by lazy {
        object : OrientationEventListener(this) {
            override fun onOrientationChanged(orientation: Int) {
                if (orientation == ORIENTATION_UNKNOWN || !settings.autoRotate) return
                val d = abs(orientation - deviceOrientation) % 360
                if (min(d, 360 - d) <= 60) {
                    if (pendingOrientation != -1) {
                        pendingOrientation = -1
                        ctrl.removeCallbacks(commitOrientation)
                    }
                    return
                }
                val candidate = ((orientation + 45) / 90 * 90) % 360
                if (candidate != pendingOrientation) {
                    pendingOrientation = candidate
                    ctrl.removeCallbacks(commitOrientation)
                    ctrl.postDelayed(commitOrientation, 500)
                }
            }
        }
    }

    private val commitOrientation = Runnable {
        if (pendingOrientation == -1) return@Runnable
        deviceOrientation = pendingOrientation
        pendingOrientation = -1
        applyTransform()
        encoder?.requestKeyframe() // começa a imagem girada com um quadro limpo
    }

    // ---------------------------------------------------------------- ciclo de vida

    override fun onCreate() {
        super.onCreate()
        instance = this
        settings = AppSettings(this)
        pairs = PairStore(this)
        createChannels()
        ServiceCompat.startForeground(
            this, NOTIF_STREAM, notification("Iniciando…"),
            if (Build.VERSION.SDK_INT >= 30) ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA else 0,
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            shutdown()
        } else if (!started) {
            started = true
            ctrl.post { startAll() }
        }
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        instance = null
        ctrl.post {
            if (started) stopAll()
            ctrlThread.quitSafely()
        }
        super.onDestroy()
    }

    fun shutdown() {
        ctrl.post {
            if (started) stopAll()
            started = false
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    private fun startAll() {
        acquireLocks()
        Hub.update { Hub.Status(running = true, addresses = localAddresses()) }

        val r = GlRenderer()
        surfaceTexture = r.start()
        renderer = r
        preview?.let { r.setPreviewSurface(it.surface, it.width, it.height) }

        camera = CameraController(this, this)
        openCamera()

        try {
            server = StreamServer(pairs, ::phoneName, this).also { it.start() }
        } catch (e: Exception) {
            Hub.update { it.copy(message = "A porta ${Proto.TCP_PORT} está ocupada: ${e.message}") }
        }
        try {
            discovery = DiscoveryResponder(pairs.phoneId.hex(), ::phoneName).also { it.start() }
        } catch (e: Exception) {
            Hub.update { it.copy(message = "Descoberta automática indisponível; use o IP manual no PC.") }
        }
        if (orientationListener.canDetectOrientation()) orientationListener.enable()
        ctrl.postDelayed(statsTick, 1000)
        updateNotification()
    }

    private fun stopAll() {
        orientationListener.disable()
        ctrl.removeCallbacksAndMessages(null)
        server?.stop()
        discovery?.stop()
        session?.close()
        stopEncoder()
        camera?.release()
        renderer?.release()
        cameraSurface?.release()
        server = null
        discovery = null
        session = null
        camera = null
        renderer = null
        cameraSurface = null
        releaseLocks()
        getSystemService(NotificationManager::class.java).cancel(NOTIF_PAIR)
        Hub.update { Hub.Status() }
    }

    // ---------------------------------------------------------------- câmera

    fun cameras(): List<CameraCaps.CamInfo> = CameraCaps.cameras(getSystemService(CameraManager::class.java))

    private fun openCamera() {
        val r = renderer ?: return
        val st = surfaceTexture ?: return
        val cam = camera ?: return
        val mgr = getSystemService(CameraManager::class.java)
        val cams = CameraCaps.cameras(mgr)
        if (cams.isEmpty()) {
            Hub.update { it.copy(message = "Nenhuma câmera disponível.") }
            return
        }
        val info = cams.firstOrNull { it.id == settings.cameraId }
            ?: cams.firstOrNull { it.facing == CameraCharacteristics.LENS_FACING_BACK }
            ?: cams.first()
        settings.cameraId = info.id
        val c = mgr.getCameraCharacteristics(info.id)

        val fpsList = CameraCaps.supportedFps(c)
        if (settings.fps !in fpsList) settings.fps = if (30 in fpsList) 30 else fpsList.max()
        val resolutions = CameraCaps.resolutions(c, settings.fps)
        val res = resolutions.firstOrNull { it.width == settings.width && it.height == settings.height }
            ?: resolutions.filter { it.height <= settings.height }.maxByOrNull { it.height }
            ?: resolutions.first()
        settings.width = res.width
        settings.height = res.height
        val camSize = CameraCaps.cameraSize(c, res.width, res.height, settings.fps, coverPortrait = settings.fill)
        camSizeFill = settings.fill

        camInfo = info
        characteristics = c
        realtimeClock = c.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) ==
            CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME

        st.setDefaultBufferSize(camSize.width, camSize.height)
        r.camW = camSize.width
        r.camH = camSize.height
        r.outW = res.width
        r.outH = res.height
        applyTransform()

        cameraSurface?.release()
        val surface = Surface(st)
        cameraSurface = surface
        cameraCount = cams.size
        cam.open(
            info.id, surface, CameraCaps.fpsRange(c, settings.fps, settings.lowLight),
            CameraController.Controls(
                zoom = settings.zoom,
                exposure = settings.exposure,
                awbMode = settings.awbMode,
                stabilization = settings.stabilization,
                iso = settings.iso,
                exposureNs = settings.exposureNs,
                focusDiopters = settings.focusDiopters,
                kelvin = settings.kelvin,
            ),
        )
        Hub.update {
            it.copy(
                video = "${info.label} • ${res.width}×${res.height} • ${settings.fps} fps • ${AppSettings.QUALITY_NAMES[settings.quality]}",
                cameraSize = "${camSize.width}×${camSize.height}",
            )
        }
    }

    private fun applyTransform() {
        val r = renderer ?: return
        val info = camInfo ?: return
        val base = if (info.facing == CameraCharacteristics.LENS_FACING_FRONT) {
            (info.sensorOrientation - deviceOrientation + 360) % 360
        } else {
            (info.sensorOrientation + deviceOrientation) % 360
        }
        r.rotation = (base + settings.rotationOffset) % 360
        r.mirror = settings.mirror
        r.fill = settings.fill
        applyPreviewTransform()
    }

    /** A prévia segue a rotação da tela (como um app de câmera); a frontal aparece espelhada. */
    fun applyPreviewTransform() {
        val r = renderer ?: return
        val info = camInfo ?: return
        val display = preview?.displayRotation ?: 0
        val front = info.facing == CameraCharacteristics.LENS_FACING_FRONT
        r.previewRotation = if (front) (info.sensorOrientation + display) % 360
        else (info.sensorOrientation - display + 360) % 360
        r.previewMirror = front
    }

    override fun onCameraReady() {
        Hub.update { if (it.message?.contains("câmera", ignoreCase = true) == true) it.copy(message = null) else it }
        sendCameraStateSoon()
    }

    override fun onControlsChanged() {
        camera?.controls?.let { settings.save(it) }
        Hub.update { it.copy(controlsRev = it.controlsRev + 1) }
        sendCameraStateSoon()
    }

    override fun onCameraError(message: String) {
        Hub.update { it.copy(message = "$message Tentando de novo…") }
        ctrl.removeCallbacks(retryCamera)
        ctrl.postDelayed(retryCamera, 3000)
    }

    private val retryCamera = Runnable {
        if (started) restartNow()
    }

    // ---------------------------------------------------------------- comandos da tela

    /** Reaplica resolução, FPS, qualidade ou câmera: reinicia câmera e encoder. */
    fun restartPipeline() {
        ctrl.post { restartNow() }
    }

    private fun restartNow() {
        switchStartedAt = SystemClock.elapsedRealtime()
        stopEncoder()
        camera?.close()
        openCamera()
        session?.let { startEncoder(it) }
    }

    /**
     * Reabre só a câmera (outra lente ou outra faixa de FPS). O encoder e a sessão continuam:
     * o PC segura o último quadro durante a troca e não precisa reiniciar o decodificador.
     * Se a câmera nova não aceitar a resolução ou o FPS atuais, reinicia tudo.
     */
    fun reopenCamera() {
        ctrl.post {
            val before = Triple(settings.width, settings.height, settings.fps)
            switchStartedAt = SystemClock.elapsedRealtime()
            camera?.close()
            openCamera()
            val s = session
            if (Triple(settings.width, settings.height, settings.fps) != before && s != null) startEncoder(s)
        }
    }

    override fun onFirstFrame() {
        val started = switchStartedAt
        if (started == 0L) return
        switchStartedAt = 0L
        val ms = SystemClock.elapsedRealtime() - started
        Hub.update { it.copy(lastSwitchMs = ms) }
    }

    fun switchCamera() {
        val cams = cameras()
        if (cams.size < 2) return
        val i = cams.indexOfFirst { it.id == settings.cameraId }
        selectCamera(cams[(i + 1) % cams.size].id)
    }

    /** Troca de câmera: zoom e foco manual são da lente, então voltam ao padrão. */
    fun selectCamera(id: String) {
        settings.cameraId = id
        settings.zoom = 1f
        settings.focusDiopters = null
        reopenCamera()
    }

    /** Qualidade (bitrate) muda na hora, sem reiniciar câmera nem encoder. */
    fun applyQuality() {
        ctrl.post {
            targetBitrate = settings.bitrate()
            encoder?.let { setEncoderBitrate(it, targetBitrate) }
            Hub.update {
                it.copy(video = it.video.substringBeforeLast(" • ") + " • ${AppSettings.QUALITY_NAMES[settings.quality]}")
            }
        }
    }

    fun updateTransform() {
        ctrl.post {
            if (settings.fill != camSizeFill && camera != null) reopenCamera() else applyTransform()
        }
    }

    fun updateControls(transform: (CameraController.Controls) -> CameraController.Controls) {
        camera?.update(transform)
    }

    fun controls(): CameraController.Controls = camera?.controls ?: CameraController.Controls()

    fun focusAt(x: Float, y: Float): Boolean {
        val p = renderer?.previewToSensor(x, y) ?: return false
        camera?.update { it.copy(focusPoint = p, focusDiopters = null) }
        return true
    }

    /** Valores automáticos que a câmera está usando agora (ponto de partida do modo manual). */
    fun liveValues(): CameraController.Live = camera?.live ?: CameraController.Live()

    /** Teto do tempo de exposição manual (um quadro no FPS atual). */
    fun frameNs(): Long = camera?.frameNs ?: (1_000_000_000L / settings.fps)

    /** Temperatura estimada do balanço automático, para o Kelvin manual começar sem salto. */
    fun autoKelvin(): Int? {
        val c = characteristics ?: return null
        val g = liveValues().awbGains ?: return null
        return WhiteBalance.estimateKelvin(c, g)
    }

    // ---------------------------------------------------------------- controle pelo PC

    override fun onRemoteControl(key: Int, value: Int) {
        ctrl.post {
            val c = characteristics ?: return@post
            when (key) {
                Proto.CTL_SWITCH_CAMERA -> switchCamera()
                Proto.CTL_TORCH -> updateControls { it.copy(torch = value != 0) }
                Proto.CTL_ZOOM -> {
                    val range = CameraCaps.zoomRange(c)
                    updateControls { it.copy(zoom = (value / 100f).coerceIn(range.lower, range.upper)) }
                }
                Proto.CTL_EV -> updateControls { it.copy(exposure = value) }
                Proto.CTL_ISO -> updateControls { it.copy(iso = value.takeIf { v -> v > 0 }) }
                Proto.CTL_EXPOSURE_US -> updateControls { it.copy(exposureNs = value.takeIf { v -> v > 0 }?.times(1000L)) }
                Proto.CTL_FOCUS -> updateControls {
                    it.copy(focusDiopters = if (value < 0) null else value / 100f, focusPoint = null)
                }
                Proto.CTL_KELVIN -> updateControls { it.copy(kelvin = value.takeIf { v -> v > 0 }) }
                Proto.CTL_ALL_AUTO -> updateControls {
                    it.copy(
                        exposure = 0, aeLock = false, focusPoint = null,
                        iso = null, exposureNs = null, focusDiopters = null, kelvin = null,
                    )
                }
            }
        }
    }

    private val sendCameraState = Runnable {
        val s = session ?: return@Runnable
        val c = characteristics ?: return@Runnable
        val ctl = controls()
        val manual = CameraCaps.manual(c)
        val zoom = CameraCaps.zoomRange(c)
        val ev = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE) ?: android.util.Range(0, 0)
        val evStep = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toDouble() ?: 1.0
        val iso = manual.iso
        val exp = manual.exposureNs
        val frame = frameNs()
        val flags = (if (c.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true) 1 else 0) or
            (if (ctl.torch) 2 else 0) or
            (if (iso != null && exp != null) 4 else 0) or
            (if (manual.focusDiopters > 0f) 8 else 0) or
            (if (manual.kelvin) 16 else 0) or
            (if (manual.focusCalibrated) 32 else 0)
        s.sendCameraState(
            Writer().str(camInfo?.label ?: "Câmera").u8(cameraCount).u8(flags)
                .u16((ctl.zoom * 100).toInt()).u16((zoom.lower * 100).toInt()).u16((zoom.upper * 100).toInt())
                .u16(ctl.exposure and 0xffff).u16(ev.lower and 0xffff).u16(ev.upper and 0xffff)
                .u16(if (evStep > 0) (1 / evStep).toInt().coerceIn(1, 1000) else 1)
                .u16(ctl.iso ?: 0).u16(iso?.lower ?: 0).u16(iso?.upper ?: 0)
                .u32(((ctl.exposureNs ?: 0L) / 1000).toInt()).u32(((exp?.lower ?: 0L) / 1000).toInt())
                .u32((minOf(exp?.upper ?: 0L, frame) / 1000).toInt())
                .u16(ctl.focusDiopters?.let { (it * 100).toInt() } ?: 0xffff)
                .u16((manual.focusDiopters * 100).toInt())
                .u16(ctl.kelvin ?: 0)
                .build(),
        )
    }

    private fun sendCameraStateSoon() {
        ctrl.removeCallbacks(sendCameraState)
        ctrl.postDelayed(sendCameraState, 50)
    }

    fun forgetPairs() {
        pairs.clear()
        session?.close()
    }

    // ---------------------------------------------------------------- sessão e encoder

    private fun nowUs(): Long =
        if (realtimeClock) SystemClock.elapsedRealtimeNanos() / 1000 else System.nanoTime() / 1000

    private fun startEncoder(s: ClientSession) {
        val r = renderer ?: return
        stopEncoder()
        targetBitrate = settings.bitrate()
        val w = settings.width
        val h = settings.height
        val fps = settings.fps
        val enc = VideoEncoder(w, h, fps, targetBitrate, ::nowUs) { s.offerVideo(it) }
        try {
            enc.start()
        } catch (e: Exception) {
            enc.stop()
            Hub.update { it.copy(message = "O encoder recusou ${w}×$h a $fps fps. Escolha outra resolução.") }
            return
        }
        encoder = enc
        targetBitrate = enc.bitrate
        s.bitrate = enc.bitrate
        encodeFps = fps
        s.sendConfig(
            Writer().u16(Proto.CODEC_H264).u16(w).u16(h).u16(fps).u32(enc.bitrate)
                .str("${camInfo?.label ?: "Câmera"} • ${enc.codecName}").build(),
        )
        r.setEncoderSurface(enc.inputSurface, w, h, fps)
    }

    private fun stopEncoder() {
        renderer?.setEncoderSurface(null, 0, 0, 0)
        encoder?.stop()
        encoder = null
    }

    override fun onSessionStarted(session: ClientSession) {
        ctrl.post {
            this.session?.takeIf { it !== session }?.close()
            this.session = session
            discovery?.announce = false
            session.listener = this
            session.clock = ::nowUs
            startEncoder(session)
            sendCameraStateSoon()
            Thread {
                // Espera alguns quadros para o relatório incluir a matriz real da câmera.
                Thread.sleep(1500)
                val r = renderer
                val extra = buildString {
                    appendLine()
                    appendLine("== Sessão")
                    appendLine("Câmera atual: ${camInfo?.label} (id ${camInfo?.id}, sensor ${camInfo?.sensorOrientation}°)")
                    appendLine("Buffer: ${r?.camW}x${r?.camH} • saída ${settings.width}x${settings.height} @ ${settings.fps}")
                    appendLine("Rotação saída/prévia: ${r?.rotation}° / ${r?.previewRotation}° • orientação do celular $deviceOrientation°")
                    appendLine("Matriz da SurfaceTexture: ${r?.matrixInfo}")
                }
                session.sendReport(DeviceReport.build(this) + extra)
            }.start()
            Hub.update { it.copy(pcName = session.pcName, message = null) }
            updateNotification()
        }
    }

    override fun onSessionEnded(session: ClientSession) {
        ctrl.post {
            if (this.session === session) {
                this.session = null
                discovery?.announce = true
                stopEncoder()
                Hub.update { it.copy(pcName = null, stats = "") }
                updateNotification()
            }
        }
    }

    override fun onKeyframeRequest() {
        encoder?.requestKeyframe()
    }

    /**
     * A rede não acompanhou: baixa o bitrate primeiro (a imagem fica um pouco mais suave, mas
     * fluida). Só corta FPS quando o bitrate já está no piso, porque menos quadros é o que
     * mais se nota numa chamada.
     */
    override fun onCongestion(severe: Boolean) {
        ctrl.post {
            val e = encoder ?: return@post
            lastCongestion = SystemClock.elapsedRealtime()
            val floor = targetBitrate / 4
            val atFloor = e.bitrate <= floor
            val reduced = maxOf(floor, if (severe) e.bitrate * 6 / 10 else e.bitrate * 8 / 10)
            if (reduced < e.bitrate) setEncoderBitrate(e, reduced)
            if (atFloor) {
                val fps = maxOf(MIN_ENCODE_FPS, encodeFps * 3 / 4)
                if (fps < encodeFps) {
                    encodeFps = fps
                    renderer?.setEncodeFps(fps)
                }
            }
        }
    }

    private fun setEncoderBitrate(e: VideoEncoder, value: Int) {
        e.setBitrate(value)
        session?.bitrate = value
    }

    private val statsTick = object : Runnable {
        override fun run() {
            val r = renderer ?: return
            val camFps = r.renderedFrames.getAndSet(0)
            val encFps = r.encodedFrames.getAndSet(0)
            val s = session
            val e = encoder
            val temp = batteryTenthsC()
            val thermal = if (Build.VERSION.SDK_INT >= 29) getSystemService(PowerManager::class.java).currentThermalStatus else 0
            val tempText = if (temp != Int.MIN_VALUE) " • ${"%.1f".format(temp / 10.0)} °C" else ""
            val stats = if (s != null && e != null) {
                val bits = s.bytesSent.getAndSet(0) * 8
                val dropped = s.framesDropped.getAndSet(0)
                val encDelay = e.takeEncodeDelayMs()
                val netDelay = s.takeNetDelayMs()
                s.sendStats(
                    Writer().u16(camFps).u16(encFps).u32(bits.toInt()).u32(e.bitrate).u16(dropped.toInt())
                        .u16(s.queuedFrames()).u16(encDelay.coerceIn(-1, 30000) and 0xffff)
                        .u16(temp.coerceIn(-32768, 32767) and 0xffff).u8(thermal)
                        .also { wifiStats(it) }.u8(encodeFps)
                        .u16(netDelay.coerceIn(-1, 30000) and 0xffff).build(),
                )
                // Recupera aos poucos depois de 2 s sem congestionamento: primeiro FPS, depois bitrate.
                if (SystemClock.elapsedRealtime() - lastCongestion > 2000) {
                    if (encodeFps < settings.fps) {
                        encodeFps = min(settings.fps, encodeFps + 5)
                        r.setEncodeFps(encodeFps)
                    } else if (e.bitrate < targetBitrate) {
                        // Rede confirmadamente rápida: sobe mais depressa (de 1/4 ao alvo em ~5 s).
                        val step = if (netDelay in 0..FAST_RECOVERY_DELAY_MS) 135 else 110
                        // Em Long: 4K passa de 16 Mbps e e.bitrate × 135 estoura o Int.
                        val raised = e.bitrate.toLong() * step / 100
                        setEncoderBitrate(e, min(targetBitrate.toLong(), raised).toInt())
                    }
                }
                "Enviando $encFps fps • ${"%.1f".format(bits / 1e6)} Mbps" +
                    (if (encDelay >= 0) " • codificação $encDelay ms" else "") +
                    (if (netDelay >= 0) " • rede $netDelay ms" else "") + tempText +
                    if (dropped > 0) " • $dropped quadros descartados" else ""
            } else {
                "Câmera $camFps fps$tempText"
            }
            val refreshAddresses = statsTicks++ % 5 == 0
            Hub.update { st ->
                st.copy(stats = stats, addresses = if (refreshAddresses) localAddresses() else st.addresses)
            }
            ctrl.postDelayed(this, 1000)
        }
    }

    // ---------------------------------------------------------------- servidor

    override fun onPairingCode(code: String?, pcName: String?) {
        Hub.update { it.copy(pairingCode = code, pairingPc = pcName) }
        val nm = getSystemService(NotificationManager::class.java)
        if (code == null) {
            nm.cancel(NOTIF_PAIR)
            return
        }
        val n = NotificationCompat.Builder(this, CH_PAIR)
            .setSmallIcon(R.drawable.ic_stat)
            .setContentTitle("Código de pareamento: ${code.take(3)} ${code.drop(3)}")
            .setContentText("Digite no GodoiCam do PC \"$pcName\"")
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setContentIntent(openAppIntent())
            .setAutoCancel(true)
            .build()
        try {
            nm.notify(NOTIF_PAIR, n)
        } catch (_: SecurityException) {
        }
    }

    override fun onPaired(pcName: String) {
        Hub.update { it.copy(message = "Pareado com $pcName.") }
    }

    override fun onServerMessage(message: String) {
        Hub.update { it.copy(message = message) }
    }

    // ---------------------------------------------------------------- utilidades

    /** Frequência (MHz), sinal (dBm), velocidade do link (Mbps) e tela ligada, para o diagnóstico. */
    @Suppress("DEPRECATION")
    private fun wifiStats(w: Writer) {
        val info = try {
            applicationContext.getSystemService(WifiManager::class.java).connectionInfo
        } catch (_: Exception) {
            null
        }
        val link = when {
            info == null -> 0
            Build.VERSION.SDK_INT >= 29 -> info.txLinkSpeedMbps
            else -> info.linkSpeed
        }
        w.u16((info?.frequency ?: 0).coerceIn(0, 65535)).u16((info?.rssi ?: 0) and 0xffff)
            .u16(link.coerceIn(0, 65535))
            .u8(if (getSystemService(PowerManager::class.java).isInteractive) 1 else 0)
    }

    /** Temperatura da bateria em décimos de °C (Int.MIN_VALUE se indisponível). */
    private fun batteryTenthsC(): Int =
        registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            ?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE) ?: Int.MIN_VALUE

    private fun phoneName(): String =
        Settings.Global.getString(contentResolver, "device_name")?.takeIf { it.isNotBlank() }
            ?: "${Build.MANUFACTURER} ${Build.MODEL}"

    private fun localAddresses(): String = try {
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.inetAddresses.toList() }
            .filterIsInstance<Inet4Address>()
            .joinToString(", ") { it.hostAddress ?: "" }
    } catch (_: Exception) {
        ""
    }

    private fun acquireLocks() {
        val pm = getSystemService(PowerManager::class.java)
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "GodoiCam:stream").apply {
            setReferenceCounted(false)
            acquire()
        }
        val wifi = applicationContext.getSystemService(WifiManager::class.java)
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wifi.createWifiLock(mode, "GodoiCam").apply {
            setReferenceCounted(false)
            acquire()
        }
        // O modo de baixa latência só vale com a tela ligada; este mantém o Wi-Fi sem economia
        // de energia também com a tela apagada.
        @Suppress("DEPRECATION")
        wifiPerfLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "GodoiCam-perf").apply {
            setReferenceCounted(false)
            acquire()
        }
        multicastLock = wifi.createMulticastLock("GodoiCam").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun releaseLocks() {
        wakeLock?.release()
        wifiLock?.release()
        wifiPerfLock?.release()
        wifiPerfLock = null
        multicastLock?.release()
        wakeLock = null
        wifiLock = null
        multicastLock = null
    }

    private fun createChannels() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CH_STREAM, "Webcam ativa", NotificationManager.IMPORTANCE_LOW),
        )
        nm.createNotificationChannel(
            NotificationChannel(CH_PAIR, "Pareamento", NotificationManager.IMPORTANCE_HIGH),
        )
    }

    private fun openAppIntent(): PendingIntent = PendingIntent.getActivity(
        this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
    )

    private fun notification(text: String): Notification {
        val stop = PendingIntent.getService(
            this, 1, Intent(this, StreamService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CH_STREAM)
            .setSmallIcon(R.drawable.ic_stat)
            .setContentTitle("GodoiCam")
            .setContentText(text)
            .setOngoing(true)
            .setContentIntent(openAppIntent())
            .addAction(0, "Desligar", stop)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()
    }

    private fun updateNotification() {
        val text = session?.let { "Câmera em uso por ${it.pcName}" } ?: "Aguardando o PC"
        getSystemService(NotificationManager::class.java).notify(NOTIF_STREAM, notification(text))
    }
}

package com.godoi.godoican

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.net.wifi.WifiManager
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
import com.godoi.godoican.media.CameraCaps
import com.godoi.godoican.media.CameraController
import com.godoi.godoican.media.GlRenderer
import com.godoi.godoican.media.VideoEncoder
import com.godoi.godoican.net.ClientSession
import com.godoi.godoican.net.DiscoveryResponder
import com.godoi.godoican.net.PairStore
import com.godoi.godoican.net.Proto
import com.godoi.godoican.net.StreamServer
import com.godoi.godoican.net.Writer
import com.godoi.godoican.net.hex
import java.net.Inet4Address
import java.net.NetworkInterface
import kotlin.math.abs
import kotlin.math.min

/**
 * Serviço em primeiro plano (tipo câmera) que mantém a webcam funcionando com a tela
 * desligada: câmera → GPU → encoder → sessão com o PC.
 */
class StreamService : Service(), StreamServer.Callbacks, ClientSession.Listener, CameraController.Listener {

    class PreviewTarget(val surface: Surface, val width: Int, val height: Int)

    companion object {
        @Volatile var instance: StreamService? = null
            private set

        @Volatile private var preview: PreviewTarget? = null

        const val ACTION_STOP = "com.godoi.godoican.STOP"
        private const val NOTIF_STREAM = 1
        private const val NOTIF_PAIR = 2
        private const val CH_STREAM = "stream"
        private const val CH_PAIR = "pair"

        fun start(context: Context) {
            ContextCompat.startForegroundService(context, Intent(context, StreamService::class.java))
        }

        /** Chamado pela tela. Síncrono: ao retornar, a GPU já soltou a Surface antiga. */
        fun setPreview(target: PreviewTarget?) {
            preview = target
            instance?.renderer?.setPreviewSurface(target?.surface, target?.width ?: 0, target?.height ?: 0)
        }
    }

    lateinit var settings: AppSettings
        private set
    lateinit var pairs: PairStore
        private set

    private val ctrlThread = HandlerThread("GodoiCan-ctrl").apply { start() }
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
    @Volatile private var realtimeClock = false
    private var started = false

    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    private var targetBitrate = 0
    private var lastCongestion = 0L
    private var statsTicks = 0

    @Volatile private var deviceOrientation = 0
    private val orientationListener by lazy {
        object : OrientationEventListener(this) {
            override fun onOrientationChanged(orientation: Int) {
                if (orientation == ORIENTATION_UNKNOWN || !settings.autoRotate) return
                val d = abs(orientation - deviceOrientation) % 360
                if (min(d, 360 - d) > 55) {
                    deviceOrientation = ((orientation + 45) / 90 * 90) % 360
                    ctrl.post { applyTransform() }
                }
            }
        }
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
        val camSize = CameraCaps.cameraSize(c, res.width, res.height, settings.fps)

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
        cam.open(
            info.id, surface, CameraCaps.fpsRange(c, settings.fps, settings.lowLight),
            CameraController.Controls(
                zoom = settings.zoom,
                exposure = settings.exposure,
                awbMode = settings.awbMode,
                stabilization = settings.stabilization,
            ),
        )
        Hub.update {
            it.copy(video = "${info.label} • ${res.width}×${res.height} • ${settings.fps} fps • ${AppSettings.QUALITY_NAMES[settings.quality]}")
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
    }

    override fun onCameraReady() {
        Hub.update { if (it.message?.contains("câmera", ignoreCase = true) == true) it.copy(message = null) else it }
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
        stopEncoder()
        camera?.close()
        openCamera()
        session?.let { startEncoder(it) }
    }

    fun switchCamera() {
        val cams = cameras()
        if (cams.size < 2) return
        val i = cams.indexOfFirst { it.id == settings.cameraId }
        settings.cameraId = cams[(i + 1) % cams.size].id
        settings.zoom = 1f
        restartPipeline()
    }

    fun updateTransform() {
        ctrl.post { applyTransform() }
    }

    fun updateControls(transform: (CameraController.Controls) -> CameraController.Controls) {
        camera?.update(transform)
    }

    fun controls(): CameraController.Controls = camera?.controls ?: CameraController.Controls()

    fun focusAt(x: Float, y: Float): Boolean {
        val p = renderer?.previewToSensor(x, y) ?: return false
        camera?.update { it.copy(focusPoint = p) }
        return true
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
        val enc = VideoEncoder(w, h, fps, targetBitrate) { s.offerVideo(it) }
        try {
            enc.start()
        } catch (e: Exception) {
            enc.stop()
            Hub.update { it.copy(message = "O encoder recusou ${w}×$h a $fps fps. Escolha outra resolução.") }
            return
        }
        encoder = enc
        targetBitrate = enc.bitrate
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
            session.listener = this
            session.clock = ::nowUs
            startEncoder(session)
            Hub.update { it.copy(pcName = session.pcName, message = null) }
            updateNotification()
        }
    }

    override fun onSessionEnded(session: ClientSession) {
        ctrl.post {
            if (this.session === session) {
                this.session = null
                stopEncoder()
                Hub.update { it.copy(pcName = null, stats = "") }
                updateNotification()
            }
        }
    }

    override fun onKeyframeRequest() {
        encoder?.requestKeyframe()
    }

    /** A rede não acompanhou: baixa o bitrate (até 1/4 do alvo) e pede um quadro-chave. */
    override fun onCongestion() {
        ctrl.post {
            val e = encoder ?: return@post
            lastCongestion = SystemClock.elapsedRealtime()
            val reduced = maxOf(targetBitrate / 4, e.bitrate * 7 / 10)
            if (reduced < e.bitrate) e.setBitrate(reduced)
            e.requestKeyframe()
        }
    }

    private val statsTick = object : Runnable {
        override fun run() {
            val r = renderer ?: return
            val camFps = r.renderedFrames.getAndSet(0)
            val encFps = r.encodedFrames.getAndSet(0)
            val s = session
            val e = encoder
            val stats = if (s != null && e != null) {
                val kbps = s.bytesSent.getAndSet(0) * 8 / 1000
                val dropped = s.framesDropped.getAndSet(0)
                // Recupera o bitrate aos poucos depois de 4 s sem congestionamento.
                if (e.bitrate < targetBitrate && SystemClock.elapsedRealtime() - lastCongestion > 4000) {
                    e.setBitrate(min(targetBitrate, e.bitrate * 115 / 100))
                }
                "Enviando $encFps fps • ${"%.1f".format(kbps / 1000.0)} Mbps" +
                    if (dropped > 0) " • $dropped quadros descartados" else ""
            } else {
                "Câmera $camFps fps"
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
            .setContentText("Digite no GodoiCan do PC \"$pcName\"")
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
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "GodoiCan:stream").apply {
            setReferenceCounted(false)
            acquire()
        }
        val wifi = applicationContext.getSystemService(WifiManager::class.java)
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wifi.createWifiLock(mode, "GodoiCan").apply {
            setReferenceCounted(false)
            acquire()
        }
        multicastLock = wifi.createMulticastLock("GodoiCan").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun releaseLocks() {
        wakeLock?.release()
        wifiLock?.release()
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
            .setContentTitle("GodoiCan")
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

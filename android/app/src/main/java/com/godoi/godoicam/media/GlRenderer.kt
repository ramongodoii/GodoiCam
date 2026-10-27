package com.godoi.godoicam.media

import android.graphics.PointF
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.max
import kotlin.math.min

/**
 * Recebe os quadros da câmera numa textura externa e desenha, na GPU:
 * - na Surface do encoder: a imagem enviada ao PC (16:9, girada conforme o celular);
 * - na prévia: a câmera inteira, sempre em pé como num app de câmera, com a área que
 *   NÃO vai para o PC escurecida.
 * Nenhum quadro passa pela CPU.
 */
class GlRenderer {
    /**
     * Como a imagem da câmera ocupa uma área: rotação horária, espelho e preencher
     * (corta) ou encaixar (faixas). Converte entre coordenadas da área (NDC, -1..1) e
     * coordenadas da textura da câmera (uv, 0..1, antes da matriz da SurfaceTexture).
     */
    private class Placement(
        val rotation: Int,
        val mirror: Boolean,
        fill: Boolean,
        camW: Int,
        camH: Int,
        areaW: Int,
        areaH: Int,
    ) {
        val sx: Float
        val sy: Float

        init {
            val content = if (rotation % 180 == 0) camW.toFloat() / camH else camH.toFloat() / camW
            val target = areaW.toFloat() / max(areaH, 1)
            var x = 1f
            var y = 1f
            if (fill) {
                if (content > target) x = content / target else y = target / content
            } else {
                if (content > target) y = target / content else x = content / target
            }
            sx = x
            sy = y
        }

        /** Canto da área (0..1, y para cima) → uv. */
        fun uv(cx: Float, cy: Float): FloatArray {
            val mx = if (mirror) 1f - cx else cx
            return when (rotation) {
                90 -> floatArrayOf(1f - cy, mx)
                180 -> floatArrayOf(1f - mx, 1f - cy)
                270 -> floatArrayOf(cy, 1f - mx)
                else -> floatArrayOf(mx, cy)
            }
        }

        fun ndcToUv(px: Float, py: Float): FloatArray = uv((px / sx + 1) / 2, (py / sy + 1) / 2)

        fun uvToNdc(u: Float, v: Float): FloatArray {
            var (cx, cy) = when (rotation) {
                90 -> v to 1f - u
                180 -> 1f - u to 1f - v
                270 -> 1f - v to u
                else -> u to v
            }
            if (mirror) cx = 1f - cx
            return floatArrayOf((cx * 2 - 1) * sx, (cy * 2 - 1) * sy)
        }
    }

    private val thread = HandlerThread("GodoiCam-gl").apply { start() }
    private val handler = Handler(thread.looper)

    private lateinit var egl: EglCore
    private lateinit var idle: EGLSurface
    private var program = 0
    private var aPos = 0
    private var aUv = 0
    private var uTex = 0
    private var solid = 0
    private var sPos = 0
    private var sColor = 0
    private var texId = 0
    private lateinit var surfaceTexture: SurfaceTexture
    private val texMatrix = FloatArray(16)
    private val rawMatrix = FloatArray(16)

    /** Última matriz da SurfaceTexture, para o relatório de diagnóstico. */
    @Volatile var matrixInfo = ""
        private set
    private val vertices = ByteBuffer.allocateDirect(16 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()

    private var encSurface: EGLSurface? = null
    private var encW = 0
    private var encH = 0
    @Volatile private var encIntervalNs = 0L
    private var nextEncTs = 0L

    private var prevSurface: EGLSurface? = null
    private var prevTarget: Surface? = null
    private var prevW = 0
    private var prevH = 0

    // Saída para o PC.
    @Volatile var rotation = 0
    @Volatile var mirror = false
    @Volatile var fill = true
    // Prévia na tela (rotação do display, espelho na frontal).
    @Volatile var previewRotation = 90
    @Volatile var previewMirror = false
    @Volatile var camW = 1920
    @Volatile var camH = 1080
    @Volatile var outW = 1920
    @Volatile var outH = 1080

    val renderedFrames = AtomicInteger()
    val encodedFrames = AtomicInteger()

    private fun <T> sync(block: () -> T): T {
        if (Looper.myLooper() == thread.looper) return block()
        var result: Result<T>? = null
        val latch = CountDownLatch(1)
        handler.post {
            result = runCatching(block)
            latch.countDown()
        }
        latch.await()
        return result!!.getOrThrow()
    }

    fun start(): SurfaceTexture = sync {
        egl = EglCore()
        idle = egl.createPbuffer(1, 1)
        egl.makeCurrent(idle)
        program = buildProgram(VERTEX, FRAGMENT)
        aPos = GLES20.glGetAttribLocation(program, "aPos")
        aUv = GLES20.glGetAttribLocation(program, "aUv")
        uTex = GLES20.glGetUniformLocation(program, "uTex")
        solid = buildProgram(SOLID_VERTEX, SOLID_FRAGMENT)
        sPos = GLES20.glGetAttribLocation(solid, "aPos")
        sColor = GLES20.glGetUniformLocation(solid, "uColor")
        val ids = IntArray(1)
        GLES20.glGenTextures(1, ids, 0)
        texId = ids[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        surfaceTexture = SurfaceTexture(texId)
        surfaceTexture.setOnFrameAvailableListener({ onFrame() }, handler)
        surfaceTexture
    }

    fun setEncoderSurface(surface: Surface?, width: Int, height: Int, fps: Int) = sync {
        encSurface?.let {
            egl.makeCurrent(idle)
            egl.destroySurface(it)
        }
        encSurface = surface?.let { egl.createWindowSurface(it) }
        encW = width
        encH = height
        encIntervalNs = if (fps > 0) 1_000_000_000L / fps else 0
        nextEncTs = 0
    }

    /** Codifica no máximo [fps] quadros por segundo (o resto da câmera vai só para a prévia). */
    fun setEncodeFps(fps: Int) {
        encIntervalNs = if (fps > 0) 1_000_000_000L / fps else 0
    }

    /**
     * Decide se o quadro [ts] vai para o encoder. Agenda pelo relógio ideal (e não pelo último
     * quadro aceito), para 20 fps de uma câmera a 30 sair como 2 de cada 3, e não 1 de cada 2.
     */
    private fun takeForEncoder(ts: Long): Boolean {
        val interval = encIntervalNs
        if (interval <= 0) return true
        if (ts + interval / 4 < nextEncTs) return false
        nextEncTs = if (ts - nextEncTs > interval) ts + interval else nextEncTs + interval
        return true
    }

    /** Mesma Surface com outro tamanho (giro da tela): só atualiza o tamanho, sem recriar nada. */
    fun setPreviewSurface(surface: Surface?, width: Int, height: Int) = sync {
        if (surface != null && surface === prevTarget && prevSurface != null) {
            prevW = width
            prevH = height
            return@sync
        }
        prevSurface?.let {
            egl.makeCurrent(idle)
            egl.destroySurface(it)
        }
        prevSurface = null
        prevTarget = null
        if (surface != null && surface.isValid) {
            try {
                val s = egl.createWindowSurface(surface)
                egl.makeCurrent(s)
                EGL14.eglSwapInterval(egl.display, 0) // a prévia nunca pode segurar o encoder
                prevSurface = s
                prevTarget = surface
            } catch (_: Exception) {
            }
        }
        prevW = width
        prevH = height
    }

    private fun outputPlacement() = Placement(rotation, mirror, fill, camW, camH, outW, outH)

    private fun previewPlacement() = Placement(previewRotation, previewMirror, false, camW, camH, prevW, prevH)

    private fun onFrame() {
        try {
            egl.makeCurrent(idle)
            surfaceTexture.updateTexImage()
        } catch (_: Exception) {
            return
        }
        surfaceTexture.getTransformMatrix(rawMatrix)
        useBufferOrientation(rawMatrix, texMatrix)
        val ts = surfaceTexture.timestamp
        renderedFrames.incrementAndGet()

        // Encoder primeiro: a prévia é secundária.
        encSurface?.let { s ->
            if (takeForEncoder(ts)) {
                egl.makeCurrent(s)
                GLES20.glViewport(0, 0, encW, encH)
                GLES20.glClearColor(0f, 0f, 0f, 1f)
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
                drawCamera(Placement(rotation, mirror, fill, camW, camH, encW, encH))
                egl.setPresentationTime(s, ts)
                egl.swap(s)
                encodedFrames.incrementAndGet()
            }
        }

        prevSurface?.let { s ->
            if (!egl.makeCurrent(s)) return
            GLES20.glViewport(0, 0, prevW, prevH)
            GLES20.glClearColor(0f, 0f, 0f, 1f)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
            val p = previewPlacement()
            drawCamera(p)
            drawSentArea(p)
            if (!egl.swap(s)) {
                egl.makeCurrent(idle)
                egl.destroySurface(s)
                prevSurface = null
                prevTarget = null
            }
        }
    }

    /**
     * Alguns aparelhos (ex.: Samsung com Android 16) colocam a rotação do sensor dentro da
     * matriz da SurfaceTexture. Todo o cálculo de rotação e proporção aqui parte da imagem
     * como o sensor a entrega, então essa rotação é descartada: fica só a inversão vertical
     * (e o recorte de borda, quando não há rotação).
     */
    private fun useBufferOrientation(raw: FloatArray, out: FloatArray) {
        val plain = raw[0] > 0.5f && kotlin.math.abs(raw[1]) < 0.01f && kotlin.math.abs(raw[4]) < 0.01f && raw[5] < -0.5f
        if (plain) {
            raw.copyInto(out)
        } else {
            out.fill(0f)
            out[0] = 1f   // s = u
            out[5] = -1f  // t = 1 - v
            out[10] = 1f
            out[13] = 1f
            out[15] = 1f
        }
        if (matrixInfo.isEmpty() || renderedFrames.get() % 300 == 0) {
            matrixInfo = "%.2f %.2f / %.2f %.2f / desloc %.2f %.2f%s".format(
                raw[0], raw[4], raw[1], raw[5], raw[12], raw[13],
                if (plain) "" else " (rotação embutida descartada)",
            )
        }
    }

    private fun drawCamera(p: Placement) {
        vertices.clear()
        for ((cx, cy) in CORNERS) {
            val uv = p.uv(cx, cy)
            vertices.put((cx * 2 - 1) * p.sx).put((cy * 2 - 1) * p.sy).put(uv[0]).put(uv[1])
        }
        vertices.position(0)
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 16, vertices)
        GLES20.glEnableVertexAttribArray(aPos)
        vertices.position(2)
        GLES20.glVertexAttribPointer(aUv, 2, GLES20.GL_FLOAT, false, 16, vertices)
        GLES20.glEnableVertexAttribArray(aUv)
        GLES20.glUniformMatrix4fv(uTex, 1, false, texMatrix, 0)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
    }

    /** Retângulo da prévia (NDC) que corresponde ao que o PC recebe. */
    private fun sentRect(p: Placement): FloatArray {
        val out = outputPlacement()
        var x0 = Float.MAX_VALUE
        var y0 = Float.MAX_VALUE
        var x1 = -Float.MAX_VALUE
        var y1 = -Float.MAX_VALUE
        for ((nx, ny) in listOf(-1f to -1f, 1f to -1f, -1f to 1f, 1f to 1f)) {
            val uv = out.ndcToUv(nx, ny)
            val q = p.uvToNdc(uv[0].coerceIn(0f, 1f), uv[1].coerceIn(0f, 1f))
            x0 = min(x0, q[0])
            y0 = min(y0, q[1])
            x1 = max(x1, q[0])
            y1 = max(y1, q[1])
        }
        return floatArrayOf(x0, y0, x1, y1)
    }

    /** Escurece, na prévia, tudo o que fica fora da imagem enviada ao PC. */
    private fun drawSentArea(p: Placement) {
        val r = sentRect(p)
        // Limites da imagem da câmera na prévia.
        val cx0 = -p.sx
        val cy0 = -p.sy
        val cx1 = p.sx
        val cy1 = p.sy
        val x0 = max(r[0], cx0)
        val y0 = max(r[1], cy0)
        val x1 = min(r[2], cx1)
        val y1 = min(r[3], cy1)
        if (x0 <= cx0 + 0.01f && y0 <= cy0 + 0.01f && x1 >= cx1 - 0.01f && y1 >= cy1 - 0.01f) return
        GLES20.glEnable(GLES20.GL_BLEND)
        GLES20.glBlendFunc(GLES20.GL_SRC_ALPHA, GLES20.GL_ONE_MINUS_SRC_ALPHA)
        GLES20.glUseProgram(solid)
        GLES20.glUniform4f(sColor, 0f, 0f, 0f, 0.6f)
        rect(cx0, y1, cx1, cy1) // acima
        rect(cx0, cy0, cx1, y0) // abaixo
        rect(cx0, y0, x0, y1) // esquerda
        rect(x1, y0, cx1, y1) // direita
        // Contorno fino da área enviada.
        GLES20.glUniform4f(sColor, 1f, 1f, 1f, 0.55f)
        val tx = 2f / max(prevW, 1) * 2
        val ty = 2f / max(prevH, 1) * 2
        rect(x0, y1 - ty, x1, y1)
        rect(x0, y0, x1, y0 + ty)
        rect(x0, y0, x0 + tx, y1)
        rect(x1 - tx, y0, x1, y1)
        GLES20.glDisable(GLES20.GL_BLEND)
    }

    private fun rect(x0: Float, y0: Float, x1: Float, y1: Float) {
        if (x1 <= x0 || y1 <= y0) return
        vertices.clear()
        vertices.put(x0).put(y0).put(x1).put(y0).put(x0).put(y1).put(x1).put(y1)
        vertices.position(0)
        GLES20.glVertexAttribPointer(sPos, 2, GLES20.GL_FLOAT, false, 8, vertices)
        GLES20.glEnableVertexAttribArray(sPos)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
    }

    /**
     * Converte um toque na prévia em coordenadas normalizadas do sensor (x para a direita,
     * y para baixo). Null se o toque caiu fora da imagem.
     */
    fun previewToSensor(px: Float, py: Float): PointF? {
        if (prevW == 0 || prevH == 0) return null
        val p = previewPlacement()
        val nx = px / prevW * 2 - 1
        val ny = 1 - py / prevH * 2
        if (nx < -p.sx || nx > p.sx || ny < -p.sy || ny > p.sy) return null
        val uv = p.ndcToUv(nx, ny)
        return PointF(uv[0], 1f - uv[1])
    }

    fun release() {
        sync {
            encSurface?.let { egl.destroySurface(it) }
            prevSurface?.let { egl.destroySurface(it) }
            encSurface = null
            prevSurface = null
            prevTarget = null
            surfaceTexture.release()
            egl.makeCurrent(idle)
            GLES20.glDeleteTextures(1, intArrayOf(texId), 0)
            GLES20.glDeleteProgram(program)
            GLES20.glDeleteProgram(solid)
            egl.destroySurface(idle)
            egl.release()
        }
        thread.quitSafely()
    }

    private fun buildProgram(vertex: String, fragment: String): Int {
        fun compile(type: Int, src: String): Int {
            val shader = GLES20.glCreateShader(type)
            GLES20.glShaderSource(shader, src)
            GLES20.glCompileShader(shader)
            val ok = IntArray(1)
            GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, ok, 0)
            check(ok[0] != 0) { "Shader: ${GLES20.glGetShaderInfoLog(shader)}" }
            return shader
        }
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, compile(GLES20.GL_VERTEX_SHADER, vertex))
        GLES20.glAttachShader(p, compile(GLES20.GL_FRAGMENT_SHADER, fragment))
        GLES20.glLinkProgram(p)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        check(ok[0] != 0) { "Programa GL: ${GLES20.glGetProgramInfoLog(p)}" }
        return p
    }

    private companion object {
        // Ordem do TRIANGLE_STRIP: baixo-esq, baixo-dir, cima-esq, cima-dir.
        val CORNERS = listOf(0f to 0f, 1f to 0f, 0f to 1f, 1f to 1f)

        const val VERTEX = """
            attribute vec2 aPos;
            attribute vec2 aUv;
            uniform mat4 uTex;
            varying highp vec2 vUv;
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
                vUv = (uTex * vec4(aUv, 0.0, 1.0)).xy;
            }
        """

        // highp nas coordenadas: com mediump a amostragem perde precisão em 1080p/4K.
        // Sem filtro extra aqui: um bilateral 5×5 na textura da câmera (4K) derrubou o
        // envio para 6–10 fps no S24 (stats 25/10 22:01).
        const val FRAGMENT = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            uniform samplerExternalOES sTex;
            varying highp vec2 vUv;
            void main() {
                gl_FragColor = texture2D(sTex, vUv);
            }
        """

        const val SOLID_VERTEX = """
            attribute vec2 aPos;
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
            }
        """

        const val SOLID_FRAGMENT = """
            precision mediump float;
            uniform vec4 uColor;
            void main() {
                gl_FragColor = uColor;
            }
        """
    }
}

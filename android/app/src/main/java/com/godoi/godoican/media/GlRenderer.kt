package com.godoi.godoican.media

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

/**
 * Recebe os quadros da câmera numa textura externa e desenha, na GPU, a mesma imagem
 * (girada, espelhada e enquadrada) na Surface do encoder e na prévia da tela.
 * Nenhum quadro passa pela CPU.
 */
class GlRenderer {
    private val thread = HandlerThread("GodoiCan-gl").apply { start() }
    private val handler = Handler(thread.looper)

    private lateinit var egl: EglCore
    private lateinit var idle: EGLSurface
    private var program = 0
    private var aPos = 0
    private var aUv = 0
    private var uTex = 0
    private var texId = 0
    private lateinit var surfaceTexture: SurfaceTexture
    private val texMatrix = FloatArray(16)
    private val vertices = ByteBuffer.allocateDirect(16 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()

    private var encSurface: EGLSurface? = null
    private var encW = 0
    private var encH = 0
    private var minEncIntervalNs = 0L
    private var lastEncTs = 0L

    private var prevSurface: EGLSurface? = null
    private var prevW = 0
    private var prevH = 0

    // Ajustes lidos pela thread GL a cada quadro.
    @Volatile var rotation = 0
    @Volatile var mirror = false
    @Volatile var fill = false
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
        program = buildProgram()
        aPos = GLES20.glGetAttribLocation(program, "aPos")
        aUv = GLES20.glGetAttribLocation(program, "aUv")
        uTex = GLES20.glGetUniformLocation(program, "uTex")
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
        minEncIntervalNs = if (fps > 0) 1_000_000_000L / fps * 8 / 10 else 0
        lastEncTs = 0
    }

    fun setPreviewSurface(surface: Surface?, width: Int, height: Int) = sync {
        prevSurface?.let {
            egl.makeCurrent(idle)
            egl.destroySurface(it)
        }
        prevSurface = null
        if (surface != null && surface.isValid) {
            try {
                val s = egl.createWindowSurface(surface)
                egl.makeCurrent(s)
                EGL14.eglSwapInterval(egl.display, 0) // a prévia nunca pode segurar o encoder
                prevSurface = s
            } catch (_: Exception) {
            }
        }
        prevW = width
        prevH = height
    }

    private fun onFrame() {
        try {
            egl.makeCurrent(idle)
            surfaceTexture.updateTexImage()
        } catch (_: Exception) {
            return
        }
        surfaceTexture.getTransformMatrix(texMatrix)
        val ts = surfaceTexture.timestamp
        renderedFrames.incrementAndGet()

        // Encoder primeiro: a prévia é secundária.
        encSurface?.let { s ->
            if (ts - lastEncTs >= minEncIntervalNs) {
                lastEncTs = ts
                egl.makeCurrent(s)
                GLES20.glViewport(0, 0, encW, encH)
                GLES20.glClearColor(0f, 0f, 0f, 1f)
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
                draw(0, 0, encW, encH)
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
            val box = letterbox(prevW, prevH)
            draw(box[0], box[1], box[2], box[3])
            if (!egl.swap(s)) {
                egl.makeCurrent(idle)
                egl.destroySurface(s)
                prevSurface = null
            }
        }
    }

    /** Área (x, y, w, h) da imagem de saída dentro da prévia, mantendo a proporção. */
    private fun letterbox(viewW: Int, viewH: Int): IntArray {
        val outAspect = outW.toFloat() / outH
        val viewAspect = viewW.toFloat() / viewH
        return if (viewAspect > outAspect) {
            val w = (viewH * outAspect).toInt()
            intArrayOf((viewW - w) / 2, 0, w, viewH)
        } else {
            val h = (viewW / outAspect).toInt()
            intArrayOf(0, (viewH - h) / 2, viewW, h)
        }
    }

    /** Escala do quadrilátero da câmera dentro de uma área com proporção [targetAspect]. */
    private fun contentScale(targetAspect: Float): FloatArray {
        val rot = rotation
        val content = if (rot % 180 == 0) camW.toFloat() / camH else camH.toFloat() / camW
        var sx = 1f
        var sy = 1f
        if (fill) {
            if (content > targetAspect) sx = content / targetAspect else sy = targetAspect / content
        } else {
            if (content > targetAspect) sy = targetAspect / content else sx = content / targetAspect
        }
        return floatArrayOf(sx, sy)
    }

    /** Ponto da tela de saída (0..1, y para cima) → coordenada de textura antes da matriz da SurfaceTexture. */
    private fun mapUv(x: Float, y: Float): FloatArray {
        val mx = if (mirror) 1f - x else x
        return when (rotation) {
            90 -> floatArrayOf(1f - y, mx)
            180 -> floatArrayOf(1f - mx, 1f - y)
            270 -> floatArrayOf(y, 1f - mx)
            else -> floatArrayOf(mx, y)
        }
    }

    private fun draw(x: Int, y: Int, w: Int, h: Int) {
        GLES20.glViewport(x, y, w, h)
        val (sx, sy) = contentScale(w.toFloat() / h).let { it[0] to it[1] }
        vertices.clear()
        for ((cx, cy) in CORNERS) {
            val uv = mapUv(cx, cy)
            vertices.put((cx * 2 - 1) * sx).put((cy * 2 - 1) * sy).put(uv[0]).put(uv[1])
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

    /**
     * Converte um toque na prévia em coordenadas normalizadas do sensor (x para a direita,
     * y para baixo), desfazendo enquadramento, espelho e rotação. Null se o toque caiu na borda.
     */
    fun previewToSensor(px: Float, py: Float): PointF? {
        if (prevW == 0 || prevH == 0) return null
        val box = letterbox(prevW, prevH)
        val ox = (px - box[0]) / box[2]
        val oy = 1f - (py - (prevH - box[1] - box[3])) / box[3]
        val s = contentScale(outW.toFloat() / outH)
        val cx = ((ox * 2 - 1) / s[0] + 1) / 2
        val cy = ((oy * 2 - 1) / s[1] + 1) / 2
        if (cx !in 0f..1f || cy !in 0f..1f) return null
        val uv = mapUv(cx, cy)
        return PointF(uv[0], 1f - uv[1])
    }

    fun release() {
        sync {
            encSurface?.let { egl.destroySurface(it) }
            prevSurface?.let { egl.destroySurface(it) }
            encSurface = null
            prevSurface = null
            surfaceTexture.release()
            egl.makeCurrent(idle)
            GLES20.glDeleteTextures(1, intArrayOf(texId), 0)
            GLES20.glDeleteProgram(program)
            egl.destroySurface(idle)
            egl.release()
        }
        thread.quitSafely()
    }

    private fun buildProgram(): Int {
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
        GLES20.glAttachShader(p, compile(GLES20.GL_VERTEX_SHADER, VERTEX))
        GLES20.glAttachShader(p, compile(GLES20.GL_FRAGMENT_SHADER, FRAGMENT))
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
            varying vec2 vUv;
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
                vUv = (uTex * vec4(aUv, 0.0, 1.0)).xy;
            }
        """

        const val FRAGMENT = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            uniform samplerExternalOES sTex;
            varying vec2 vUv;
            void main() {
                gl_FragColor = texture2D(sTex, vUv);
            }
        """
    }
}

package com.godoi.godoican

import android.os.Handler
import android.os.Looper

/** Estado compartilhado entre o serviço e a tela. Os observadores são chamados na thread principal. */
object Hub {
    data class Status(
        val running: Boolean = false,
        val pcName: String? = null,
        val pairingCode: String? = null,
        val pairingPc: String? = null,
        val addresses: String = "",
        val video: String = "",
        val stats: String = "",
        val message: String? = null,
    )

    private val main = Handler(Looper.getMainLooper())
    private val observers = mutableListOf<(Status) -> Unit>()

    @Volatile var status = Status()
        private set

    fun update(transform: (Status) -> Status) {
        synchronized(this) { status = transform(status) }
        main.post {
            val s = status
            observers.toList().forEach { it(s) }
        }
    }

    fun observe(observer: (Status) -> Unit) {
        observers += observer
        observer(status)
    }

    fun unobserve(observer: (Status) -> Unit) {
        observers -= observer
    }
}

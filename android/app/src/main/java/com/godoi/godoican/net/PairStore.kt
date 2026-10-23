package com.godoi.godoican.net

import android.content.Context

/** Guarda a identidade do celular e as chaves dos PCs pareados (armazenamento privado do app). */
class PairStore(context: Context) {
    private val prefs = context.getSharedPreferences("pairs", Context.MODE_PRIVATE)

    val phoneId: ByteArray by lazy {
        prefs.getString(SELF, null)?.let(::hexToBytes)
            ?: Crypto.random(16).also { prefs.edit().putString(SELF, it.hex()).apply() }
    }

    fun key(pcId: ByteArray): ByteArray? =
        prefs.getString(PC + pcId.hex(), null)?.substringBefore('|')?.let(::hexToBytes)

    fun put(pcId: ByteArray, key: ByteArray, pcName: String) {
        prefs.edit().putString(PC + pcId.hex(), key.hex() + "|" + pcName).apply()
    }

    fun names(): List<String> =
        prefs.all.filterKeys { it.startsWith(PC) }.values.map { (it as String).substringAfter('|') }

    fun clear() {
        val editor = prefs.edit()
        prefs.all.keys.filter { it.startsWith(PC) }.forEach { editor.remove(it) }
        editor.apply()
    }

    private companion object {
        const val SELF = "self"
        const val PC = "pc_"
    }
}

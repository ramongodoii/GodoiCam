package com.godoi.godoicam.net

/** Constantes do protocolo. A especificação completa está em docs/PROTOCOLO.md. */
object Proto {
    const val TCP_PORT = 47180
    const val UDP_PORT = 47181
    const val VERSION = 1
    val MAGIC = "GDCN".toByteArray(Charsets.US_ASCII)
    const val MAX_FRAME = 16 * 1024 * 1024

    const val DISCOVER_REQUEST = "GDCN?1"
    const val DISCOVER_REPLY = "GDCN!1"

    // Handshake, em texto claro
    const val HELLO = 0x01
    const val CHALLENGE = 0x02
    const val PAIR_CHALLENGE = 0x03
    const val AUTH = 0x04
    const val AUTH_OK = 0x05
    const val ERROR = 0x06

    // Sessão, cifradas
    const val CONFIG = 0x10
    const val VIDEO = 0x11
    const val PING = 0x12
    const val PONG = 0x13
    const val KEYFRAME_REQ = 0x14
    const val BYE = 0x15
    const val REPORT = 0x16
    const val STATS = 0x17
    const val FRAME_ACK = 0x18
    const val CONTROL = 0x19
    const val CAMERA_STATE = 0x1A

    // Chaves do CONTROL (PC → celular): u8 chave, u32 valor com sinal.
    const val CTL_SWITCH_CAMERA = 1
    const val CTL_TORCH = 2
    /** Zoom × 100. */
    const val CTL_ZOOM = 3
    /** Compensação de exposição em passos da câmera. */
    const val CTL_EV = 4
    /** 0 = automático. */
    const val CTL_ISO = 5
    /** Tempo de exposição em µs; 0 = automático. */
    const val CTL_EXPOSURE_US = 6
    /** Dioptrias × 100; negativo = automático. */
    const val CTL_FOCUS = 7
    /** 0 = automático. */
    const val CTL_KELVIN = 8
    const val CTL_ALL_AUTO = 9

    const val ERR_NOT_PAIRED = 1
    const val ERR_BAD_CODE = 2
    const val ERR_BUSY = 3
    const val ERR_VERSION = 4
    const val ERR_AUTH = 5
    const val ERR_TIMEOUT = 6

    const val HELLO_FORCE_PAIR = 1

    /** Capacidades do PC (u32 no HELLO). */
    const val CAP_FRAME_ACK = 1

    const val VIDEO_KEY = 1
    const val VIDEO_CONFIG = 2
    /** Bytes antes dos dados H.264 no payload VIDEO: flags (1) + pts (8). */
    const val VIDEO_HEADER = 9

    const val CODEC_H264 = 1
}

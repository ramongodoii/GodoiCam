// Lado do app: publica cada quadro NV12 na memória compartilhada lida pela câmera virtual.
#pragma once

#include <windows.h>

#include <mutex>

#include "GodoiCamShared.h"

class FrameWriter {
public:
    ~FrameWriter();
    // Chamado periodicamente: abre a memória quando a câmera virtual a cria e sinaliza que o app vive.
    void Heartbeat(bool streaming, LONG flags = 0);
    void Write(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int width, int height);
    // A câmera virtual está sendo lida por algum aplicativo agora?
    bool CameraInUse();

private:
    bool OpenLocked();
    std::mutex m_lock;
    HANDLE m_map = nullptr;
    HANDLE m_event = nullptr;
    GdcShared* m_shared = nullptr;
    uint8_t* m_frame = nullptr;
    ULONGLONG m_lastTry = 0;
};

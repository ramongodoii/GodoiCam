#pragma once

#include <windows.h>
#include <mfapi.h>

#include <mutex>
#include <string>
#include <vector>

#include "GodoiCamShared.h"

// Lado da câmera virtual: lê o quadro mais recente que o app publicou na memória
// compartilhada e o entrega no formato pedido pelo aplicativo (Zoom, Teams, OBS...).
// Sem app ou sem celular, entrega uma tela de espera.
class FrameSource {
public:
    FrameSource();
    ~FrameSource();

    // Evento sinalizado pelo app a cada quadro novo (pode ser nulo se a criação falhou).
    HANDLE FrameEvent() const { return m_event; }

    // Preenche um buffer travado (pitch em bytes) com o quadro atual no tamanho e
    // formato pedidos (NV12 ou RGB32).
    void Fill(BYTE* dst, LONG pitch, UINT32 width, UINT32 height, const GUID& subtype);

private:
    bool CopyLatest();
    const std::vector<BYTE>& Placeholder(UINT32 width, UINT32 height, const GUID& subtype, bool appRunning);

    HANDLE m_map = nullptr;
    HANDLE m_event = nullptr;
    GdcShared* m_shared = nullptr;
    const BYTE* m_frame = nullptr;

    std::vector<BYTE> m_latest;  // NV12 compacto
    UINT32 m_latestW = 0;
    UINT32 m_latestH = 0;
    LONG64 m_latestSeq = -1;

    std::vector<BYTE> m_scratch;  // NV12 intermediário para a saída RGB32

    std::vector<BYTE> m_placeholder;
    UINT32 m_phW = 0, m_phH = 0;
    GUID m_phSubtype = GUID_NULL;
    bool m_phApp = false;
};

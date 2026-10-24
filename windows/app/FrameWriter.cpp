#include "FrameWriter.h"

FrameWriter::~FrameWriter() {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_shared) {
        InterlockedExchange(&m_shared->state, GDC_STATE_IDLE);
        InterlockedExchange64(&m_shared->writerTick, 0);
        UnmapViewOfFile(m_shared);
    }
    if (m_map) CloseHandle(m_map);
    if (m_event) CloseHandle(m_event);
}

// A memória só existe depois que algum aplicativo abriu a câmera (a DLL a cria).
// Mantemos o handle aberto depois disso para ela sobreviver entre usos.
bool FrameWriter::OpenLocked() {
    if (m_shared) return true;
    ULONGLONG now = GetTickCount64();
    if (now - m_lastTry < 500) return false;
    m_lastTry = now;
    m_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, GDC_SHM_NAME);
    if (!m_map) return false;
    auto* view = (uint8_t*)MapViewOfFile(m_map, FILE_MAP_ALL_ACCESS, 0, 0, GDC_SHM_SIZE);
    if (!view) {
        CloseHandle(m_map);
        m_map = nullptr;
        return false;
    }
    m_shared = (GdcShared*)view;
    m_frame = view + GDC_HEADER_SIZE;
    m_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, GDC_EVENT_NAME);
    return true;
}

void FrameWriter::Heartbeat(bool streaming, LONG flags) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (!OpenLocked()) return;
    InterlockedExchange64(&m_shared->writerTick, (LONG64)GetTickCount64());
    InterlockedExchange(&m_shared->state, streaming ? GDC_STATE_STREAMING : GDC_STATE_IDLE);
    InterlockedExchange(&m_shared->flags, flags);
    if (!streaming && m_event) SetEvent(m_event);  // acorda a câmera para trocar para a tela de espera
}

void FrameWriter::Write(const uint8_t* y, int yPitch, const uint8_t* uv, int uvPitch, int width, int height) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (!OpenLocked()) return;
    if (width <= 0 || height <= 0 || width > (int)GDC_MAX_W || height > (int)GDC_MAX_H) return;
    width &= ~1;
    height &= ~1;

    InterlockedIncrement64(&m_shared->seq);  // ímpar: escrevendo
    MemoryBarrier();
    uint8_t* dy = m_frame;
    for (int r = 0; r < height; r++) memcpy(dy + (size_t)r * width, y + (size_t)r * yPitch, width);
    uint8_t* duv = m_frame + (size_t)width * height;
    for (int r = 0; r < height / 2; r++) memcpy(duv + (size_t)r * width, uv + (size_t)r * uvPitch, width);
    m_shared->width = width;
    m_shared->height = height;
    MemoryBarrier();
    InterlockedIncrement64(&m_shared->seq);  // par: pronto
    InterlockedExchange64(&m_shared->writerTick, (LONG64)GetTickCount64());
    InterlockedExchange(&m_shared->state, GDC_STATE_STREAMING);
    if (m_event) SetEvent(m_event);
}

bool FrameWriter::CameraInUse() {
    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_shared) return false;
    return GetTickCount64() - (ULONGLONG)m_shared->readerTick < GDC_READER_TIMEOUT_MS;
}

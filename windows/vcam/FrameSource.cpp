#include "FrameSource.h"

#include <sddl.h>

#include "ImageUtil.h"

namespace {

// O app roda como o usuário logado; a DLL roda no Frame Server (LOCAL SERVICE).
// Os dois precisam abrir os mesmos objetos.
SECURITY_ATTRIBUTES* OpenToEveryone(SECURITY_ATTRIBUTES& sa) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;SY)(A;;GA;;;LS)", SDDL_REVISION_1,
                                                              &sd, nullptr))
        return nullptr;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return &sa;
}

}  // namespace

FrameSource::FrameSource() {
    SECURITY_ATTRIBUTES sa{};
    SECURITY_ATTRIBUTES* psa = OpenToEveryone(sa);
    m_map = CreateFileMappingW(INVALID_HANDLE_VALUE, psa, PAGE_READWRITE, 0, GDC_SHM_SIZE, GDC_SHM_NAME);
    if (!m_map) m_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, GDC_SHM_NAME);
    m_event = CreateEventW(psa, FALSE, FALSE, GDC_EVENT_NAME);
    if (!m_event) m_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, GDC_EVENT_NAME);
    if (psa) LocalFree(sa.lpSecurityDescriptor);

    if (m_map) {
        BYTE* view = (BYTE*)MapViewOfFile(m_map, FILE_MAP_ALL_ACCESS, 0, 0, GDC_SHM_SIZE);
        if (view) {
            m_shared = (GdcShared*)view;
            m_frame = view + GDC_HEADER_SIZE;
            if (m_shared->magic != GDC_MAGIC) {
                m_shared->headerSize = GDC_HEADER_SIZE;
                m_shared->maxFrameBytes = GDC_MAX_FRAME;
                m_shared->magic = GDC_MAGIC;
            }
        }
    }
}

FrameSource::~FrameSource() {
    if (m_shared) UnmapViewOfFile(m_shared);
    if (m_map) CloseHandle(m_map);
    if (m_event) CloseHandle(m_event);
}

// Copia o quadro publicado se ele mudou. Usa o seqlock para nunca pegar um quadro pela metade.
bool FrameSource::CopyLatest() {
    for (int attempt = 0; attempt < 3; attempt++) {
        LONG64 seq = m_shared->seq;
        if (seq & 1) {
            Sleep(1);
            continue;
        }
        if (seq == m_latestSeq) return !m_latest.empty();
        MemoryBarrier();
        UINT32 w = (UINT32)m_shared->width;
        UINT32 h = (UINT32)m_shared->height;
        if (w == 0 || h == 0 || w > GDC_MAX_W || h > GDC_MAX_H || (w & 1) || (h & 1)) return false;
        size_t size = (size_t)w * h * 3 / 2;
        m_latest.resize(size);
        memcpy(m_latest.data(), m_frame, size);
        MemoryBarrier();
        if (m_shared->seq == seq) {
            m_latestW = w;
            m_latestH = h;
            m_latestSeq = seq;
            return true;
        }
    }
    return !m_latest.empty();
}

void FrameSource::Fill(BYTE* dst, LONG pitch, UINT32 width, UINT32 height, const GUID& subtype) {
    bool appRunning = false;
    bool streaming = false;
    if (m_shared) {
        ULONGLONG now = GetTickCount64();
        InterlockedExchange64(&m_shared->readerTick, (LONG64)now);
        appRunning = now - (ULONGLONG)m_shared->writerTick < GDC_WRITER_TIMEOUT_MS;
        streaming = appRunning && m_shared->state == GDC_STATE_STREAMING && CopyLatest();
    }

    bool nv12 = subtype == MFVideoFormat_NV12;
    if (!streaming && m_shared && appRunning && (m_shared->flags & GDC_FLAG_BLANK_WHEN_IDLE)) {
        // Preto: Y=16 e UV=128 em NV12; zeros em RGB32.
        UINT32 rows = nv12 ? height * 3 / 2 : height;
        UINT32 rowBytes = nv12 ? width : width * 4;
        for (UINT32 y = 0; y < rows; y++)
            memset(dst + (size_t)y * pitch, nv12 ? (y < height ? 16 : 128) : 0, rowBytes);
        return;
    }
    if (!streaming) {
        const std::vector<BYTE>& ph = Placeholder(width, height, subtype, appRunning);
        UINT32 rowBytes = nv12 ? width : width * 4;
        UINT32 rows = nv12 ? height * 3 / 2 : height;
        for (UINT32 y = 0; y < rows; y++) memcpy(dst + (size_t)y * pitch, ph.data() + (size_t)y * rowBytes, rowBytes);
        return;
    }

    const BYTE* sy = m_latest.data();
    const BYTE* suv = sy + (size_t)m_latestW * m_latestH;
    if (nv12) {
        img::ScaleNV12(sy, m_latestW, suv, m_latestW, m_latestW, m_latestH, dst, pitch, dst + (size_t)pitch * height,
                       pitch, width, height);
        return;
    }
    // RGB32: redimensiona em NV12 primeiro (se preciso) e converte.
    if (m_latestW != width || m_latestH != height) {
        m_scratch.resize((size_t)width * height * 3 / 2);
        img::ScaleNV12(sy, m_latestW, suv, m_latestW, m_latestW, m_latestH, m_scratch.data(), width,
                       m_scratch.data() + (size_t)width * height, width, width, height);
        sy = m_scratch.data();
        suv = sy + (size_t)width * height;
        img::NV12ToBGRA(sy, width, suv, width, width, height, dst, pitch);
    } else {
        img::NV12ToBGRA(sy, m_latestW, suv, m_latestW, width, height, dst, pitch);
    }
}

// Tela de espera desenhada com GDI e guardada até o tamanho/formato/estado mudar.
const std::vector<BYTE>& FrameSource::Placeholder(UINT32 width, UINT32 height, const GUID& subtype, bool appRunning) {
    if (!m_placeholder.empty() && m_phW == width && m_phH == height && m_phSubtype == subtype && m_phApp == appRunning)
        return m_placeholder;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = (LONG)width;
    bmi.bmiHeader.biHeight = -(LONG)height;  // de cima para baixo
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    std::vector<BYTE> bgra((size_t)width * height * 4);

    if (dc && bmp && bits) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        RECT all{0, 0, (LONG)width, (LONG)height};
        HBRUSH bg = CreateSolidBrush(RGB(22, 24, 29));
        FillRect(dc, &all, bg);
        DeleteObject(bg);

        // Lente: anel laranja e miolo claro.
        int cx = (int)width / 2, cy = (int)height * 2 / 5, r = (int)height / 9;
        HBRUSH ring = CreateSolidBrush(RGB(255, 122, 69));
        HBRUSH inner = CreateSolidBrush(RGB(22, 24, 29));
        HBRUSH core = CreateSolidBrush(RGB(244, 245, 247));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        HGDIOBJ oldBrush = SelectObject(dc, ring);
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, inner);
        int r2 = r * 73 / 100;
        Ellipse(dc, cx - r2, cy - r2, cx + r2, cy + r2);
        SelectObject(dc, core);
        int r3 = r * 42 / 100;
        Ellipse(dc, cx - r3, cy - r3, cx + r3, cy + r3);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(ring);
        DeleteObject(inner);
        DeleteObject(core);

        SetBkMode(dc, TRANSPARENT);
        HFONT title = CreateFontW(-(int)height / 11, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                  ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HFONT body = CreateFontW(-(int)height / 26, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, title);
        SetTextColor(dc, RGB(244, 245, 247));
        RECT t{0, cy + r + (LONG)height / 30, (LONG)width, (LONG)height};
        DrawTextW(dc, L"GodoiCam", -1, &t, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(dc, body);
        SetTextColor(dc, RGB(169, 174, 184));
        RECT m{0, t.top + (LONG)height / 8, (LONG)width, (LONG)height};
        DrawTextW(dc,
                  appRunning ? L"Aguardando o celular… Abra o GodoiCam no Android."
                             : L"Abra o GodoiCam no PC para usar o celular como webcam.",
                  -1, &m, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        DeleteObject(title);
        DeleteObject(body);

        GdiFlush();
        memcpy(bgra.data(), bits, bgra.size());
        for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
        SelectObject(dc, oldBmp);
    }
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);

    if (subtype == MFVideoFormat_NV12) {
        m_placeholder.resize((size_t)width * height * 3 / 2);
        img::BGRAToNV12(bgra.data(), (int)width * 4, (int)width, (int)height, m_placeholder.data(), (int)width,
                        m_placeholder.data() + (size_t)width * height, (int)width);
    } else {
        m_placeholder = std::move(bgra);
    }
    m_phW = width;
    m_phH = height;
    m_phSubtype = subtype;
    m_phApp = appRunning;
    return m_placeholder;
}

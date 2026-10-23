// Contrato entre o GodoiCan.exe (escreve quadros) e a GodoiCanVCam.dll (câmera virtual,
// carregada pelo serviço Frame Server do Windows, em outra sessão e outra conta).
#pragma once

#include <windows.h>
#include <stdint.h>

// {6F1C2B7E-4D0A-4E8B-9C33-6A1F0D2E5B71}
static const GUID CLSID_GodoiCanVCam = {0x6f1c2b7e, 0x4d0a, 0x4e8b, {0x9c, 0x33, 0x6a, 0x1f, 0x0d, 0x2e, 0x5b, 0x71}};
#define GDC_VCAM_CLSID_STR L"{6F1C2B7E-4D0A-4E8B-9C33-6A1F0D2E5B71}"
#define GDC_VCAM_NAME L"GodoiCan"

// Objetos globais: a DLL (no Frame Server) cria; o app do usuário abre.
#define GDC_SHM_NAME L"Global\\GodoiCanFramesV1"
#define GDC_EVENT_NAME L"Global\\GodoiCanFrameReadyV1"

constexpr uint32_t GDC_MAGIC = 0x31434447;  // "GDC1"
constexpr uint32_t GDC_MAX_W = 3840;
constexpr uint32_t GDC_MAX_H = 2160;
constexpr uint32_t GDC_MAX_FRAME = GDC_MAX_W * GDC_MAX_H * 3 / 2;
constexpr uint32_t GDC_HEADER_SIZE = 4096;
constexpr uint32_t GDC_SHM_SIZE = GDC_HEADER_SIZE + GDC_MAX_FRAME;

// Sem batimento do app por esse tempo, a câmera mostra "abra o GodoiCan".
constexpr uint64_t GDC_WRITER_TIMEOUT_MS = 1500;
// Sem leitura da câmera por esse tempo, o app considera a câmera fechada.
constexpr uint64_t GDC_READER_TIMEOUT_MS = 1500;

enum GdcState : LONG {
    GDC_STATE_IDLE = 0,       // app aberto, sem celular
    GDC_STATE_STREAMING = 1,  // quadros chegando
};

// Cabeçalho no início da memória compartilhada. O quadro NV12 (stride = largura)
// fica em GDC_HEADER_SIZE. `seq` funciona como seqlock: ímpar enquanto o app escreve.
struct GdcShared {
    uint32_t magic;
    uint32_t headerSize;
    uint32_t maxFrameBytes;
    uint32_t reserved0;
    volatile LONG64 seq;
    volatile LONG64 writerTick;  // GetTickCount64 do último batimento do app
    volatile LONG64 readerTick;  // GetTickCount64 da última leitura da câmera
    volatile LONG state;         // GdcState
    volatile LONG width;
    volatile LONG height;
    volatile LONG reserved1;
};

// Uma conexão com o celular: handshake/pareamento, recepção, decodificação e
// publicação na câmera virtual. Reconecta sozinha até o usuário desconectar.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Decoder.h"
#include "FrameWriter.h"
#include "Net.h"
#include "PairStore.h"

struct Target {
    bool usb = false;
    std::string host;
    uint16_t port = proto::TCP_PORT;
    std::string phoneId;  // pode ser vazio (USB ou IP manual)
    std::string name;
    // Persistente: reconecta para sempre (USB/IP escolhidos pelo usuário). Senão desiste
    // logo e deixa a conexão automática escolher de novo quando o celular reaparecer.
    bool persistent = false;
    bool automatic = false;
};

class Session {
public:
    enum class State { Idle, Connecting, NeedCode, Connected, Streaming, Failed };

    struct Status {
        State state = State::Idle;
        std::wstring title;
        std::wstring detail;
        std::wstring phoneName;
        std::wstring brief;  // ex.: "1920×1080 • 30 fps • 98 ms"
        std::wstring stats;  // linha completa de estatísticas
    };

    // Estado da câmera do celular (CAMERA_STATE), para o menu de controle remoto.
    struct PhoneCamera {
        bool valid = false;
        std::wstring label;
        int cameraCount = 1;
        bool torchAvailable = false, torchOn = false;
        // O que a câmera atual aceita (não o que está ligado).
        bool manualExposure = false, manualFocus = false, kelvinAvailable = false, focusCalibrated = false;
        int zoom = 100, zoomMin = 100, zoomMax = 100;  // × 100
        int ev = 0, evMin = 0, evMax = 0, evPerStop = 1;  // passos da câmera
        int iso = 0, isoMin = 0, isoMax = 0;               // 0 = automático
        int exposureUs = 0, exposureMinUs = 0, exposureMaxUs = 0;
        int focus = -1, focusMax = 0;  // dioptrias × 100; -1 = automático
        int kelvin = 0;
    };

    Session(PairStore& pairs, FrameWriter& writer, std::function<void()> onStatus, std::function<void()> onPreview);
    ~Session();

    void Connect(const Target& target);
    void Disconnect();
    void SubmitCode(const std::string& code);

    Status GetStatus();
    bool Active() const { return m_running && !m_stopping; }
    bool Streaming() const { return m_streaming; }
    // Recebeu vídeo nos últimos `ms` (para segurar a imagem durante quedas curtas).
    bool RecentlyStreaming(ULONGLONG ms) const { return GetTickCount64() - m_lastFrameTick < ms; }
    // A última tentativa chegou a autenticar com o celular?
    bool LastRunConnected() const { return m_connectedOnce; }
    // Tamanho máximo da prévia (área de vídeo da janela); 0 desliga a prévia.
    void SetPreviewSize(int width, int height) {
        m_previewMaxW = width;
        m_previewMaxH = height;
    }
    Target CurrentTarget();

    PhoneCamera GetPhoneCamera();
    // Muda um ajuste da câmera do celular (proto::CTL_*). Ignorado sem sessão autenticada.
    void SendControl(uint8_t key, int32_t value);

    // Copia a prévia BGRA mais recente; falso se não há nada novo.
    bool TakePreview(std::vector<uint8_t>& bgra, int& width, int& height);

private:
    enum class Outcome { Retry, RetryNow, Stop };

    void Run(Target target);
    Outcome RunOnce(const Target& target, bool& forcePair);
    bool Handshake(Conn& conn, bool& forcePair, Outcome& outcome, std::string& phoneId, std::string& phoneName);
    void Receive(Conn& conn, const std::string& phoneName);
    void SetStatus(State state, const std::wstring& title, const std::wstring& detail = L"");
    void SetStats(const std::wstring& brief, const std::wstring& stats);
    bool WaitStop(DWORD ms);
    static int64_t NowUs();

    PairStore& m_pairs;
    FrameWriter& m_writer;
    std::function<void()> m_onStatus;
    std::function<void()> m_onPreview;

    std::thread m_thread;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_streaming{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connectedOnce{false};
    std::atomic<ULONGLONG> m_lastFrameTick{0};
    std::atomic<int> m_previewMaxW{0};
    std::atomic<int> m_previewMaxH{0};
    HANDLE m_wake = nullptr;  // desconectar
    HANDLE m_codeReady = nullptr;
    std::mutex m_lock;
    Status m_status;
    Target m_target;
    std::string m_code;
    Conn* m_conn = nullptr;
    PhoneCamera m_camera;  // protegido por m_lock; valid só durante a recepção autenticada

    // Relógio do celular − relógio do PC, estimado pelo PING/PONG de menor RTT recente.
    std::atomic<int64_t> m_clockOffset{0};
    std::atomic<int64_t> m_bestRtt{INT64_MAX};
    std::atomic<int64_t> m_rtt{0};

    std::mutex m_previewLock;
    std::vector<uint8_t> m_preview;
    int m_previewW = 0, m_previewH = 0;
    bool m_previewNew = false;
    std::atomic<bool> m_previewPosted{false};
};

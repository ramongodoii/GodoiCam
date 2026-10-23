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
};

class Session {
public:
    enum class State { Idle, Connecting, NeedCode, Connected, Streaming, Failed };

    struct Status {
        State state = State::Idle;
        std::wstring title;
        std::wstring detail;
        std::wstring phoneName;
    };

    Session(PairStore& pairs, FrameWriter& writer, std::function<void()> onStatus, std::function<void()> onPreview);
    ~Session();

    void Connect(const Target& target);
    void Disconnect();
    void SubmitCode(const std::string& code);

    Status GetStatus();
    bool Active() const { return m_thread.joinable() && !m_stopping; }
    bool Streaming() const { return m_streaming; }
    Target CurrentTarget();

    // Copia a prévia BGRA mais recente; falso se não há nada novo.
    bool TakePreview(std::vector<uint8_t>& bgra, int& width, int& height);

private:
    enum class Outcome { Retry, RetryNow, Stop };

    void Run(Target target);
    Outcome RunOnce(const Target& target, bool& forcePair);
    bool Handshake(Conn& conn, bool& forcePair, Outcome& outcome, std::string& phoneId, std::string& phoneName);
    void Receive(Conn& conn, const std::string& phoneName);
    void SetStatus(State state, const std::wstring& title, const std::wstring& detail = L"");
    bool WaitStop(DWORD ms);
    static int64_t NowUs();

    PairStore& m_pairs;
    FrameWriter& m_writer;
    std::function<void()> m_onStatus;
    std::function<void()> m_onPreview;

    std::thread m_thread;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_streaming{false};
    HANDLE m_wake = nullptr;  // desconectar
    HANDLE m_codeReady = nullptr;
    std::mutex m_lock;
    Status m_status;
    Target m_target;
    std::string m_code;
    Conn* m_conn = nullptr;

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

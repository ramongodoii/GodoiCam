#include "Session.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <share.h>
#include <stdio.h>

#include "H264Sps.h"
#include "ImageUtil.h"
#include "System.h"

namespace {

std::wstring ErrorText(const Bytes& payload) {
    if (payload.size() <= 1) return L"O celular recusou a conexão.";
    return Widen(std::string(payload.begin() + 1, payload.end()));
}

// Quadro que esperou mais que isso para ser decodificado: o PC ficou para trás e pula até o
// próximo quadro-chave, para voltar ao tempo real em vez de exibir o atraso.
constexpr int64_t kMaxDecodeLagUs = 150000;
constexpr size_t kMaxDecodeQueue = 6;

// Recepção → decodificação. A thread do socket nunca espera o decodificador.
struct PacketQueue {
    struct Item {
        uint8_t type = 0;
        Bytes data;
        int64_t arrivedUs = 0;
    };

    void Push(uint8_t type, Bytes&& data, int64_t now) {
        {
            std::lock_guard<std::mutex> l(lock);
            items.push_back(Item{type, std::move(data), now});
        }
        cv.notify_one();
    }

    bool Pop(Item& out) {
        std::unique_lock<std::mutex> l(lock);
        cv.wait(l, [&] { return closed || !items.empty(); });
        if (closed) return false;
        out = std::move(items.front());
        items.pop_front();
        return true;
    }

    size_t Size() {
        std::lock_guard<std::mutex> l(lock);
        return items.size();
    }

    void Close() {
        {
            std::lock_guard<std::mutex> l(lock);
            closed = true;
        }
        cv.notify_all();
    }

    std::mutex lock;
    std::condition_variable cv;
    std::deque<Item> items;
    bool closed = false;
};

// Decodificação → prévia da janela. Um quadro por vez; enquanto a conversão roda, os quadros
// novos seguem só para a câmera virtual.
struct PreviewJob {
    std::mutex lock;
    std::condition_variable cv;
    Bytes nv12;  // stride = largura
    int width = 0, height = 0;
    bool pending = false;
    bool closed = false;
};

}  // namespace

Session::Session(PairStore& pairs, FrameWriter& writer, std::function<void()> onStatus,
                 std::function<void()> onPreview)
    : m_pairs(pairs), m_writer(writer), m_onStatus(std::move(onStatus)), m_onPreview(std::move(onPreview)) {
    m_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_codeReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Session::~Session() {
    Disconnect();
    CloseHandle(m_wake);
    CloseHandle(m_codeReady);
}

int64_t Session::NowUs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart / freq.QuadPart * 1000000 + c.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart);
}

void Session::SetStatus(State state, const std::wstring& title, const std::wstring& detail) {
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_status.state = state;
        m_status.title = title;
        m_status.detail = detail;
    }
    if (m_onStatus) m_onStatus();
}

void Session::SetStats(const std::wstring& brief, const std::wstring& stats) {
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_status.brief = brief;
        m_status.stats = stats;
    }
    if (m_onStatus) m_onStatus();
}

Session::Status Session::GetStatus() {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_status;
}

Target Session::CurrentTarget() {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_target;
}

Session::PhoneCamera Session::GetPhoneCamera() {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_camera;
}

void Session::SendControl(uint8_t key, int32_t value) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_conn || !m_camera.valid) return;
    m_conn->Send(proto::CONTROL, Writer().U8(key).U32((uint32_t)value).b);
    Log("controle remoto: chave %d valor %d", key, value);
}

bool Session::WaitStop(DWORD ms) { return WaitForSingleObject(m_wake, ms) == WAIT_OBJECT_0; }

void Session::Connect(const Target& target) {
    Disconnect();
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_target = target;
        m_status = Status{};
        m_status.phoneName = Widen(target.name);
    }
    m_stopping = false;
    m_connectedOnce = false;
    m_running = true;
    ResetEvent(m_wake);
    m_thread = std::thread([this, target] { Run(target); });
}

void Session::Disconnect() {
    if (!m_thread.joinable()) return;
    m_stopping = true;
    SetEvent(m_wake);
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_conn) {
            m_conn->Send(proto::BYE, {});
            m_conn->Close();
        }
    }
    m_thread.join();
    m_streaming = false;
    m_running = false;
    SetStatus(State::Idle, L"Desconectado");
}

void Session::SubmitCode(const std::string& code) {
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_code = code;
    }
    SetEvent(m_codeReady);
}

bool Session::TakePreview(std::vector<uint8_t>& bgra, int& width, int& height) {
    std::lock_guard<std::mutex> lock(m_previewLock);
    m_previewPosted = false;
    if (!m_previewNew) return false;
    // Troca em vez de copiar: a janela fica com o quadro novo e devolve o buffer antigo.
    bgra.swap(m_preview);
    width = m_previewW;
    height = m_previewH;
    m_previewNew = false;
    return true;
}

// ------------------------------------------------------------------------------------ laço principal

void Session::Run(Target target) {
    bool forcePair = false;
    int failures = 0;
    while (!m_stopping) {
        bool wasConnected = m_connectedOnce;
        Outcome o = RunOnce(target, forcePair);
        m_streaming = false;
        if (m_stopping || o == Outcome::Stop) break;
        if (o == Outcome::RetryNow) continue;
        if (m_connectedOnce && !wasConnected) failures = 0;  // a sessão chegou a funcionar
        failures++;
        // Alvos não persistentes desistem cedo: a conexão automática escolhe de novo (outra
        // rota, outro celular) assim que o aparelho reaparecer.
        if (!target.persistent && failures >= (m_connectedOnce ? 4 : 1)) break;
        // Cabo tirado: não adianta insistir no USB; a conexão automática volta para o Wi-Fi.
        if (target.usb && !target.persistent && !sys::AdbDevicePresent()) {
            Log("cabo USB desconectado: voltando para a conexão automática");
            break;
        }
        if (WaitStop(1000u * std::min(failures, 5))) break;
    }
    m_running = false;
    if (m_onStatus) m_onStatus();
}

Session::Outcome Session::RunOnce(const Target& target, bool& forcePair) {
    std::wstring name = Widen(target.name.empty() ? (target.usb ? "celular (USB)" : target.host) : target.name);
    SetStatus(State::Connecting, L"Conectando a " + name + L"…");

    std::wstring error;
    std::string host = target.host;
    uint16_t port = target.port;
    if (target.usb) {
        if (!sys::AdbForward(error)) {
            SetStatus(State::Failed, L"USB indisponível", error);
            return Outcome::Retry;
        }
        host = "127.0.0.1";
        port = proto::TCP_PORT;
    }

    Conn conn;
    if (!conn.Connect(host, port, 4000, error)) {
        SetStatus(State::Failed, L"Sem conexão com " + name, error);
        return Outcome::Retry;
    }
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_conn = &conn;
    }
    struct Clear {
        Session* s;
        ~Clear() {
            std::lock_guard<std::mutex> lock(s->m_lock);
            s->m_conn = nullptr;
        }
    } clear{this};
    if (m_stopping) return Outcome::Stop;

    Outcome outcome = Outcome::Retry;
    std::string phoneId, phoneName;
    if (!Handshake(conn, forcePair, outcome, phoneId, phoneName)) return outcome;
    forcePair = false;
    m_connectedOnce = true;
    m_pairs.SetLastPhone(phoneId);
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_target.phoneId = phoneId;
        m_target.name = phoneName;
        m_status.phoneName = Widen(phoneName);
    }
    Receive(conn, phoneName);
    return Outcome::Retry;
}

bool Session::Handshake(Conn& conn, bool& forcePair, Outcome& outcome, std::string& phoneId,
                        std::string& phoneName) {
    conn.SetRecvTimeout(15000);
    crypto::Ecdh ecdh;
    Bytes pcId = m_pairs.PcId();
    Bytes pcNonce = crypto::Random(16);
    Bytes pcPub = ecdh.PublicBytes();
    if (pcPub.size() != 65) {
        SetStatus(State::Failed, L"Erro de criptografia no PC");
        outcome = Outcome::Stop;
        return false;
    }

    Writer hello;
    hello.Raw({'G', 'D', 'C', 'N'})
        .U16(proto::VERSION)
        .Raw(pcId)
        .U8(forcePair ? proto::HELLO_FORCE_PAIR : 0)
        .Raw(pcNonce)
        .Raw(pcPub)
        .Str(sys::ComputerName())
        .U32(proto::CAP_FRAME_ACK);
    uint8_t type = 0;
    Bytes payload;
    if (!conn.Send(proto::HELLO, hello.b) || !conn.Recv(type, payload)) {
        SetStatus(State::Failed, L"O celular fechou a conexão");
        return false;
    }

    if (type == proto::ERROR_MSG) {
        uint8_t code = payload.empty() ? 0 : payload[0];
        SetStatus(State::Failed, L"O celular recusou", ErrorText(payload));
        if (code == proto::ERR_VERSION) outcome = Outcome::Stop;
        return false;
    }

    Bytes key;
    if (type == proto::CHALLENGE) {
        Reader r(payload);
        Bytes rawId = r.Raw(16), phoneNonce = r.Raw(16);
        phoneName = r.Str();
        if (!r.ok) return false;
        phoneId = crypto::Hex(rawId);
        if (!m_pairs.Key(phoneId, key)) {
            // O celular nos conhece, mas perdemos a chave: pede um pareamento novo.
            conn.Send(proto::ERROR_MSG, Bytes{proto::ERR_NOT_PAIRED});
            forcePair = true;
            outcome = Outcome::RetryNow;
            return false;
        }
        Bytes ids = crypto::Concat({&pcId, &rawId, &pcNonce, &phoneNonce});
        Bytes lpc = crypto::Label("GDCN-auth-pc"), lphone = crypto::Label("GDCN-auth-phone");
        if (!conn.Send(proto::AUTH, crypto::Hmac(key, crypto::Concat({&lpc, &ids}))) || !conn.Recv(type, payload))
            return false;
        if (type != proto::AUTH_OK || !crypto::Equal(payload, crypto::Hmac(key, crypto::Concat({&lphone, &ids})))) {
            m_pairs.Remove(phoneId);
            SetStatus(State::Failed, L"O celular não reconheceu este PC", L"Vamos parear de novo.");
            forcePair = true;
            outcome = Outcome::RetryNow;
            return false;
        }
        Bytes l1 = crypto::Label("GDCN-key-p2c"), l2 = crypto::Label("GDCN-key-c2p");
        conn.EnableCrypto(crypto::Hmac(key, crypto::Concat({&l2, &pcNonce, &phoneNonce})),
                          crypto::Hmac(key, crypto::Concat({&l1, &pcNonce, &phoneNonce})));
        return true;
    }

    if (type == proto::PAIR_CHALLENGE) {
        Reader r(payload);
        Bytes rawId = r.Raw(16), phoneNonce = r.Raw(16), phonePub = r.Raw(65);
        phoneName = r.Str();
        if (!r.ok) return false;
        phoneId = crypto::Hex(rawId);
        m_pairs.Remove(phoneId);
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_status.phoneName = Widen(phoneName);
            m_code.clear();
        }
        ResetEvent(m_codeReady);
        SetStatus(State::NeedCode, L"Digite o código mostrado no celular",
                  L"O celular " + Widen(phoneName) + L" está exibindo um código de 6 dígitos.");

        HANDLE waits[2] = {m_wake, m_codeReady};
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 115000);
        if (w != WAIT_OBJECT_0 + 1) {
            if (w == WAIT_TIMEOUT) SetStatus(State::Failed, L"Tempo esgotado para digitar o código");
            outcome = Outcome::Stop;
            return false;
        }
        std::string code;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            code = m_code;
        }
        Bytes z;
        if (!ecdh.Agree(phonePub, z)) return false;
        Bytes lpair = crypto::Label("GDCN-pair"), codeBytes(code.begin(), code.end());
        Bytes p = crypto::Sha256(crypto::Concat({&lpair, &z, &codeBytes}));
        Bytes t = crypto::Concat({&pcPub, &phonePub, &pcNonce, &phoneNonce, &pcId, &rawId});
        Bytes lpc = crypto::Label("GDCN-pair-pc"), lphone = crypto::Label("GDCN-pair-phone"),
              lkey = crypto::Label("GDCN-pair-key");
        SetStatus(State::Connecting, L"Verificando o código…");
        if (!conn.Send(proto::AUTH, crypto::Hmac(p, crypto::Concat({&lpc, &t}))) || !conn.Recv(type, payload))
            return false;
        if (type == proto::ERROR_MSG) {
            SetStatus(State::Failed, L"Código incorreto", L"O celular vai mostrar um código novo.");
            forcePair = true;
            return false;  // Retry: espera o celular liberar um novo pareamento
        }
        if (type != proto::AUTH_OK || !crypto::Equal(payload, crypto::Hmac(p, crypto::Concat({&lphone, &t})))) {
            SetStatus(State::Failed, L"Falha na verificação do celular");
            return false;
        }
        key = crypto::Hmac(p, crypto::Concat({&lkey, &t}));
        m_pairs.Put(phoneId, key, phoneName);
        Bytes l1 = crypto::Label("GDCN-key-p2c"), l2 = crypto::Label("GDCN-key-c2p");
        conn.EnableCrypto(crypto::Hmac(key, crypto::Concat({&l2, &pcNonce, &phoneNonce})),
                          crypto::Hmac(key, crypto::Concat({&l1, &pcNonce, &phoneNonce})));
        return true;
    }

    SetStatus(State::Failed, L"Resposta inesperada do celular");
    return false;
}

// ------------------------------------------------------------------------------------ recepção

// Três threads: esta lê o socket (e confirma cada quadro ao celular na hora), uma decodifica
// e publica na câmera virtual, e outra converte a prévia da janela no ritmo que conseguir.
// Assim o socket é sempre esvaziado e nenhum atraso fica escondido no buffer do sistema.
void Session::Receive(Conn& conn, const std::string& phoneName) {
    std::wstring wname = Widen(phoneName);
    SetStatus(State::Connected, L"Conectado a " + wname, L"Aguardando vídeo…");
    conn.SetRecvTimeout(5000);
    m_bestRtt = INT64_MAX;

    std::atomic<bool> done{false};
    std::thread pinger([&] {
        int n = 0;
        while (!done && !m_stopping) {
            if (!conn.Send(proto::PING, Writer().I64(NowUs()).b)) break;
            // A cada 10 s esquece o melhor RTT, para acompanhar a deriva dos relógios.
            if (++n % 10 == 0) m_bestRtt = INT64_MAX;
            if (WaitStop(1000)) break;
        }
    });

    int cfgW = 0, cfgH = 0, cfgFps = 0;
    uint32_t cfgBitrate = 0;
    std::string cfgDesc;
    int64_t windowStart = NowUs();
    uint64_t bytes = 0;
    // Preenchidos pela thread de decodificação; lidos e zerados uma vez por segundo.
    std::atomic<int> frames{0}, latencyCount{0}, decodeCount{0}, pcDropped{0};
    std::atomic<int64_t> latencySum{0}, decodeSum{0};
    std::atomic<int64_t> lastKeyReq{0};
    std::atomic<bool> decoderFailed{false};
    std::atomic<bool> hwDecode{false};
    struct PhoneStats {
        bool valid = false;
        int camFps = 0, encFps = 0, dropped = 0, queued = 0, encodeMs = -1, tempTenths = 0, thermal = 0;
        int wifiMhz = 0, rssi = 0, linkMbps = 0, screenOn = -1, fpsTarget = 0, netMs = -1;
        uint32_t bits = 0, target = 0;
    } phone;

    // Uma linha por segundo em %APPDATA%\GodoiCam\stats.csv (Fase 0: medições).
    std::wstring csvPath = AppDataFile(L"stats.csv");
    // Arquivo com cabeçalho antigo (sem as colunas novas): guarda à parte e começa outro.
    if (FILE* old = _wfopen(csvPath.c_str(), L"rb")) {
        char head[640] = {};
        fgets(head, sizeof(head), old);
        fclose(old);
        if (!strstr(head, "pc_gpu")) {
            std::wstring backup = AppDataFile(L"stats-antigo.csv");
            DeleteFileW(backup.c_str());
            MoveFileW(csvPath.c_str(), backup.c_str());
        }
    }
    bool csvNew = GetFileAttributesW(csvPath.c_str()) == INVALID_FILE_ATTRIBUTES;
    FILE* csv = _wfsopen(csvPath.c_str(), L"ab", _SH_DENYWR);
    if (csv && csvNew)
        fputs("hora;celular;resolucao;cel_cam_fps;cel_env_fps;cel_mbps;cel_alvo_mbps;cel_descartados;cel_fila;"
              "cel_codificacao_ms;cel_temp_c;cel_termico;pc_fps;pc_mbps;pc_decodificacao_ms;"
              "atraso_captura_ate_pc_ms;rtt_ms;camera_em_uso;cel_fps_alvo;wifi_mhz;wifi_rssi_dbm;wifi_link_mbps;tela_ligada;"
              "cel_rede_ms;pc_descartados;pc_gpu\n",
              csv);

    auto requestKeyframe = [&] {
        int64_t now = NowUs();
        int64_t last = lastKeyReq;
        if (now - last > 500000 && lastKeyReq.compare_exchange_strong(last, now)) conn.Send(proto::KEYFRAME_REQ, {});
    };

    // ---- prévia da janela
    PreviewJob job;
    std::thread previewer([&] {
        for (;;) {
            {
                std::unique_lock<std::mutex> l(job.lock);
                job.cv.wait(l, [&] { return job.closed || job.pending; });
                if (job.closed) return;
            }
            // Enquanto `pending`, a decodificação não mexe em job.nv12.
            int sw = job.width, sh = job.height;
            int maxW = m_previewMaxW, maxH = m_previewMaxH;
            if (maxW > 16 && maxH > 16) {
                int pw = maxW, ph = (int)((int64_t)maxW * sh / sw);
                if (ph > maxH) {
                    ph = maxH;
                    pw = (int)((int64_t)maxH * sw / sh);
                }
                pw &= ~1;
                {
                    std::lock_guard<std::mutex> lock(m_previewLock);
                    m_preview.resize((size_t)pw * ph * 4);
                    img::NV12ToBGRABilinear(job.nv12.data(), sw, job.nv12.data() + (size_t)sw * sh, sw, sw, sh,
                                            m_preview.data(), pw * 4, pw, ph);
                    m_previewW = pw;
                    m_previewH = ph;
                    m_previewNew = true;
                    m_previewPosted = true;
                }
                if (m_onPreview) m_onPreview();
            }
            std::lock_guard<std::mutex> l(job.lock);
            job.pending = false;
        }
    });

    // ---- decodificação
    PacketQueue queue;
    std::thread decoderThread([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        H264Decoder decoder;
        bool waitKey = false;

        auto onFrame = [&](const DecodedFrame& f) {
            m_writer.Write(f.y, f.yPitch, f.uv, f.uvPitch, f.width, f.height);
            m_streaming = true;
            m_lastFrameTick = GetTickCount64();
            frames++;
            int64_t latency = NowUs() + m_clockOffset - f.ptsUs;
            if (m_bestRtt != INT64_MAX && latency > 0 && latency < 5000000) {
                latencySum += latency;
                latencyCount++;
            }
            // Só entrega à prévia se a anterior já foi desenhada e a conversão está livre.
            if (m_previewPosted || m_previewMaxW <= 16 || m_previewMaxH <= 16 || f.width <= 0 || f.height <= 0)
                return;
            {
                std::lock_guard<std::mutex> l(job.lock);
                if (job.pending) return;
                job.nv12.resize((size_t)f.width * f.height * 3 / 2);
                uint8_t* dy = job.nv12.data();
                for (int r = 0; r < f.height; r++)
                    memcpy(dy + (size_t)r * f.width, f.y + (size_t)r * f.yPitch, f.width);
                uint8_t* duv = dy + (size_t)f.width * f.height;
                for (int r = 0; r < f.height / 2; r++)
                    memcpy(duv + (size_t)r * f.width, f.uv + (size_t)r * f.uvPitch, f.width);
                job.width = f.width;
                job.height = f.height;
                job.pending = true;
            }
            job.cv.notify_one();
        };

        int curW = 0, curH = 0, curFps = 0;
        int hwFailures = 0;
        std::string lastColor;
        auto initDecoder = [&] {
            if (!decoder.Init(curW, curH, curFps)) return false;
            hwDecode = decoder.Hardware();
            Log("decodificador: %s%ls", decoder.Hardware() ? "placa de vídeo, " : "CPU",
                decoder.Hardware() ? decoder.AdapterName().c_str() : L"");
            waitKey = false;
            hwFailures = 0;
            requestKeyframe();
            return true;
        };

        PacketQueue::Item item;
        while (queue.Pop(item)) {
            if (item.type == proto::CONFIG) {
                Reader c(item.data);
                c.U16();  // codec (já conferido na recepção)
                curW = c.U16();
                curH = c.U16();
                curFps = c.U16();
                if (!initDecoder()) {
                    decoderFailed = true;
                    conn.Close();
                    break;
                }
                continue;
            }
            Reader r(item.data);
            uint8_t flags = r.U8();
            int64_t pts = r.I64();
            if (!r.ok || !decoder.Ready()) continue;
            bool isKey = (flags & proto::VIDEO_KEY) != 0;
            bool isConfig = (flags & proto::VIDEO_CONFIG) != 0;
            if (!isKey && !isConfig && !waitKey &&
                (NowUs() - item.arrivedUs > kMaxDecodeLagUs || queue.Size() > kMaxDecodeQueue)) {
                waitKey = true;
                Log("decodificação atrasada (%lld ms, %zu na fila): pulando até o quadro-chave",
                    (long long)((NowUs() - item.arrivedUs) / 1000), queue.Size());
            }
            if (waitKey && !isKey && !isConfig) {
                pcDropped++;
                requestKeyframe();
                continue;
            }
            if (isKey) waitKey = false;
            if (isKey || isConfig) {
                H264Color color;
                if (ReadH264Color(r.Rest(), r.Remaining(), color)) {
                    std::string text = DescribeH264Color(color);
                    if (text != lastColor) {
                        lastColor = text;
                        Log("cor do vídeo: %s%s", text.c_str(),
                            H264ColorMatchesPc(color) ? "" : " — DIFERENTE do que o PC supõe (BT.709 limitada)");
                    }
                }
            }
            int64_t decodeStart = NowUs();
            bool decoded = decoder.Decode(r.Rest(), r.Remaining(), pts, onFrame);
            decodeSum += NowUs() - decodeStart;
            decodeCount++;
            if (!decoded) {
                Log("decode falhou (flags=%d, %zu bytes): %s", flags, r.Remaining(), decoder.LastError());
                // A placa de vídeo aceitou a configuração mas não decodifica: passa para a CPU.
                if (decoder.Hardware() && ++hwFailures >= 5) {
                    Log("placa de vídeo falhou %d vezes seguidas: usando a CPU", hwFailures);
                    decoder.ForceSoftware();
                    if (!initDecoder()) {
                        decoderFailed = true;
                        conn.Close();
                        break;
                    }
                    continue;
                }
                requestKeyframe();
            } else {
                hwFailures = 0;
            }
        }
        decoder.Close();
        CoUninitialize();
    });

    // GODOICAM_DUMP=arquivo grava os pacotes recebidos (u32 tamanho, i64 pts, H.264) para depuração.
    FILE* dump = nullptr;
    wchar_t dumpPath[MAX_PATH];
    if (GetEnvironmentVariableW(L"GODOICAM_DUMP", dumpPath, MAX_PATH)) _wfopen_s(&dump, dumpPath, L"wb");

    // ---- recepção (esta thread)
    uint8_t type = 0;
    Bytes payload;
    while (!m_stopping && conn.Recv(type, payload)) {
        if (type == proto::VIDEO && payload.size() >= 9) {
            if (dump) {
                uint32_t n = (uint32_t)payload.size() - 9;
                fwrite(&n, 4, 1, dump);
                fwrite(payload.data() + 1, 8, 1, dump);
                fwrite(payload.data() + 9, 1, n, dump);
                fflush(dump);
            }
            bytes += payload.size();
            // Confirma na hora (o pts): o celular mede a rede por aqui e limita o vídeo em trânsito.
            if (!(payload[0] & proto::VIDEO_CONFIG)) conn.Send(proto::FRAME_ACK, Bytes(payload.begin() + 1, payload.begin() + 9));
            queue.Push(type, std::move(payload), NowUs());
            payload = Bytes();
        } else if (type == proto::CONFIG) {
            Reader r(payload);
            uint16_t codec = r.U16();
            cfgW = r.U16();
            cfgH = r.U16();
            cfgFps = r.U16();
            cfgBitrate = r.U32();
            cfgDesc = r.Str();
            Log("CONFIG %dx%d %d fps %u bps (%s)", cfgW, cfgH, cfgFps, cfgBitrate, cfgDesc.c_str());
            if (codec != 1) {
                decoderFailed = true;
                break;
            }
            queue.Push(type, std::move(payload), NowUs());
            payload = Bytes();
        } else if (type == proto::PONG) {
            Reader r(payload);
            int64_t sent = r.I64(), phoneTime = r.I64();
            int64_t rtt = NowUs() - sent;
            m_rtt = rtt;
            if (r.ok && rtt >= 0 && rtt <= m_bestRtt) {
                m_bestRtt = rtt;
                m_clockOffset = phoneTime - (sent + rtt / 2);
            }
        } else if (type == proto::CAMERA_STATE) {
            Reader r(payload);
            PhoneCamera c;
            c.label = Widen(r.Str());
            c.cameraCount = r.U8();
            uint8_t flags = r.U8();
            c.torchAvailable = flags & 1;
            c.torchOn = flags & 2;
            c.manualExposure = flags & 4;
            c.manualFocus = flags & 8;
            c.kelvinAvailable = flags & 16;
            c.focusCalibrated = flags & 32;
            c.zoom = r.U16();
            c.zoomMin = r.U16();
            c.zoomMax = r.U16();
            c.ev = (int16_t)r.U16();
            c.evMin = (int16_t)r.U16();
            c.evMax = (int16_t)r.U16();
            c.evPerStop = r.U16();
            c.iso = r.U16();
            c.isoMin = r.U16();
            c.isoMax = r.U16();
            c.exposureUs = (int)r.U32();
            c.exposureMinUs = (int)r.U32();
            c.exposureMaxUs = (int)r.U32();
            uint16_t focus = r.U16();
            c.focus = focus == 0xffff ? -1 : focus;
            c.focusMax = r.U16();
            c.kelvin = r.U16();
            c.valid = r.ok;
            if (c.evPerStop < 1) c.evPerStop = 1;
            std::lock_guard<std::mutex> lock(m_lock);
            m_camera = c;
        } else if (type == proto::REPORT) {
            std::wstring path = AppDataFile((L"relatorio-" + Widen(phoneName) + L".txt").c_str());
            if (FILE* f = _wfopen(path.c_str(), L"wb")) {
                fwrite(payload.data(), 1, payload.size(), f);
                fclose(f);
            }
            Log("relatorio do aparelho salvo (%zu bytes)", payload.size());
        } else if (type == proto::STATS) {
            Reader r(payload);
            phone.camFps = r.U16();
            phone.encFps = r.U16();
            phone.bits = r.U32();
            phone.target = r.U32();
            phone.dropped = r.U16();
            phone.queued = r.U16();
            phone.encodeMs = (int16_t)r.U16();
            phone.tempTenths = (int16_t)r.U16();
            phone.thermal = r.U8();
            if (r.Remaining() >= 8) {  // celular com diagnóstico de Wi-Fi
                phone.wifiMhz = r.U16();
                phone.rssi = (int16_t)r.U16();
                phone.linkMbps = r.U16();
                phone.screenOn = r.U8();
                phone.fpsTarget = r.U8();
            }
            if (r.Remaining() >= 2) phone.netMs = (int16_t)r.U16();  // celular com confirmação de quadros
            phone.valid = r.ok;
        } else if (type == proto::BYE || type == proto::ERROR_MSG) {
            break;
        }

        int64_t now = NowUs();
        if (now - windowStart >= 1000000) {
            double secs = (now - windowStart) / 1e6;
            int nFrames = frames.exchange(0);
            int nLatency = latencyCount.exchange(0);
            int64_t sumLatency = latencySum.exchange(0);
            int nDecode = decodeCount.exchange(0);
            int64_t sumDecode = decodeSum.exchange(0);
            int nDropped = pcDropped.exchange(0);
            int latencyMs = nLatency ? (int)(sumLatency / nLatency / 1000) : 0;
            double decodeMs = nDecode ? sumDecode / 1000.0 / nDecode : 0.0;
            if (nFrames > 0) {
                wchar_t brief[128], stats[640];
                swprintf_s(brief, L"%d×%d • %.0f fps • %d ms", cfgW, cfgH, nFrames / secs, latencyMs);
                swprintf_s(stats,
                           L"Celular: câmera %d fps • enviando %d fps • %.1f Mbps • codificação %d ms • rede %d ms • "
                           L"%.1f °C • Wi-Fi %.1f GHz %d dBm\n"
                           L"PC: %.0f fps • decodificação %.1f ms (%s) • atraso captura→PC %d ms • RTT %d ms • "
                           L"%d descartados • câmera virtual %s",
                           phone.camFps, phone.encFps, phone.bits / 1e6, phone.encodeMs, phone.netMs,
                           phone.tempTenths / 10.0, phone.wifiMhz / 1000.0, phone.rssi, nFrames / secs, decodeMs,
                           hwDecode ? L"GPU" : L"CPU",
                           latencyMs, (int)(m_rtt / 1000), nDropped, m_writer.CameraInUse() ? L"em uso" : L"livre");
                if (GetStatus().state != State::Streaming) SetStatus(State::Streaming, L"Transmitindo de " + wname);
                SetStats(brief, stats);
            } else if (GetStatus().state != State::Connected) {
                SetStatus(State::Connected, L"Conectado a " + wname, L"Aguardando vídeo…");
            }
            if (csv) {
                SYSTEMTIME t;
                GetLocalTime(&t);
                fprintf(csv,
                        "%02d:%02d:%02d;%s;%dx%d@%d;%d;%d;%.2f;%.2f;%d;%d;%d;%.1f;%d;%.1f;%.2f;%.2f;%d;%d;%d;%d;%d;%d;%d;%d;"
                        "%d;%d;%d\n",
                        t.wHour, t.wMinute, t.wSecond, phoneName.c_str(), cfgW, cfgH, cfgFps, phone.camFps,
                        phone.encFps, phone.bits / 1e6, phone.target / 1e6, phone.dropped, phone.queued,
                        phone.encodeMs, phone.tempTenths / 10.0, phone.thermal, nFrames / secs,
                        bytes * 8 / secs / 1e6, decodeMs, nLatency ? latencyMs : -1, (int)(m_rtt / 1000),
                        m_writer.CameraInUse() ? 1 : 0, phone.fpsTarget, phone.wifiMhz, phone.rssi, phone.linkMbps,
                        phone.screenOn, phone.netMs, nDropped, hwDecode ? 1 : 0);
                fflush(csv);
            }
            windowStart = now;
            bytes = 0;
        }
    }

    if (dump) fclose(dump);
    if (csv) fclose(csv);
    done = true;
    conn.Close();
    queue.Close();
    decoderThread.join();
    {
        std::lock_guard<std::mutex> l(job.lock);
        job.closed = true;
    }
    job.cv.notify_all();
    previewer.join();
    pinger.join();
    m_streaming = false;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_camera = PhoneCamera{};
    }
    if (decoderFailed && !m_stopping) {
        SetStatus(State::Failed, L"Não foi possível iniciar o decodificador H.264 do Windows");
    } else if (!m_stopping) {
        SetStatus(State::Failed, L"Conexão com " + wname + L" perdida", L"Reconectando…");
    }
}

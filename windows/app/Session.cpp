#include "Session.h"

#include <algorithm>
#include <share.h>
#include <stdio.h>

#include "ImageUtil.h"
#include "System.h"

namespace {

std::wstring ErrorText(const Bytes& payload) {
    if (payload.size() <= 1) return L"O celular recusou a conexão.";
    return Widen(std::string(payload.begin() + 1, payload.end()));
}

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
    m_previewPosted = false;
    std::lock_guard<std::mutex> lock(m_previewLock);
    if (!m_previewNew) return false;
    bgra = m_preview;
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
        .U32(0);
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

    H264Decoder decoder;
    int cfgW = 0, cfgH = 0, cfgFps = 0;
    uint32_t cfgBitrate = 0;
    std::string cfgDesc;
    int64_t lastKeyReq = 0;
    int64_t windowStart = NowUs();
    int frames = 0;
    uint64_t bytes = 0;
    int64_t latencySum = 0;
    int latencyCount = 0;
    int64_t decodeSum = 0;
    int decodeCount = 0;
    struct PhoneStats {
        bool valid = false;
        int camFps = 0, encFps = 0, dropped = 0, queued = 0, encodeMs = -1, tempTenths = 0, thermal = 0;
        uint32_t bits = 0, target = 0;
    } phone;

    // Uma linha por segundo em %APPDATA%\GodoiCan\stats.csv (Fase 0: medições).
    std::wstring csvPath = AppDataFile(L"stats.csv");
    bool csvNew = GetFileAttributesW(csvPath.c_str()) == INVALID_FILE_ATTRIBUTES;
    FILE* csv = _wfsopen(csvPath.c_str(), L"ab", _SH_DENYWR);
    if (csv && csvNew)
        fputs("hora;celular;resolucao;cel_cam_fps;cel_env_fps;cel_mbps;cel_alvo_mbps;cel_descartados;cel_fila;"
              "cel_codificacao_ms;cel_temp_c;cel_termico;pc_fps;pc_mbps;pc_decodificacao_ms;"
              "atraso_captura_ate_pc_ms;rtt_ms;camera_em_uso\n",
              csv);

    auto requestKeyframe = [&] {
        int64_t now = NowUs();
        if (now - lastKeyReq > 500000) {
            lastKeyReq = now;
            conn.Send(proto::KEYFRAME_REQ, {});
        }
    };

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
        // Prévia no tamanho exato da área de vídeo da janela (a janela só copia, sem esticar).
        // Só gera se a anterior já foi desenhada e a janela está visível.
        int maxW = m_previewMaxW, maxH = m_previewMaxH;
        if (!m_previewPosted && maxW > 16 && maxH > 16 && f.width > 0 && f.height > 0) {
            int pw = maxW, ph = (int)((int64_t)maxW * f.height / f.width);
            if (ph > maxH) {
                ph = maxH;
                pw = (int)((int64_t)maxH * f.width / f.height);
            }
            {
                std::lock_guard<std::mutex> lock(m_previewLock);
                m_preview.resize((size_t)pw * ph * 4);
                img::NV12ToBGRABilinear(f.y, f.yPitch, f.uv, f.uvPitch, f.width, f.height, m_preview.data(),
                                        pw * 4, pw, ph);
                m_previewW = pw;
                m_previewH = ph;
                m_previewNew = true;
            }
            m_previewPosted = true;
            if (m_onPreview) m_onPreview();
        }
    };

    // GODOICAN_DUMP=arquivo grava os pacotes recebidos (u32 tamanho, i64 pts, H.264) para depuração.
    FILE* dump = nullptr;
    wchar_t dumpPath[MAX_PATH];
    if (GetEnvironmentVariableW(L"GODOICAN_DUMP", dumpPath, MAX_PATH)) _wfopen_s(&dump, dumpPath, L"wb");

    uint8_t type = 0;
    Bytes payload;
    while (!m_stopping && conn.Recv(type, payload)) {
        if (type == proto::VIDEO && dump && payload.size() >= 9) {
            uint32_t n = (uint32_t)payload.size() - 9;
            fwrite(&n, 4, 1, dump);
            fwrite(payload.data() + 1, 8, 1, dump);
            fwrite(payload.data() + 9, 1, n, dump);
            fflush(dump);
        }
        if (type == proto::VIDEO) {
            Reader r(payload);
            uint8_t flags = r.U8();
            int64_t pts = r.I64();
            if (!r.ok || !decoder.Ready()) continue;
            bytes += payload.size();
            int64_t decodeStart = NowUs();
            bool decoded = decoder.Decode(r.Rest(), r.Remaining(), pts, onFrame);
            decodeSum += NowUs() - decodeStart;
            decodeCount++;
            if (!decoded) {
                Log("decode falhou (flags=%d, %zu bytes): %s", flags, r.Remaining(), decoder.LastError());
                requestKeyframe();
            }
            (void)flags;
        } else if (type == proto::CONFIG) {
            Reader r(payload);
            uint16_t codec = r.U16();
            cfgW = r.U16();
            cfgH = r.U16();
            cfgFps = r.U16();
            cfgBitrate = r.U32();
            cfgDesc = r.Str();
            Log("CONFIG %dx%d %d fps %u bps (%s)", cfgW, cfgH, cfgFps, cfgBitrate, cfgDesc.c_str());
            if (codec != 1 || !decoder.Init(cfgW, cfgH, cfgFps)) {
                SetStatus(State::Failed, L"Não foi possível iniciar o decodificador H.264 do Windows");
                break;
            }
            requestKeyframe();
        } else if (type == proto::PONG) {
            Reader r(payload);
            int64_t sent = r.I64(), phone = r.I64();
            int64_t rtt = NowUs() - sent;
            m_rtt = rtt;
            if (r.ok && rtt >= 0 && rtt <= m_bestRtt) {
                m_bestRtt = rtt;
                m_clockOffset = phone - (sent + rtt / 2);
            }
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
            phone.valid = r.ok;
        } else if (type == proto::BYE || type == proto::ERROR_MSG) {
            break;
        }

        int64_t now = NowUs();
        if (now - windowStart >= 1000000) {
            double secs = (now - windowStart) / 1e6;
            if (frames > 0) {
                int latencyMs = latencyCount ? (int)(latencySum / latencyCount / 1000) : 0;
                wchar_t brief[128], stats[512];
                swprintf_s(brief, L"%d×%d • %.0f fps • %d ms", cfgW, cfgH, frames / secs, latencyMs);
                swprintf_s(stats,
                           L"Celular: câmera %d fps • enviando %d fps • %.1f Mbps • codificação %d ms • %.1f °C\n"
                           L"PC: %.0f fps • decodificação %.1f ms • atraso captura→PC %d ms • rede %d ms • "
                           L"câmera virtual %s",
                           phone.camFps, phone.encFps, phone.bits / 1e6, phone.encodeMs, phone.tempTenths / 10.0,
                           frames / secs, decodeCount ? decodeSum / 1000.0 / decodeCount : 0.0, latencyMs,
                           (int)(m_rtt / 1000), m_writer.CameraInUse() ? L"em uso" : L"livre");
                if (GetStatus().state != State::Streaming) SetStatus(State::Streaming, L"Transmitindo de " + wname);
                SetStats(brief, stats);
            } else if (GetStatus().state != State::Connected) {
                SetStatus(State::Connected, L"Conectado a " + wname, L"Aguardando vídeo…");
            }
            if (csv) {
                SYSTEMTIME t;
                GetLocalTime(&t);
                fprintf(csv, "%02d:%02d:%02d;%s;%dx%d@%d;%d;%d;%.2f;%.2f;%d;%d;%d;%.1f;%d;%.1f;%.2f;%.2f;%d;%d;%d\n",
                        t.wHour, t.wMinute, t.wSecond, phoneName.c_str(), cfgW, cfgH, cfgFps, phone.camFps,
                        phone.encFps, phone.bits / 1e6, phone.target / 1e6, phone.dropped, phone.queued,
                        phone.encodeMs, phone.tempTenths / 10.0, phone.thermal, frames / secs,
                        bytes * 8 / secs / 1e6, decodeCount ? decodeSum / 1000.0 / decodeCount : 0.0,
                        latencyCount ? (int)(latencySum / latencyCount / 1000) : -1, (int)(m_rtt / 1000),
                        m_writer.CameraInUse() ? 1 : 0);
                fflush(csv);
            }
            decodeSum = 0;
            decodeCount = 0;
            windowStart = now;
            frames = 0;
            bytes = 0;
            latencySum = 0;
            latencyCount = 0;
        }
    }

    if (dump) fclose(dump);
    if (csv) fclose(csv);
    done = true;
    conn.Close();
    pinger.join();
    decoder.Close();
    m_streaming = false;
    if (!m_stopping) SetStatus(State::Failed, L"Conexão com " + wname + L" perdida", L"Reconectando…");
}

#include "Net.h"

#include <shlobj.h>
#include <stdarg.h>
#include <share.h>
#include <stdio.h>

void Log(const char* fmt, ...) {
    static std::mutex lock;
    static FILE* file = [] {
        PWSTR dir = nullptr;
        FILE* f = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &dir))) {
            std::wstring path = std::wstring(dir) + L"\\GodoiCan";
            CreateDirectoryW(path.c_str(), nullptr);
            path += L"\\log.txt";
            f = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
        }
        CoTaskMemFree(dir);
        return f;
    }();
    if (!file) return;
    std::lock_guard<std::mutex> guard(lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fputc('\n', file);
    fflush(file);
}

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), s.data(), n, nullptr, nullptr);
    return s;
}

Conn::~Conn() { Close(); }

bool Conn::Connect(const std::string& host, uint16_t port, int timeoutMs, std::wstring& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        error = L"Endereço inválido: " + Widen(host);
        return false;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);

    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(s, &writable);
    FD_SET(s, &failed);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int r = select(0, nullptr, &writable, &failed, &tv);
    if (r <= 0 || FD_ISSET(s, &failed)) {
        closesocket(s);
        error = r == 0 ? L"O celular não respondeu (" + Widen(host) + L")."
                       : L"Conexão recusada por " + Widen(host) + L". O GodoiCan está aberto no celular?";
        return false;
    }
    nonblocking = 0;
    ioctlsocket(s, FIONBIO, &nonblocking);
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    DWORD sendTimeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeout, sizeof(sendTimeout));
    int rcvbuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    m_sock = s;
    return true;
}

void Conn::SetRecvTimeout(int ms) {
    DWORD t = (DWORD)ms;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
}

void Conn::EnableCrypto(const Bytes& sendKey, const Bytes& recvKey) {
    std::lock_guard<std::mutex> lock(m_sendLock);
    m_send = std::make_unique<crypto::Aead>(sendKey);
    m_recv = std::make_unique<crypto::Aead>(recvKey);
}

bool Conn::Send(uint8_t type, const Bytes& payload) {
    std::lock_guard<std::mutex> lock(m_sendLock);
    if (m_sock == INVALID_SOCKET) return false;
    Bytes body;
    const Bytes* out = &payload;
    if (m_send) {
        if (!m_send->Seal(type, payload.data(), payload.size(), body)) return false;
        out = &body;
    }
    Bytes frame;
    frame.reserve(5 + out->size());
    Writer w;
    w.U8(type).U32((uint32_t)out->size());
    frame = std::move(w.b);
    frame.insert(frame.end(), out->begin(), out->end());
    size_t sent = 0;
    while (sent < frame.size()) {
        int n = send(m_sock, (const char*)frame.data() + sent, (int)(frame.size() - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool Conn::RecvAll(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = recv(m_sock, (char*)buf + got, (int)(len - got), 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

bool Conn::Recv(uint8_t& type, Bytes& payload) {
    if (m_sock == INVALID_SOCKET) return false;
    uint8_t header[5];
    if (!RecvAll(header, 5)) return false;
    type = header[0];
    uint32_t len = uint32_t(header[1]) << 24 | header[2] << 16 | header[3] << 8 | header[4];
    if (len > proto::MAX_FRAME) return false;
    if (!m_recv) {
        payload.resize(len);
        return len == 0 || RecvAll(payload.data(), len);
    }
    m_frame.resize(len);
    if (len && !RecvAll(m_frame.data(), len)) return false;
    return m_recv->Open(type, m_frame.data(), len, payload);
}

void Conn::Close() {
    // shutdown() primeiro e fora da trava: desbloqueia um send()/recv() parado em outra thread.
    SOCKET s = m_sock;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    std::lock_guard<std::mutex> lock(m_sendLock);
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
}

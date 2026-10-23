// Conexão TCP com o celular e enquadramento das mensagens (docs/PROTOCOLO.md).
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <memory>
#include <mutex>
#include <string>

#include "Crypto.h"

namespace proto {
constexpr uint16_t TCP_PORT = 47180;
constexpr uint16_t UDP_PORT = 47181;
constexpr uint16_t VERSION = 1;
constexpr uint32_t MAX_FRAME = 16 * 1024 * 1024;

enum Type : uint8_t {
    HELLO = 0x01,
    CHALLENGE = 0x02,
    PAIR_CHALLENGE = 0x03,
    AUTH = 0x04,
    AUTH_OK = 0x05,
    ERROR_MSG = 0x06,
    CONFIG = 0x10,
    VIDEO = 0x11,
    PING = 0x12,
    PONG = 0x13,
    KEYFRAME_REQ = 0x14,
    BYE = 0x15,
};

enum Error : uint8_t {
    ERR_NOT_PAIRED = 1,
    ERR_BAD_CODE = 2,
    ERR_BUSY = 3,
    ERR_VERSION = 4,
    ERR_AUTH = 5,
    ERR_TIMEOUT = 6,
};

constexpr uint8_t HELLO_FORCE_PAIR = 1;
constexpr uint8_t VIDEO_KEY = 1;
constexpr uint8_t VIDEO_CONFIG = 2;
}  // namespace proto

// Monta payloads big-endian.
class Writer {
public:
    Writer& U8(uint8_t v) { b.push_back(v); return *this; }
    Writer& U16(uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); return *this; }
    Writer& U32(uint32_t v) { for (int i = 3; i >= 0; i--) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Writer& I64(int64_t v) { for (int i = 7; i >= 0; i--) b.push_back(uint8_t((uint64_t)v >> (8 * i))); return *this; }
    Writer& Raw(const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); return *this; }
    Writer& Str(const std::string& utf8) {
        size_t n = utf8.size() > 1024 ? 1024 : utf8.size();
        U16((uint16_t)n);
        b.insert(b.end(), utf8.begin(), utf8.begin() + n);
        return *this;
    }
    Bytes b;
};

// Lê payloads big-endian; `ok` vira falso se faltar dado.
class Reader {
public:
    Reader(const uint8_t* data, size_t len) : p(data), end(data + len) {}
    explicit Reader(const Bytes& data) : Reader(data.data(), data.size()) {}
    uint8_t U8() { return Need(1) ? *p++ : 0; }
    uint16_t U16() { if (!Need(2)) return 0; uint16_t v = uint16_t(p[0] << 8 | p[1]); p += 2; return v; }
    uint32_t U32() { if (!Need(4)) return 0; uint32_t v = uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; p += 4; return v; }
    int64_t I64() { if (!Need(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; p += 8; return (int64_t)v; }
    Bytes Raw(size_t n) { if (!Need(n)) return {}; Bytes v(p, p + n); p += n; return v; }
    std::string Str() { uint16_t n = U16(); if (!Need(n)) return {}; std::string s((const char*)p, n); p += n; return s; }
    const uint8_t* Rest() const { return p; }
    size_t Remaining() const { return size_t(end - p); }
    bool ok = true;

private:
    bool Need(size_t n) { if (size_t(end - p) < n) { ok = false; return false; } return true; }
    const uint8_t* p;
    const uint8_t* end;
};

class Conn {
public:
    ~Conn();
    bool Connect(const std::string& host, uint16_t port, int timeoutMs, std::wstring& error);
    void SetRecvTimeout(int ms);
    bool Send(uint8_t type, const Bytes& payload);
    bool Recv(uint8_t& type, Bytes& payload);
    void Close();
    void EnableCrypto(const Bytes& sendKey, const Bytes& recvKey);

private:
    bool RecvAll(uint8_t* buf, size_t len);
    SOCKET m_sock = INVALID_SOCKET;
    std::mutex m_sendLock;
    std::unique_ptr<crypto::Aead> m_send;
    std::unique_ptr<crypto::Aead> m_recv;
    Bytes m_frame;
};

// Registro de diagnóstico em %APPDATA%\GodoiCan\log.txt (sem conteúdo de vídeo nem chaves).
void Log(const char* fmt, ...);

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);

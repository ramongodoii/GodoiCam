// Criptografia do protocolo (docs/PROTOCOLO.md) com a CNG do Windows (bcrypt).
#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <stdint.h>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

namespace crypto {

Bytes Random(size_t n);
Bytes Label(const char* s);
Bytes Concat(std::initializer_list<const Bytes*> parts);
Bytes Sha256(const Bytes& data);
Bytes Hmac(const Bytes& key, const Bytes& data);
bool Equal(const Bytes& a, const Bytes& b);

// Par de chaves P-256 efêmero, usado só no pareamento.
class Ecdh {
public:
    Ecdh();
    ~Ecdh();
    Ecdh(const Ecdh&) = delete;
    Ecdh& operator=(const Ecdh&) = delete;

    Bytes PublicBytes() const;  // 0x04 || X || Y
    bool Agree(const Bytes& peer, Bytes& secret) const;  // coordenada X, big-endian

private:
    BCRYPT_ALG_HANDLE m_alg = nullptr;
    BCRYPT_KEY_HANDLE m_key = nullptr;
};

// AES-256-GCM com nonce de contador; uma instância por direção.
class Aead {
public:
    explicit Aead(const Bytes& key);
    ~Aead();
    Aead(const Aead&) = delete;
    Aead& operator=(const Aead&) = delete;

    bool Seal(uint8_t type, const uint8_t* data, size_t len, Bytes& out);
    bool Open(uint8_t type, const uint8_t* data, size_t len, Bytes& out);

private:
    void Nonce(uint8_t out[12]);
    BCRYPT_ALG_HANDLE m_alg = nullptr;
    BCRYPT_KEY_HANDLE m_key = nullptr;
    uint64_t m_counter = 0;
};

std::string Hex(const Bytes& b);
Bytes FromHex(const std::string& s);

}  // namespace crypto

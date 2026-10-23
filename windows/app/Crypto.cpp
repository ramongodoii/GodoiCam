#include "Crypto.h"

#include <algorithm>

#pragma comment(lib, "bcrypt.lib")

namespace crypto {

Bytes Random(size_t n) {
    Bytes b(n);
    BCryptGenRandom(nullptr, b.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return b;
}

Bytes Label(const char* s) { return Bytes(s, s + strlen(s)); }

Bytes Concat(std::initializer_list<const Bytes*> parts) {
    Bytes out;
    for (const Bytes* p : parts) out.insert(out.end(), p->begin(), p->end());
    return out;
}

Bytes Sha256(const Bytes& data) {
    Bytes out(32);
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), out.data(), 32);
    return out;
}

Bytes Hmac(const Bytes& key, const Bytes& data) {
    Bytes out(32);
    BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, (PUCHAR)key.data(), (ULONG)key.size(), (PUCHAR)data.data(),
               (ULONG)data.size(), out.data(), 32);
    return out;
}

bool Equal(const Bytes& a, const Bytes& b) {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

std::string Hex(const Bytes& b) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t v : b) {
        s += digits[v >> 4];
        s += digits[v & 15];
    }
    return s;
}

Bytes FromHex(const std::string& s) {
    Bytes b;
    for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back((uint8_t)std::stoi(s.substr(i, 2), nullptr, 16));
    return b;
}

// ------------------------------------------------------------------------------------ ECDH

Ecdh::Ecdh() {
    if (BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0) != 0) return;
    if (BCryptGenerateKeyPair(m_alg, &m_key, 256, 0) != 0) return;
    BCryptFinalizeKeyPair(m_key, 0);
}

Ecdh::~Ecdh() {
    if (m_key) BCryptDestroyKey(m_key);
    if (m_alg) BCryptCloseAlgorithmProvider(m_alg, 0);
}

Bytes Ecdh::PublicBytes() const {
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG size = 0;
    if (!m_key || BCryptExportKey(m_key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &size, 0) != 0)
        return {};
    Bytes out{4};
    out.insert(out.end(), blob + sizeof(BCRYPT_ECCKEY_BLOB), blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    return out;
}

bool Ecdh::Agree(const Bytes& peer, Bytes& secret) const {
    if (!m_key || peer.size() != 65 || peer[0] != 4) return false;
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    auto* header = (BCRYPT_ECCKEY_BLOB*)blob;
    header->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), peer.data() + 1, 64);

    BCRYPT_KEY_HANDLE peerKey = nullptr;
    BCRYPT_SECRET_HANDLE agreed = nullptr;
    bool ok = BCryptImportKeyPair(m_alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peerKey, blob, sizeof(blob), 0) == 0 &&
              BCryptSecretAgreement(m_key, peerKey, &agreed, 0) == 0;
    if (ok) {
        secret.assign(32, 0);
        ULONG size = 0;
        ok = BCryptDeriveKey(agreed, BCRYPT_KDF_RAW_SECRET, nullptr, secret.data(), 32, &size, 0) == 0 && size == 32;
        // BCRYPT_KDF_RAW_SECRET devolve o segredo em little-endian.
        std::reverse(secret.begin(), secret.end());
    }
    if (agreed) BCryptDestroySecret(agreed);
    if (peerKey) BCryptDestroyKey(peerKey);
    return ok;
}

// ------------------------------------------------------------------------------------ AES-GCM

Aead::Aead(const Bytes& key) {
    if (BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return;
    BCryptSetProperty(m_alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    BCryptGenerateSymmetricKey(m_alg, &m_key, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0);
}

Aead::~Aead() {
    if (m_key) BCryptDestroyKey(m_key);
    if (m_alg) BCryptCloseAlgorithmProvider(m_alg, 0);
}

void Aead::Nonce(uint8_t out[12]) {
    memset(out, 0, 4);
    uint64_t c = m_counter++;
    for (int i = 0; i < 8; i++) out[4 + i] = (uint8_t)(c >> (56 - 8 * i));
}

bool Aead::Seal(uint8_t type, const uint8_t* data, size_t len, Bytes& out) {
    if (!m_key) return false;
    uint8_t nonce[12];
    Nonce(nonce);
    out.resize(len + 16);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = 12;
    info.pbAuthData = &type;
    info.cbAuthData = 1;
    info.pbTag = out.data() + len;
    info.cbTag = 16;
    ULONG written = 0;
    return BCryptEncrypt(m_key, (PUCHAR)data, (ULONG)len, &info, nullptr, 0, out.data(), (ULONG)len, &written, 0) ==
           0;
}

bool Aead::Open(uint8_t type, const uint8_t* data, size_t len, Bytes& out) {
    if (!m_key || len < 16) return false;
    uint8_t nonce[12];
    Nonce(nonce);
    size_t plain = len - 16;
    out.resize(plain);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = 12;
    info.pbAuthData = &type;
    info.cbAuthData = 1;
    info.pbTag = (PUCHAR)data + plain;
    info.cbTag = 16;
    ULONG written = 0;
    return BCryptDecrypt(m_key, (PUCHAR)data, (ULONG)plain, &info, nullptr, 0, out.data(), (ULONG)plain, &written,
                         0) == 0;
}

}  // namespace crypto

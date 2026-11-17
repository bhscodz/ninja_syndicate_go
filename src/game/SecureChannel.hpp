// Encrypted + authenticated packet channel on top of sf::TcpSocket packets.
//
// Primitives all come from mbedTLS (bundled with SFML): X25519 key agreement,
// HKDF-SHA256 and ChaCha20-Poly1305. Nothing here invents crypto.
//
// Handshake (Noise-NK-like, one round trip):
//   client -> server : magic, Qc                    (client ephemeral X25519 public key)
//   server -> client : magic, Qs, S                 (server ephemeral key, server long-term public key)
//   both derive      : ikm  = X25519(ec, Qs) || X25519(ec, S)   (server: X25519(es, Qc) || X25519(s, Qc))
//                      keys = HKDF-SHA256(ikm, salt = SHA256(Qc||Qs||S), "stt-v1")  -> k_c2s, k_s2c
//   server -> client : first AEAD packet (counter 0). Only the holder of S's private key can
//                      produce it, so a successful decrypt authenticates the server to the pinned key.
// Every later packet is ChaCha20-Poly1305 with a per-direction 64-bit counter used as nonce; the
// receiver only accepts the exact next counter, so replay / reorder / truncation is rejected.
#pragma once
#include <SFML/Network.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <mbedtls/chachapoly.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

namespace sec {

constexpr uint32_t MAGIC = 0x53545431;      // "STT1"
constexpr size_t   KEY_LEN = 32, TAG_LEN = 16;
constexpr size_t   MAX_PLAIN = 4096;         // largest accepted plaintext packet
constexpr size_t   MAX_WIRE = MAX_PLAIN + TAG_LEN;

// OS random bytes. (SFML's bundled mbedTLS is built without an entropy source, so we do not use
// mbedtls_entropy/ctr_drbg; the OS CSPRNG is used directly.)
#ifdef _WIN32
extern "C" long __stdcall BCryptGenRandom(void* hAlgorithm, unsigned char* pbBuffer, unsigned long cbBuffer, unsigned long dwFlags);
#endif
inline int rngFn(void*, unsigned char* out, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, (unsigned long)n, 2 /*BCRYPT_USE_SYSTEM_PREFERRED_RNG*/) == 0 ? 0 : -1;
#else
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = std::fread(out, 1, n, f);
    std::fclose(f);
    return got == n ? 0 : -1;
#endif
}
inline void randomBytes(void* out, size_t n) { rngFn(nullptr, (unsigned char*)out, n); }
inline uint64_t randomToken() { uint64_t t = 0; randomBytes(&t, sizeof(t)); return t ? t : 1; }

// ---- X25519 helpers (mbedtls_ecdh low-level API) ----
struct KeyPair {
    uint8_t priv[32] = {}, pub[32] = {};
    bool ok = false;
};

inline KeyPair generateKeyPair() {
    KeyPair kp;
    mbedtls_ecp_group grp; mbedtls_mpi d; mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_ecp_point_init(&Q);
    size_t olen = 0;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
        mbedtls_ecdh_gen_public(&grp, &d, &Q, rngFn, nullptr) == 0 &&
        mbedtls_mpi_write_binary_le(&d, kp.priv, 32) == 0 &&
        mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, kp.pub, 32) == 0 && olen == 32)
        kp.ok = true;
    mbedtls_ecp_group_free(&grp); mbedtls_mpi_free(&d); mbedtls_ecp_point_free(&Q);
    return kp;
}

// out = X25519(priv, peerPub). Rejects the all-zero (low order) result.
inline bool dh(const uint8_t priv[32], const uint8_t peerPub[32], uint8_t out[32]) {
    mbedtls_ecp_group grp; mbedtls_mpi d, z; mbedtls_ecp_point P;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_mpi_init(&z); mbedtls_ecp_point_init(&P);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
              mbedtls_mpi_read_binary_le(&d, priv, 32) == 0 &&
              mbedtls_ecp_point_read_binary(&grp, &P, peerPub, 32) == 0 &&
              mbedtls_ecdh_compute_shared(&grp, &z, &P, &d, rngFn, nullptr) == 0 &&
              mbedtls_mpi_write_binary_le(&z, out, 32) == 0;
    if (ok) { uint8_t acc = 0; for (int i = 0; i < 32; ++i) acc |= out[i]; ok = acc != 0; }
    mbedtls_ecp_group_free(&grp); mbedtls_mpi_free(&d); mbedtls_mpi_free(&z); mbedtls_ecp_point_free(&P);
    return ok;
}

inline std::string toHex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s; for (size_t i = 0; i < n; ++i) { s.push_back(h[p[i] >> 4]); s.push_back(h[p[i] & 15]); }
    return s;
}
inline bool fromHex(const std::string& s, uint8_t* out, size_t n) {
    if (s.size() < n * 2) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1)); };
    for (size_t i = 0; i < n; ++i) { int a = v(s[2 * i]), b = v(s[2 * i + 1]); if (a < 0 || b < 0) return false; out[i] = (uint8_t)(a * 16 + b); }
    return true;
}

// Human-friendly fingerprint of a public key: first 16 bytes of SHA-256, hex, in groups of 4.
inline std::string fingerprint(const uint8_t pub[32]) {
    uint8_t h[32]; mbedtls_sha256(pub, 32, h, 0);
    std::string hex = toHex(h, 16), out;
    for (size_t i = 0; i < hex.size(); i += 4) { if (i) out.push_back(':'); out += hex.substr(i, 4); }
    return out;
}

// Server long-term identity, persisted in `path` (hex of priv||pub). Created on first use.
inline KeyPair loadOrCreateIdentity(const std::string& path, bool& created) {
    created = false;
    KeyPair kp;
    std::ifstream f(path);
    std::string line;
    if (f && std::getline(f, line) && fromHex(line, kp.priv, 32) && line.size() >= 128 && fromHex(line.substr(64), kp.pub, 32)) {
        kp.ok = true; return kp;
    }
    kp = generateKeyPair();
    if (kp.ok) {
        std::ofstream o(path, std::ios::trunc);
        o << toHex(kp.priv, 32) << toHex(kp.pub, 32) << "\n";
        created = true;
    }
    return kp;
}

struct SessionKeys { uint8_t c2s[32], s2c[32]; };

inline bool deriveKeys(const uint8_t dh1[32], const uint8_t dh2[32], const uint8_t Qc[32], const uint8_t Qs[32],
                       const uint8_t S[32], SessionKeys& out) {
    uint8_t ikm[64]; std::memcpy(ikm, dh1, 32); std::memcpy(ikm + 32, dh2, 32);
    uint8_t tr[96]; std::memcpy(tr, Qc, 32); std::memcpy(tr + 32, Qs, 32); std::memcpy(tr + 64, S, 32);
    uint8_t salt[32]; mbedtls_sha256(tr, 96, salt, 0);
    uint8_t okm[64];
    const char* info = "stt-v1";
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_hkdf(md, salt, 32, ikm, 64, (const unsigned char*)info, std::strlen(info), okm, 64) != 0) return false;
    std::memcpy(out.c2s, okm, 32); std::memcpy(out.s2c, okm + 32, 32);
    return true;
}

// ---- AEAD channel ----
class Channel {
public:
    Channel() { mbedtls_chachapoly_init(&tx_); mbedtls_chachapoly_init(&rx_); }
    ~Channel() { mbedtls_chachapoly_free(&tx_); mbedtls_chachapoly_free(&rx_); }
    Channel(const Channel&) = delete; Channel& operator=(const Channel&) = delete;

    bool on() const { return on_; }
    void start(const uint8_t sendKey[32], const uint8_t recvKey[32]) {
        mbedtls_chachapoly_setkey(&tx_, sendKey); mbedtls_chachapoly_setkey(&rx_, recvKey);
        sendCtr_ = recvCtr_ = 0; on_ = true;
    }
    void reset() { on_ = false; sendCtr_ = recvCtr_ = 0; }

    // plaintext packet -> ciphertext||tag packet
    bool seal(const sf::Packet& in, sf::Packet& out) {
        size_t n = in.getDataSize();
        if (!on_ || n > MAX_PLAIN) return false;
        std::vector<uint8_t> buf(n + TAG_LEN);
        uint8_t nonce[12]; makeNonce(sendCtr_++, nonce);
        if (mbedtls_chachapoly_encrypt_and_tag(&tx_, n, nonce, nullptr, 0, (const unsigned char*)in.getData(), buf.data(), buf.data() + n) != 0)
            return false;
        out.clear(); out.append(buf.data(), buf.size());
        return true;
    }
    // ciphertext||tag packet -> plaintext packet (false = tampered / replayed / reordered / oversize)
    bool open(const sf::Packet& in, sf::Packet& out) {
        size_t n = in.getDataSize();
        if (!on_ || n < TAG_LEN || n > MAX_WIRE) return false;
        size_t pl = n - TAG_LEN;
        std::vector<uint8_t> buf(pl ? pl : 1);
        const uint8_t* d = (const uint8_t*)in.getData();
        uint8_t nonce[12]; makeNonce(recvCtr_, nonce);
        if (mbedtls_chachapoly_auth_decrypt(&rx_, pl, nonce, nullptr, 0, d + pl, d, buf.data()) != 0) return false;
        ++recvCtr_;
        out.clear(); if (pl) out.append(buf.data(), pl);
        return true;
    }
private:
    static void makeNonce(uint64_t c, uint8_t n[12]) {
        std::memset(n, 0, 12);
        for (int i = 0; i < 8; ++i) n[4 + i] = (uint8_t)(c >> (8 * i));
    }
    mbedtls_chachapoly_context tx_, rx_;
    uint64_t sendCtr_ = 0, recvCtr_ = 0;
    bool on_ = false;
};

// Blocking-ish send for non-blocking sockets (retries Partial / NotReady for a short while).
inline sf::Socket::Status sendPacket(sf::TcpSocket& s, sf::Packet& p) {
    sf::Socket::Status st = sf::Socket::Status::Done;
    for (int i = 0; i < 400; ++i) {
        st = s.send(p);
        if (st != sf::Socket::Status::Partial && st != sf::Socket::Status::NotReady) break;
        sf::sleep(sf::milliseconds(1));
    }
    return st;
}

} // namespace sec

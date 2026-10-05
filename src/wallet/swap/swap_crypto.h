#pragma once
// Internal helpers shared by the swap runner and the watchtower: hex, key
// derivation, BIP340 / ECDSA signing and verification. Not a public API.

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "wallet/swap/htlc.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dinero::swap::detail {

[[noreturn]] inline void Fail(const std::string& why) { throw std::invalid_argument("swap: " + why); }

inline std::string ToHex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}

inline std::vector<uint8_t> FromHex(const std::string& h) {
    if (h.size() % 2) Fail("odd-length hex");
    std::vector<uint8_t> out;
    for (size_t i = 0; i < h.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = nib(h[i]), lo = nib(h[i + 1]);
        if (hi < 0 || lo < 0) Fail("bad hex");
        out.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return out;
}

inline secp256k1_keypair Keypair(const Bytes32& secret_key) {
    secp256k1_keypair kp;
    if (secp256k1_keypair_create(crypto::GetSecp256k1ContextSignVerify(), &kp, secret_key.data()) != 1) {
        Fail("invalid secret key");
    }
    return kp;
}

inline Bytes32 XOnlyOf(const Bytes32& secret_key) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    auto kp = Keypair(secret_key);
    secp256k1_xonly_pubkey x;
    secp256k1_keypair_xonly_pub(secp, &x, nullptr, &kp);
    Bytes32 out{};
    secp256k1_xonly_pubkey_serialize(secp, out.data(), &x);
    return out;
}

inline std::array<uint8_t, 33> CompressedOf(const Bytes32& secret_key) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_pubkey pk;
    if (secp256k1_ec_pubkey_create(secp, &pk, secret_key.data()) != 1) Fail("invalid secret key");
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(secp, out.data(), &len, &pk, SECP256K1_EC_COMPRESSED);
    return out;
}

inline std::array<uint8_t, 64> SchnorrSign(const Bytes32& secret_key, const Bytes32& msg) {
    auto kp = Keypair(secret_key);
    std::array<uint8_t, 64> sig{};
    const std::array<uint8_t, 32> aux{};
    if (secp256k1_schnorrsig_sign32(crypto::GetSecp256k1ContextSignVerify(), sig.data(), msg.data(), &kp,
                                    aux.data()) != 1) {
        Fail("schnorr signing failed");
    }
    return sig;
}

inline std::vector<uint8_t> EcdsaSignAll(const Bytes32& secret_key, const Bytes32& msg) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_ecdsa_signature sig;
    if (secp256k1_ecdsa_sign(secp, &sig, msg.data(), secret_key.data(), nullptr, nullptr) != 1) {  // low-S
        Fail("ecdsa signing failed");
    }
    std::vector<uint8_t> der(72);
    size_t len = der.size();
    secp256k1_ecdsa_signature_serialize_der(secp, der.data(), &len, &sig);
    der.resize(len);
    der.push_back(0x01);  // SIGHASH_ALL
    return der;
}

inline bool SchnorrVerify(const Bytes32& xonly, const Bytes32& msg, const std::array<uint8_t, 64>& sig) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_xonly_pubkey pk;
    if (secp256k1_xonly_pubkey_parse(secp, &pk, xonly.data()) != 1) return false;
    return secp256k1_schnorrsig_verify(secp, sig.data(), msg.data(), msg.size(), &pk) == 1;
}

// `sig` is DER + the SIGHASH_ALL byte; only low-S signatures are accepted.
inline bool EcdsaVerifyAll(const std::array<uint8_t, 33>& pubkey, const Bytes32& msg, const std::vector<uint8_t>& sig) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    if (sig.size() < 2 || sig.back() != 0x01) return false;
    secp256k1_ecdsa_signature s;
    if (secp256k1_ecdsa_signature_parse_der(secp, &s, sig.data(), sig.size() - 1) != 1) return false;
    secp256k1_ecdsa_signature normalized;
    if (secp256k1_ecdsa_signature_normalize(secp, &normalized, &s) == 1) return false;  // high-S
    secp256k1_pubkey pk;
    if (secp256k1_ec_pubkey_parse(secp, &pk, pubkey.data(), pubkey.size()) != 1) return false;
    return secp256k1_ecdsa_verify(secp, &s, msg.data(), &pk) == 1;
}

inline Bytes32 HmacSha256(const Bytes32& key, const uint8_t* msg, size_t len) {
    std::array<uint8_t, 64> ipad{}, opad{};
    for (size_t i = 0; i < 64; ++i) {
        const uint8_t k = i < key.size() ? key[i] : 0;  // a 32-byte key fits the 64-byte block
        ipad[i] = k ^ 0x36;
        opad[i] = k ^ 0x5c;
    }
    Bytes32 inner{}, out{};
    crypto::CSHA256().Write(ipad.data(), ipad.size()).Write(msg, len).Finalize(inner.data());
    crypto::CSHA256().Write(opad.data(), opad.size()).Write(inner.data(), inner.size()).Finalize(out.data());
    return out;
}

inline Bytes32 HmacSha256(const Bytes32& key, const std::string& msg) {
    return HmacSha256(key, reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
}

// temp + fsync + rename (mode 0600): a crash leaves the old or the new file.
#ifdef _WIN32
// Windows: write + _commit, then MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)
// (std::rename refuses to replace an existing file there). No directory sync.
inline void WriteFileAtomically(const std::string& path, const std::string& text) {
    const std::filesystem::path target(path), tmp(path + ".tmp");
    const int fd = ::_wopen(tmp.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) throw std::runtime_error("swap: cannot open " + path + ".tmp");
    size_t off = 0;
    while (off < text.size()) {
        const int n = ::_write(fd, text.data() + off, static_cast<unsigned>(text.size() - off));
        if (n <= 0) { ::_close(fd); throw std::runtime_error("swap: write failed: " + path); }
        off += static_cast<size_t>(n);
    }
    if (::_commit(fd) != 0) { ::_close(fd); throw std::runtime_error("swap: fsync failed: " + path); }
    ::_close(fd);
    if (!::MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("swap: rename failed: " + path);
    }
}
#else
// fsync on macOS only reaches the drive's cache; F_FULLFSYNC flushes it.
inline int SyncToDisk(int fd) {
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) return 0;  // falls back where the filesystem lacks it
#endif
    return ::fsync(fd);
}

inline void WriteFileAtomically(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("swap: cannot open " + tmp);
    size_t off = 0;
    while (off < text.size()) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n <= 0) { ::close(fd); throw std::runtime_error("swap: write failed: " + path); }
        off += static_cast<size_t>(n);
    }
    if (SyncToDisk(fd) != 0) { ::close(fd); throw std::runtime_error("swap: fsync failed: " + path); }
    ::close(fd);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) throw std::runtime_error("swap: rename failed: " + path);
    // Make the rename itself durable: without syncing the directory, a power
    // loss can bring back the old file (e.g. a state from before funding).
    const auto slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    const int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd >= 0) {
        SyncToDisk(dfd);
        ::close(dfd);
    }
}
#endif

}  // namespace dinero::swap::detail

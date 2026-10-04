#pragma once
// Internal helpers shared by the swap runner and the watchtower: hex, key
// derivation, BIP340 / ECDSA signing and verification. Not a public API.

#include "crypto/evp_secp256k1.h"
#include "wallet/swap/htlc.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <stdexcept>
#include <string>
#include <vector>

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

}  // namespace dinero::swap::detail

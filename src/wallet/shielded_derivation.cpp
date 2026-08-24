// Phase 5 Wave 1+2 — Shielded key derivation. See header for spec context.

#include "wallet/shielded_derivation.h"

#include "wallet/bip32_deriver.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_generator.h>

#include "../external/bech32/bech32.hpp"

#include <cstring>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <vector>

namespace dinero::wallet::shielded {

using consensus::shielded::PoseidonHash2;

Hash DstToHash(const char* dst) {
    Hash h{};
    if (dst == nullptr) return h;
    const std::size_t len = std::strlen(dst);
    if (len > h.size()) {
        throw std::runtime_error("DST too long for 32-byte hash");
    }
    std::memcpy(h.data(), dst, len);
    return h;
}

Hash ShieldedPRF(const Hash& key, const char* dst) {
    return PoseidonHash2(key, DstToHash(dst));
}

namespace {

/// secp256k1 base context for verification + keypair operations. Lazily
/// initialised; safe across threads after first call (libsecp creates an
/// immutable verify-only context once).
secp256k1_context* GetCtx() {
    static secp256k1_context* ctx = []() {
        return secp256k1_context_create(SECP256K1_CONTEXT_VERIFY |
                                        SECP256K1_CONTEXT_SIGN);
    }();
    return ctx;
}

/// Compute (effective_scalar, x_only_pubkey) for a 32-byte input, applying
/// BIP340 even-y normalisation. The "effective scalar" is the scalar that,
/// when multiplied by G, yields the returned x-only pubkey (i.e., possibly
/// negated from the input so the resulting y is even).
///
/// IMPORTANT — libsecp's keypair API does NOT normalise the scalar:
///   - `secp256k1_keypair_save` stores the input scalar UNTOUCHED at
///     `keypair->data[0..32]` (see modules/extrakeys/main_impl.h:157).
///   - `secp256k1_keypair_sec` reads that buffer back as-is.
///   - Only the x-only pubkey path (`keypair_xonly_pub`) reports the
///     parity flag and exposes the BIP340-normalised x-coordinate.
///
/// Earlier revisions assumed keypair_create did the negation internally;
/// it does not. We therefore read `pk_parity` from `keypair_xonly_pub` and
/// negate the secret in place when parity == 1, so the returned scalar is
/// the one that pairs with the returned x-only pubkey under BIP340.
struct ScalarPubkey {
    Hash scalar{};
    Hash x_only{};
};

ScalarPubkey NormalizeScalarToEvenY(const Hash& candidate) {
    secp256k1_context* ctx = GetCtx();
    if (ctx == nullptr) {
        throw std::runtime_error("secp256k1 context creation failed");
    }
    if (secp256k1_ec_seckey_verify(ctx, candidate.data()) != 1) {
        // Either zero or out of range. PRF output has negligible chance
        // of hitting zero exactly; treat as fatal seed-generation error.
        throw std::runtime_error("shielded PRF produced invalid scalar (zero or >= q)");
    }
    secp256k1_keypair kp{};
    if (secp256k1_keypair_create(ctx, &kp, candidate.data()) != 1) {
        throw std::runtime_error("secp256k1_keypair_create failed");
    }
    ScalarPubkey out;
    if (secp256k1_keypair_sec(ctx, out.scalar.data(), &kp) != 1) {
        OPENSSL_cleanse(&kp, sizeof(kp));
        throw std::runtime_error("secp256k1_keypair_sec failed");
    }
    secp256k1_xonly_pubkey xonly{};
    int pk_parity = 0;
    if (secp256k1_keypair_xonly_pub(ctx, &xonly, &pk_parity, &kp) != 1) {
        OPENSSL_cleanse(&kp, sizeof(kp));
        throw std::runtime_error("secp256k1_keypair_xonly_pub failed");
    }
    if (secp256k1_xonly_pubkey_serialize(ctx, out.x_only.data(), &xonly) != 1) {
        OPENSSL_cleanse(&kp, sizeof(kp));
        throw std::runtime_error("secp256k1_xonly_pubkey_serialize failed");
    }
    // BIP340 even-y normalisation: if the keypair's pubkey had odd-y and
    // the x-only path negated to even-y, negate the secret too so that
    // `out.scalar * G == out.x_only` (with even-y).
    if (pk_parity == 1) {
        if (secp256k1_ec_seckey_negate(ctx, out.scalar.data()) != 1) {
            OPENSSL_cleanse(&kp, sizeof(kp));
            OPENSSL_cleanse(out.scalar.data(), out.scalar.size());
            throw std::runtime_error("secp256k1_ec_seckey_negate failed during even-y normalisation");
        }
    }
    OPENSSL_cleanse(&kp, sizeof(kp));
    return out;
}

}  // namespace

ShieldedAccountKeys DeriveShieldedAccount(const uint8_t* seed,
                                          std::size_t seed_len,
                                          uint32_t account) {
    if (seed == nullptr) {
        throw std::runtime_error("DeriveShieldedAccount: null seed");
    }
    if (seed_len != 64) {
        // BIP32 master seed length is 64 bytes (BIP39 PBKDF2 output).
        throw std::runtime_error("DeriveShieldedAccount: seed_len must be 64");
    }

    // ── BIP32 walk: m / 99' / 1448' / account'.
    BIP32Deriver deriver(seed, seed_len);
    deriver.deriveHardened(kPurposeShielded);
    deriver.deriveHardened(kCoinTypeDinero);
    deriver.deriveHardened(account);
    if (!deriver.isValid()) {
        throw std::runtime_error("DeriveShieldedAccount: BIP32 path produced invalid key");
    }

    auto sk_arr = deriver.getPrivateKey();
    Hash sk{};
    std::memcpy(sk.data(), sk_arr.data(), sk_arr.size());
    OPENSSL_cleanse(sk_arr.data(), sk_arr.size());

    // ── Sapling sub-derivation (§4.3 – §4.6).
    Hash ask_raw = ShieldedPRF(sk, kDstAsk);
    Hash nsk_raw = ShieldedPRF(sk, kDstNsk);
    Hash ovk     = ShieldedPRF(sk, kDstOvk);
    Hash dk      = ShieldedPRF(sk, kDstDk);

    auto ak_norm = NormalizeScalarToEvenY(ask_raw);
    auto nk_norm = NormalizeScalarToEvenY(nsk_raw);

    // ── ivk = Poseidon2(ak, nk). The Poseidon evaluator takes raw bytes
    // ── and treats them as big-endian scalars; the result is also a
    // ── 32-byte big-endian scalar representative. No explicit mod q
    // ── reduction needed — Poseidon's permutation outputs in-field.
    Hash ivk = PoseidonHash2(ak_norm.x_only, nk_norm.x_only);

    ShieldedAccountKeys out;
    out.sk  = sk;
    out.ask = ak_norm.scalar;
    out.nsk = nk_norm.scalar;
    out.ovk = ovk;
    out.dk  = dk;
    out.ak  = ak_norm.x_only;
    out.nk  = nk_norm.x_only;
    out.ivk = ivk;

    // The raw PRF outputs may differ from the normalised scalars (when
    // negation was applied). Caller never needs the pre-normalisation
    // scalars; cleanse them so they don't linger.
    OPENSSL_cleanse(ask_raw.data(), ask_raw.size());
    OPENSSL_cleanse(nsk_raw.data(), nsk_raw.size());
    OPENSSL_cleanse(sk.data(), sk.size());
    return out;
}

// ── Phase 5 Wave 2: diversifier + diversified address ───────────────

Diversifier ChaCha20Diversifier(const Hash& dk, uint64_t j) {
    // OpenSSL's EVP_chacha20() expects a 16-byte IV laid out as
    //   [0..4)  = block counter (little-endian uint32)
    //   [4..16) = nonce (12 bytes; IETF ChaCha20-style)
    // Passing only a 12-byte buffer caused OpenSSL to read 4 bytes
    // past the end of the std::array — that uninitialized stack
    // memory was platform/compiler/call-site deterministic but
    // diverged between (e.g.) Mac arm64 and Linux x86_64, producing
    // different keystreams for the same (dk, j). That platform-
    // dependent diversifier propagated through HashToPoint, DerivePkD,
    // and EncodeShieldedAddress, giving different shielded addresses
    // on different platforms for the same seed. The bug was caught
    // when the v2 tests workflow first ran the full test target list
    // on Linux CI (PR #54 era).
    //
    // Construct an explicit 16-byte IV with counter=0 prefix + the
    // intended 12-byte IETF nonce = (j little-endian, 8 bytes) zero-
    // padded to 12. Both platforms now produce identical output.
    std::array<uint8_t, 16> iv{};
    for (int i = 0; i < 8; ++i) {
        iv[4 + i] = static_cast<uint8_t>((j >> (8 * i)) & 0xFF);
    }
    // Encrypt 11 zero bytes → 11 keystream bytes (counter starts at 0).
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    }
    Diversifier out{};
    std::array<uint8_t, 11> zeroes{};
    int written = 0;
    int ok =
        EVP_EncryptInit_ex(ctx, EVP_chacha20(), nullptr, dk.data(), iv.data()) &&
        EVP_EncryptUpdate(ctx, out.data(), &written,
                          zeroes.data(), static_cast<int>(zeroes.size()));
    if (!ok || written != static_cast<int>(zeroes.size())) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("ChaCha20 keystream generation failed");
    }
    EVP_CIPHER_CTX_free(ctx);
    return out;
}

Hash HashToPoint(const Diversifier& d, const char* dst) {
    // seed = SHA-256(dst_padded_32 || d). Matches the V-generator pattern
    // (pedersen_generators.cpp): try-and-increment via libsecp's
    // generator_generate. Same nothing-up-my-sleeve property as SSWU,
    // negligible collision probability across the 88-bit diversifier
    // index space.
    Hash dst_h = DstToHash(dst);
    std::array<uint8_t, 32 + 11> material{};
    std::memcpy(material.data(), dst_h.data(), 32);
    std::memcpy(material.data() + 32, d.data(), 11);
    std::array<uint8_t, 32> seed{};
    SHA256(material.data(), material.size(), seed.data());

    secp256k1_context* ctx = GetCtx();
    secp256k1_generator gen{};
    if (secp256k1_generator_generate(ctx, &gen, seed.data()) != 1) {
        throw std::runtime_error("secp256k1_generator_generate failed for P_d");
    }
    // generator_serialize emits a 33-byte buffer: pedersen prefix
    // (0x08/0x09 — different from SEC1's 0x02/0x03) || x-coord. Pull the
    // 32 x-coord bytes; the prefix's parity flag is irrelevant because
    // we always normalise to even-y (BIP340 convention) below.
    std::array<uint8_t, 33> pedersen_compressed{};
    if (secp256k1_generator_serialize(ctx, pedersen_compressed.data(), &gen) != 1) {
        throw std::runtime_error("secp256k1_generator_serialize failed");
    }
    Hash out{};
    std::memcpy(out.data(), pedersen_compressed.data() + 1, 32);
    // Sanity-check: x is on the curve (parse as 0x02 even-y compressed).
    std::array<uint8_t, 33> sec1{};
    sec1[0] = 0x02;
    std::memcpy(sec1.data() + 1, out.data(), 32);
    secp256k1_pubkey pk{};
    if (secp256k1_ec_pubkey_parse(ctx, &pk, sec1.data(), sec1.size()) != 1) {
        throw std::runtime_error("P_d x-coord not on curve (should be impossible)");
    }
    return out;
}

Hash DerivePkD(const Hash& ivk, const Hash& p_d_xonly) {
    secp256k1_context* ctx = GetCtx();

    // Parse P_d as an even-y full pubkey via the x-only path.
    secp256k1_xonly_pubkey xonly{};
    if (secp256k1_xonly_pubkey_parse(ctx, &xonly, p_d_xonly.data()) != 1) {
        throw std::runtime_error("secp256k1_xonly_pubkey_parse(P_d) failed");
    }
    // Convert xonly → full pubkey by serializing then parsing as
    // 0x02-prefix compressed (even-y).
    std::array<uint8_t, 33> compressed{};
    compressed[0] = 0x02;
    std::memcpy(compressed.data() + 1, p_d_xonly.data(), 32);
    secp256k1_pubkey pubkey{};
    if (secp256k1_ec_pubkey_parse(ctx, &pubkey, compressed.data(),
                                  compressed.size()) != 1) {
        throw std::runtime_error("secp256k1_ec_pubkey_parse(P_d compressed) failed");
    }

    // ivk is a 32-byte big-endian scalar; libsecp's tweak_mul takes BE.
    // ivk could in principle be 0 or >= q (Poseidon output is in-field
    // but the field element's BE byte representation may exceed q if
    // misaligned). Use seckey_verify to bail on edge cases.
    if (secp256k1_ec_seckey_verify(ctx, ivk.data()) != 1) {
        throw std::runtime_error("ivk is zero or out of secp256k1 range");
    }
    if (secp256k1_ec_pubkey_tweak_mul(ctx, &pubkey, ivk.data()) != 1) {
        throw std::runtime_error("secp256k1_ec_pubkey_tweak_mul(ivk·P_d) failed");
    }

    // Serialize even-y x-only.
    secp256k1_xonly_pubkey result_xonly{};
    if (secp256k1_xonly_pubkey_from_pubkey(ctx, &result_xonly, nullptr,
                                           &pubkey) != 1) {
        throw std::runtime_error("xonly_pubkey_from_pubkey(pk_d) failed");
    }
    Hash out{};
    if (secp256k1_xonly_pubkey_serialize(ctx, out.data(), &result_xonly) != 1) {
        throw std::runtime_error("xonly_pubkey_serialize(pk_d) failed");
    }
    return out;
}

AddressPayload BuildAddressPayload(const Diversifier& d, const Hash& pk_d) {
    AddressPayload payload{};
    std::memcpy(payload.data(), d.data(), 11);
    std::memcpy(payload.data() + 11, pk_d.data(), 32);
    return payload;
}

std::string EncodeShieldedAddress(const AddressPayload& payload,
                                  const std::string& hrp) {
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    std::vector<uint8_t> data5;
    if (!bech32::convertbits(data5, bytes, 8, 5, /*pad=*/true)) {
        throw std::runtime_error("bech32 convertbits 8→5 failed");
    }
    return bech32::EncodeRaw(hrp, data5, bech32::Encoding::BECH32M);
}

DiversifiedAddress DeriveDiversifiedAddress(const ShieldedAccountKeys& keys,
                                            uint64_t j,
                                            const std::string& hrp) {
    DiversifiedAddress out;
    out.d = ChaCha20Diversifier(keys.dk, j);
    Hash p_d = HashToPoint(out.d, kDstDiv);
    out.pk_d = DerivePkD(keys.ivk, p_d);
    out.payload = BuildAddressPayload(out.d, out.pk_d);
    out.address = EncodeShieldedAddress(out.payload, hrp);
    return out;
}

// ── Phase 5 Wave 3: address decode + encrypted-note ECDH/AEAD ───────

namespace {

bool IsValidShieldedHrp(const std::string& hrp) {
    return hrp == kHrpMainnet || hrp == kHrpTestnet || hrp == kHrpRegtest;
}

/// HKDF-SHA256 expand for a single 32-byte block (sufficient for our
/// 32-byte AEAD key). Returns T(1) = HMAC-SHA256(prk, info || 0x01).
std::array<uint8_t, 32> HkdfExtractAndExpand(const uint8_t* salt,
                                             std::size_t salt_len,
                                             const uint8_t* ikm,
                                             std::size_t ikm_len,
                                             const std::string& info) {
    std::array<uint8_t, 32> prk{};
    unsigned int prk_len = 0;
    if (HMAC(EVP_sha256(), salt, static_cast<int>(salt_len), ikm, ikm_len,
             prk.data(), &prk_len) == nullptr || prk_len != 32) {
        throw std::runtime_error("HKDF-Extract HMAC failed");
    }
    std::vector<uint8_t> info_buf(info.begin(), info.end());
    info_buf.push_back(0x01);  // T(1) counter
    std::array<uint8_t, 32> okm{};
    unsigned int okm_len = 0;
    if (HMAC(EVP_sha256(), prk.data(), 32, info_buf.data(), info_buf.size(),
             okm.data(), &okm_len) == nullptr || okm_len != 32) {
        OPENSSL_cleanse(prk.data(), prk.size());
        throw std::runtime_error("HKDF-Expand HMAC failed");
    }
    OPENSSL_cleanse(prk.data(), prk.size());
    return okm;
}

}  // namespace

/// shared = (scalar · pk_xonly).x — even-y normalised on output.
Hash EcdhShared(const Hash& scalar_be, const Hash& pk_xonly) {
    secp256k1_context* ctx = GetCtx();
    std::array<uint8_t, 33> sec1{};
    sec1[0] = 0x02;
    std::memcpy(sec1.data() + 1, pk_xonly.data(), 32);
    secp256k1_pubkey pubkey{};
    if (secp256k1_ec_pubkey_parse(ctx, &pubkey, sec1.data(), sec1.size()) != 1) {
        throw std::runtime_error("ECDH: pk x-coord not on curve");
    }
    if (secp256k1_ec_seckey_verify(ctx, scalar_be.data()) != 1) {
        throw std::runtime_error("ECDH: scalar invalid");
    }
    if (secp256k1_ec_pubkey_tweak_mul(ctx, &pubkey, scalar_be.data()) != 1) {
        throw std::runtime_error("ECDH: pubkey_tweak_mul failed");
    }
    secp256k1_xonly_pubkey xonly{};
    if (secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly, nullptr, &pubkey) != 1) {
        throw std::runtime_error("ECDH: xonly_from_pubkey failed");
    }
    Hash out{};
    if (secp256k1_xonly_pubkey_serialize(ctx, out.data(), &xonly) != 1) {
        throw std::runtime_error("ECDH: xonly_serialize failed");
    }
    return out;
}


DecodedShieldedAddress DecodeShieldedAddress(const std::string& addr) {
    auto raw = bech32::DecodeRaw(addr);
    if (!raw) {
        throw std::runtime_error("shielded address: bech32m decode failed");
    }
    if (raw->encoding != bech32::Encoding::BECH32M) {
        throw std::runtime_error("shielded address: must be bech32m, not bech32");
    }
    if (!IsValidShieldedHrp(raw->hrp)) {
        throw std::runtime_error("shielded address: HRP must be dins/tdins/rdins, got '" +
                                 raw->hrp + "'");
    }
    std::vector<uint8_t> bytes;
    if (!bech32::convertbits(bytes, raw->data, 5, 8, /*pad=*/false)) {
        throw std::runtime_error("shielded address: 5→8 convertbits failed");
    }
    if (bytes.size() != 43) {
        throw std::runtime_error("shielded address: payload must be 43 bytes, got " +
                                 std::to_string(bytes.size()));
    }
    DecodedShieldedAddress out;
    out.hrp = raw->hrp;
    std::memcpy(out.d.data(), bytes.data(), 11);
    std::memcpy(out.pk_d.data(), bytes.data() + 11, 32);
    std::memcpy(out.payload.data(), bytes.data(), 43);

    // Validate pk_d is on the curve (force even-y parse).
    secp256k1_context* ctx = GetCtx();
    std::array<uint8_t, 33> sec1{};
    sec1[0] = 0x02;
    std::memcpy(sec1.data() + 1, out.pk_d.data(), 32);
    secp256k1_pubkey pubkey{};
    if (secp256k1_ec_pubkey_parse(ctx, &pubkey, sec1.data(), sec1.size()) != 1) {
        throw std::runtime_error("shielded address: pk_d x-coord not on curve");
    }
    return out;
}

std::array<uint8_t, 563> NotePlaintext::Serialize() const {
    std::array<uint8_t, 563> out{};
    std::memcpy(out.data(), d.data(), 11);
    for (int i = 0; i < 8; ++i) {
        out[11 + i] = static_cast<uint8_t>((value_una >> (8 * i)) & 0xFF);
    }
    std::memcpy(out.data() + 19, rcm.data(), 32);
    std::memcpy(out.data() + 51, memo.data(), 512);
    return out;
}

NotePlaintext NotePlaintext::Deserialize(const std::array<uint8_t, 563>& bytes) {
    NotePlaintext out;
    std::memcpy(out.d.data(), bytes.data(), 11);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(bytes[11 + i]) << (8 * i);
    }
    out.value_una = v;
    std::memcpy(out.rcm.data(), bytes.data() + 19, 32);
    std::memcpy(out.memo.data(), bytes.data() + 51, 512);
    return out;
}

namespace {

constexpr const char* kAeadInfo = "DIN/v7/shielded/note";

/// AEAD encrypt with ChaCha20-Poly1305. Nonce is 12 zero bytes (epk
/// provides freshness + uniqueness). aad = epk. Output is ct (563) ||
/// tag (16) = 579 bytes; caller prepends epk.
std::array<uint8_t, 579> AeadEncrypt(const std::array<uint8_t, 32>& key,
                                     const std::array<uint8_t, 32>& aad_epk,
                                     const std::array<uint8_t, 563>& plaintext) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    }
    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 579> out{};
    int written = 0;
    int aad_written = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
        EVP_EncryptUpdate(ctx, nullptr, &aad_written, aad_epk.data(), 32) == 1 &&
        EVP_EncryptUpdate(ctx, out.data(), &written,
                          plaintext.data(), static_cast<int>(plaintext.size())) == 1;
    int final_written = 0;
    ok = ok && EVP_EncryptFinal_ex(ctx, out.data() + written, &final_written) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16,
                                   out.data() + plaintext.size()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok || written + final_written != static_cast<int>(plaintext.size())) {
        throw std::runtime_error("AEAD encrypt failed");
    }
    return out;
}

bool AeadDecrypt(const std::array<uint8_t, 32>& key,
                 const std::array<uint8_t, 32>& aad_epk,
                 const uint8_t* ct_and_tag,
                 std::array<uint8_t, 563>& out_plaintext) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) return false;
    std::array<uint8_t, 12> nonce{};
    int written = 0;
    int aad_written = 0;
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
        EVP_DecryptUpdate(ctx, nullptr, &aad_written, aad_epk.data(), 32) == 1 &&
        EVP_DecryptUpdate(ctx, out_plaintext.data(), &written, ct_and_tag, 563) == 1;
    // Set expected tag, then finalize. EVP_DecryptFinal returns 0 on tag mismatch.
    ok = ok && EVP_CIPHER_CTX_ctrl(
                   ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                   const_cast<uint8_t*>(ct_and_tag) + 563) == 1;
    int final_written = 0;
    int final_rc = 0;
    if (ok) {
        final_rc = EVP_DecryptFinal_ex(ctx, out_plaintext.data() + written, &final_written);
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok && final_rc == 1 &&
           (written + final_written) == 563;
}

}  // namespace

EncryptedNote EncryptNoteForRecipient(const Diversifier& recipient_d,
                                      const Hash& pk_d_xonly,
                                      const NotePlaintext& note,
                                      const Hash* esk_override) {
    secp256k1_context* ctx = GetCtx();

    // Derive esk: random or test-supplied.
    Hash esk{};
    if (esk_override != nullptr) {
        esk = *esk_override;
        if (secp256k1_ec_seckey_verify(ctx, esk.data()) != 1) {
            throw std::runtime_error("encrypt: esk_override invalid");
        }
    } else {
        do {
            if (RAND_bytes(esk.data(), static_cast<int>(esk.size())) != 1) {
                throw std::runtime_error("encrypt: RAND_bytes failed");
            }
        } while (secp256k1_ec_seckey_verify(ctx, esk.data()) != 1);
    }

    // Sapling-correct ECDH: epk = esk · P_d (NOT esk · G), where P_d =
    // HashToPoint(d). receiver computes shared = ivk · epk = ivk · esk
    // · P_d = esk · (ivk · P_d) = esk · pk_d, matching the sender.
    Hash p_d_xonly = HashToPoint(recipient_d, kDstDiv);
    // epk = esk · P_d, then BIP340 even-y normalise.
    std::array<uint8_t, 33> p_d_sec1{};
    p_d_sec1[0] = 0x02;
    std::memcpy(p_d_sec1.data() + 1, p_d_xonly.data(), 32);
    secp256k1_pubkey p_d_pubkey{};
    if (secp256k1_ec_pubkey_parse(ctx, &p_d_pubkey, p_d_sec1.data(),
                                  p_d_sec1.size()) != 1) {
        OPENSSL_cleanse(esk.data(), esk.size());
        throw std::runtime_error("encrypt: P_d not on curve");
    }
    if (secp256k1_ec_pubkey_tweak_mul(ctx, &p_d_pubkey, esk.data()) != 1) {
        OPENSSL_cleanse(esk.data(), esk.size());
        throw std::runtime_error("encrypt: esk·P_d tweak_mul failed");
    }
    secp256k1_xonly_pubkey epk_xonly{};
    int parity_swapped = 0;  // 0 if even-y, 1 if odd-y was negated
    if (secp256k1_xonly_pubkey_from_pubkey(ctx, &epk_xonly, &parity_swapped,
                                           &p_d_pubkey) != 1) {
        OPENSSL_cleanse(esk.data(), esk.size());
        throw std::runtime_error("encrypt: xonly_from_pubkey(epk) failed");
    }
    Hash epk{};
    if (secp256k1_xonly_pubkey_serialize(ctx, epk.data(), &epk_xonly) != 1) {
        OPENSSL_cleanse(esk.data(), esk.size());
        throw std::runtime_error("encrypt: xonly_serialize(epk) failed");
    }
    // If parity was swapped (epk's pubkey had odd-y so we negated to
    // even-y), negate esk to match — receiver's `ivk·epk_evenY` will
    // then equal `esk_negated · pk_d`. This is the same BIP340 trick
    // used in keypair_create.
    Hash esk_norm = esk;
    if (parity_swapped) {
        if (secp256k1_ec_seckey_negate(ctx, esk_norm.data()) != 1) {
            OPENSSL_cleanse(esk.data(), esk.size());
            OPENSSL_cleanse(esk_norm.data(), esk_norm.size());
            throw std::runtime_error("encrypt: seckey_negate failed");
        }
    }
    OPENSSL_cleanse(esk.data(), esk.size());

    Hash shared = EcdhShared(esk_norm, pk_d_xonly);
    OPENSSL_cleanse(esk_norm.data(), esk_norm.size());

    auto key = HkdfExtractAndExpand(epk.data(), 32, shared.data(), 32, kAeadInfo);
    OPENSSL_cleanse(shared.data(), shared.size());

    auto plaintext = note.Serialize();
    auto ct_and_tag = AeadEncrypt(key, epk, plaintext);
    OPENSSL_cleanse(key.data(), key.size());

    EncryptedNote out{};
    std::memcpy(out.data(), epk.data(), 32);
    std::memcpy(out.data() + 32, ct_and_tag.data(), ct_and_tag.size());
    return out;
}

Hash DeriveNoteSpendKey(const Hash& rcm) {
    return PoseidonHash2(rcm, DstToHash(kDstNoteSpendKey));
}

DiversifiedSpendKey DeriveDiversifiedSpendKey(const Hash& ivk,
                                              const Diversifier& d) {
    // Pack the 11-byte diversifier into a 32-byte field element the same way
    // the note commitment already does (`d_packed`), so `d` means the same
    // thing in both places.
    Hash d_packed{};
    std::memcpy(d_packed.data(), d.data(), d.size());

    // Hash to a SCALAR (not to a point): this is what makes ownership a
    // fixed-base multiplication instead of a variable-base one.
    const Hash s_raw = PoseidonHash2(ivk, d_packed);

    // Reuse the account-key normalisation so `pk_d` is the BIP340 even-y
    // representative and `s` is the matching (possibly negated) scalar —
    // identical treatment to ak = ask·G and nk = nsk·G.
    const auto norm = NormalizeScalarToEvenY(s_raw);

    DiversifiedSpendKey out;
    out.s    = norm.scalar;
    out.pk_d = norm.x_only;
    return out;
}

std::optional<NotePlaintext> TryDecryptNoteForViewer(
    const Hash& ivk, const EncryptedNote& encrypted) {
    Hash epk{};
    std::memcpy(epk.data(), encrypted.data(), 32);

    Hash shared{};
    try {
        shared = EcdhShared(ivk, epk);
    } catch (const std::exception&) {
        return std::nullopt;  // off-curve epk or bad ivk → not for us
    }
    auto key = HkdfExtractAndExpand(epk.data(), 32, shared.data(), 32, kAeadInfo);
    OPENSSL_cleanse(shared.data(), shared.size());

    std::array<uint8_t, 563> plaintext{};
    bool ok = AeadDecrypt(key, epk, encrypted.data() + 32, plaintext);
    OPENSSL_cleanse(key.data(), key.size());
    if (!ok) {
        OPENSSL_cleanse(plaintext.data(), plaintext.size());
        return std::nullopt;
    }
    NotePlaintext note = NotePlaintext::Deserialize(plaintext);
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    return note;
}

}  // namespace dinero::wallet::shielded

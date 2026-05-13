#pragma once

#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <openssl/rand.h>
#include <secp256k1.h>

namespace dinero::crypto {

// Non-aborting illegal_argument callback for libsecp256k1.
//
// libsecp256k1's default callback prints to stderr and calls abort(). On
// MSVC the abort is reported as STATUS_STACK_BUFFER_OVERRUN (0xc0000409)
// which terminates the process. For a consensus daemon, reaching this
// path means a caller passed inputs that violate libsecp256k1's API
// preconditions — by far the most common cause is malformed data on the
// wire (an adversary can send arbitrary bytes to the P2P / RPC layers,
// which can flow into signature verification). Crashing on adversarial
// input is a worse outcome than logging-and-rejecting: libsecp256k1's
// affected routine will already return failure to the caller (which
// causes the script/block/tx to be rejected), so the safety property
// holds. We just don't want the process to die.
//
// Used by the canonical contexts created here (see InitSecpContext) and
// by every other secp256k1_context_create() in src/consensus/ that
// bypasses these helpers (validation_worker_pool.cpp, script_validation.cpp,
// transaction_validator.cpp).
inline void DineroSecpIllegalCallback(const char* message, void* /*data*/) {
    std::fprintf(stderr, "[libsecp256k1] illegal argument: %s\n",
                 message ? message : "(no message)");
    // Do not abort. The verify/sign function will return failure to the
    // caller in the same call; the caller will treat that as a normal
    // signature/operation rejection.
}

namespace detail {

struct SecpContextSlot {
    std::once_flag once;
    secp256k1_context* ctx = nullptr;
};

inline secp256k1_context* InitSecpContext(SecpContextSlot& slot, unsigned flags) {
    std::call_once(slot.once, [&] {
        slot.ctx = secp256k1_context_create(flags);
        if (!slot.ctx) {
            std::fprintf(stderr, "secp256k1: failed to create context\n");
            std::abort();
        }

        // Register the non-aborting illegal_argument callback before any
        // verify/sign call can hit the default abort path.
        secp256k1_context_set_illegal_callback(slot.ctx,
                                               DineroSecpIllegalCallback,
                                               nullptr);

        if (flags != SECP256K1_CONTEXT_NONE) {
            unsigned char seed[32];
            if (RAND_bytes(seed, sizeof(seed)) != 1) {
                std::fprintf(stderr, "secp256k1: failed to generate random seed\n");
                std::abort();
            }
            if (!secp256k1_context_randomize(slot.ctx, seed)) {
                std::fprintf(stderr, "secp256k1: failed to randomize context\n");
                std::abort();
            }
        }
    });
    return slot.ctx;
}

inline SecpContextSlot& SignVerifySlot() {
    static SecpContextSlot slot;
    return slot;
}

inline SecpContextSlot& SignSlot() {
    static SecpContextSlot slot;
    return slot;
}

inline SecpContextSlot& VerifySlot() {
    static SecpContextSlot slot;
    return slot;
}

inline SecpContextSlot& NoneSlot() {
    static SecpContextSlot slot;
    return slot;
}

} // namespace detail

secp256k1_context* GetSecp256k1ContextSignVerify();
secp256k1_context* GetSecp256k1ContextSign();
secp256k1_context* GetSecp256k1ContextVerify();
secp256k1_context* GetSecp256k1ContextNone();

// Call once early in process (after args parsed)
inline void InitSecp256k1Once() {
    (void)GetSecp256k1ContextSignVerify();
}

// Canonical shared secp256k1 contexts for production code.
inline secp256k1_context* GetSecp256k1ContextSignVerify() {
    return detail::InitSecpContext(detail::SignVerifySlot(), SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
}

inline secp256k1_context* GetSecp256k1ContextSign() {
    return detail::InitSecpContext(detail::SignSlot(), SECP256K1_CONTEXT_SIGN);
}

inline secp256k1_context* GetSecp256k1ContextVerify() {
    return detail::InitSecpContext(detail::VerifySlot(), SECP256K1_CONTEXT_VERIFY);
}

inline secp256k1_context* GetSecp256k1ContextNone() {
    return detail::InitSecpContext(detail::NoneSlot(), SECP256K1_CONTEXT_NONE);
}

// Returns the canonical shared sign+verify context.
inline secp256k1_context* CreateSecp256k1Context() {
    return GetSecp256k1ContextSignVerify();
}

// Generate a valid 32-byte secp256k1 private key using the project CSPRNG.
inline bool GenerateSecp256k1PrivateKey(unsigned char out32[32]) {
    if (!out32) {
        return false;
    }

    auto* ctx = GetSecp256k1ContextSignVerify();
    do {
        if (RAND_bytes(out32, 32) != 1) {
            return false;
        }
    } while (!secp256k1_ec_seckey_verify(ctx, out32));

    return true;
}

// Get compressed 33-byte public key from private key (array version)
// Returns true on success, false on failure
inline bool GetCompressedPubKey33(const unsigned char seckey[32], std::array<unsigned char,33>& out) {
    auto* ctx = GetSecp256k1ContextSignVerify();

    bool allZero = true;
    for (int i = 0; i < 32; ++i) {
        if (seckey[i]) {
            allZero = false;
            break;
        }
    }
    if (allZero) {
        std::fprintf(stderr, "GetCompressedPubKey33: seckey is all zeros\n");
        return false;
    }

    if (!secp256k1_ec_seckey_verify(ctx, seckey)) {
        std::fprintf(stderr, "GetCompressedPubKey33: invalid private key\n");
        return false;
    }

    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, seckey)) {
        std::fprintf(stderr, "GetCompressedPubKey33: failed to create public key\n");
        return false;
    }

    size_t output_len = 33;
    if (!secp256k1_ec_pubkey_serialize(ctx, out.data(), &output_len, &pubkey, SECP256K1_EC_COMPRESSED)) {
        std::fprintf(stderr, "GetCompressedPubKey33: failed to serialize public key\n");
        return false;
    }

    return output_len == 33;
}

// Get compressed public key from private key (vector version) - canonical helper
// Returns empty vector on failure, 33-byte compressed pubkey on success
inline std::vector<unsigned char> GetCompressedPubKey(const unsigned char seckey[32]) {
    std::array<unsigned char, 33> pubkey_array;
    if (GetCompressedPubKey33(seckey, pubkey_array)) {
        return std::vector<unsigned char>(pubkey_array.begin(), pubkey_array.end());
    }
    return {};
}

// Self-test function (dev builds only)
inline void Secp256k1SelfTest() {
    unsigned char test_seckey[32];
    if (!GenerateSecp256k1PrivateKey(test_seckey)) {
        std::fprintf(stderr, "Secp256k1SelfTest: failed to generate test private key\n");
        return;
    }

    std::array<unsigned char, 33> test_pubkey;
    if (!GetCompressedPubKey33(test_seckey, test_pubkey)) {
        std::fprintf(stderr, "Secp256k1SelfTest: failed to derive public key\n");
        return;
    }

    std::vector<unsigned char> test_pubkey_vec = GetCompressedPubKey(test_seckey);
    if (test_pubkey_vec.size() != 33) {
        std::fprintf(stderr, "Secp256k1SelfTest: vector version returned wrong size\n");
        return;
    }

    if (memcmp(test_pubkey.data(), test_pubkey_vec.data(), 33) != 0) {
        std::fprintf(stderr, "Secp256k1SelfTest: array and vector versions differ\n");
        return;
    }

    std::fprintf(stderr, "Secp256k1SelfTest: all tests passed\n");
}

} // namespace dinero::crypto

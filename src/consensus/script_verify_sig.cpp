#include "consensus/script_interpreter.h"
#include "crypto/evp_secp256k1.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
#include <cstring>

namespace dinero {
namespace consensus {

// ============================================================================
// Phase 24.2: Signature Verification
// ============================================================================

static secp256k1_context* GetSecp256k1Context() {
    return dinero::crypto::GetSecp256k1ContextSignVerify();
}

// ============================================================================
// DER Signature Parsing
// ============================================================================

/**
 * Parse DER-encoded ECDSA signature
 *
 * DER format:
 * 0x30 [total-length] 0x02 [R-length] [R] 0x02 [S-length] [S]
 *
 * Returns false if signature is invalid.
 */
static bool ParseDERSignature(
    const std::vector<uint8_t>& sig,
    std::vector<uint8_t>& r,
    std::vector<uint8_t>& s,
    bool strict
) {
    if (sig.size() < 8 || sig.size() > 72) {
        return false;  // Invalid length
    }

    const uint8_t* p = sig.data();
    const uint8_t* end = p + sig.size();

    // Check sequence tag
    if (*p != 0x30) {
        return false;
    }
    p++;

    // Check total length
    uint8_t total_len = *p++;
    if (strict) {
        // Strict mode: DER must extend exactly to end of buffer
        if (p + total_len != end) {
            return false;
        }
    } else {
        // Non-strict: DER must fit within buffer (trailing bytes allowed)
        if (p + total_len > end) {
            return false;
        }
        // Use DER-specified end for parsing, ignore trailing bytes
        end = p + total_len;
    }

    // Parse R
    if (*p != 0x02) {
        return false;
    }
    p++;

    uint8_t r_len = *p++;
    if (r_len == 0 || p + r_len >= end) {
        return false;
    }

    // Check R value
    if (strict) {
        // Must not be negative
        if (p[0] & 0x80) {
            return false;
        }
        // Must not be padded
        if (r_len > 1 && p[0] == 0x00 && !(p[1] & 0x80)) {
            return false;
        }
    }

    r.assign(p, p + r_len);
    p += r_len;

    // Parse S
    if (*p != 0x02) {
        return false;
    }
    p++;

    uint8_t s_len = *p++;
    if (s_len == 0 || p + s_len != end) {
        return false;
    }

    // Check S value
    if (strict) {
        // Must not be negative
        if (p[0] & 0x80) {
            return false;
        }
        // Must not be padded
        if (s_len > 1 && p[0] == 0x00 && !(p[1] & 0x80)) {
            return false;
        }
    }

    s.assign(p, p + s_len);

    return true;
}

/**
 * Check if S value is low (BIP 62)
 *
 * To prevent signature malleability, we require S <= order/2.
 * This is enforced by the LOW_S flag.
 */
static bool IsLowS(const std::vector<uint8_t>& s) {
    // secp256k1 order (n)
    static const uint8_t order_half[32] = {
        0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x5D, 0x57, 0x6E, 0x73, 0x57, 0xA4, 0x50, 0x1D,
        0xDF, 0xE9, 0x2F, 0x46, 0x68, 0x1B, 0x20, 0xA0
    };

    // Pad S to 32 bytes
    std::vector<uint8_t> s_padded(32, 0);
    if (s.size() <= 32) {
        std::copy(s.begin(), s.end(), s_padded.end() - s.size());
    } else {
        return false;  // S too large
    }

    // Compare with order/2
    for (int i = 0; i < 32; i++) {
        if (s_padded[i] < order_half[i]) {
            return true;
        }
        if (s_padded[i] > order_half[i]) {
            return false;
        }
    }

    return true;  // Equal is okay
}

/**
 * Verify ECDSA signature (legacy P2PKH, P2SH)
 *
 * This implements secp256k1 ECDSA verification with DER encoding.
 */
bool CheckECDSASignature(
    const std::vector<uint8_t>& signature,
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& sighash,
    uint32_t flags
) {
    // Phase 26: Optimized ECDSA verification

    // Fast path: Check basic validity
    if (signature.empty() || pubkey.empty() || sighash.size() != 32) {
        return false;
    }

    auto* secp256k1_ctx = GetSecp256k1Context();

    // Extract hash type byte (last byte of signature)
    std::vector<uint8_t> sig_data = signature;
    uint8_t hash_type = sig_data.back();

    // Check for valid sighash type when STRICTENC is set
    if (flags & SCRIPT_VERIFY_STRICTENC) {
        uint8_t base_type = hash_type & ~0x80;
        if (base_type < SIGHASH_ALL || base_type > SIGHASH_SINGLE) {
            return false;
        }
    }
    sig_data.pop_back();

    // Parse DER signature
    std::vector<uint8_t> r, s;
    bool strict_der = (flags & SCRIPT_VERIFY_DERSIG);
    if (!ParseDERSignature(sig_data, r, s, strict_der)) {
        return false;
    }

    // Check low S value (BIP 62)
    if ((flags & (SCRIPT_VERIFY_LOW_S | SCRIPT_VERIFY_STRICTENC)) && !IsLowS(s)) {
        return false;
    }

    // Parse public key
    secp256k1_pubkey pk;
    if (!secp256k1_ec_pubkey_parse(secp256k1_ctx, &pk, pubkey.data(), pubkey.size())) {
        return false;
    }

    // Check compressed pubkey requirement (for witness)
    if (flags & SCRIPT_VERIFY_WITNESS_PUBKEYTYPE) {
        if (pubkey.size() != 33) {
            return false;
        }
    }

    // Parse signature for secp256k1 verification
    secp256k1_ecdsa_signature sig_internal;

    if (!strict_der) {
        // Non-strict mode: use compact format from extracted r,s
        uint8_t compact[64] = {0};

        // Pad R to 32 bytes (right-aligned)
        if (r.size() > 32) {
            size_t offset = 0;
            while (offset < r.size() - 32 && r[offset] == 0) offset++;
            if (r.size() - offset > 32) return false;
            std::copy(r.begin() + offset, r.end(), compact + 32 - (r.size() - offset));
        } else {
            std::copy(r.begin(), r.end(), compact + 32 - r.size());
        }

        // Pad S to 32 bytes (right-aligned)
        if (s.size() > 32) {
            size_t offset = 0;
            while (offset < s.size() - 32 && s[offset] == 0) offset++;
            if (s.size() - offset > 32) return false;
            std::copy(s.begin() + offset, s.end(), compact + 64 - (s.size() - offset));
        } else {
            std::copy(s.begin(), s.end(), compact + 64 - s.size());
        }

        if (!secp256k1_ecdsa_signature_parse_compact(secp256k1_ctx, &sig_internal, compact)) {
            return false;
        }
    } else {
        // Strict mode: use secp256k1's DER parser
        if (!secp256k1_ecdsa_signature_parse_der(secp256k1_ctx, &sig_internal, sig_data.data(), sig_data.size())) {
            return false;
        }
    }

    // Normalize signature to low-S form for verification
    secp256k1_ecdsa_signature sig_normalized;
    secp256k1_ecdsa_signature_normalize(secp256k1_ctx, &sig_normalized, &sig_internal);

    // Verify signature
    return secp256k1_ecdsa_verify(secp256k1_ctx, &sig_normalized, sighash.data(), &pk) == 1;
}

/**
 * Check if signature has valid DER encoding (BIP 66)
 *
 * This is a standalone DER validation check used before signature verification.
 * When DERSIG flag is set and this returns false, the script should fail with SIG_DER.
 *
 * @param sig            Signature bytes (DER + sighash byte)
 * @return               True if valid DER encoding, false otherwise
 */
bool IsValidSignatureEncoding(const std::vector<uint8_t>& sig) {
    // Empty signature is valid (for CHECKSIG NOT etc)
    if (sig.empty()) {
        return true;
    }

    // Minimum DER signature is 9 bytes: 30 06 02 01 R 02 01 S + sighash
    // Maximum is 73 bytes: 30 45 02 21 R 02 21 S + sighash
    if (sig.size() < 9 || sig.size() > 73) {
        return false;
    }

    // A signature is of type 0x30 (compound)
    if (sig[0] != 0x30) {
        return false;
    }

    // Make sure the length covers the entire signature
    if (sig[1] != sig.size() - 3) {
        return false;
    }

    // Extract the length of the R element
    size_t lenR = sig[3];

    // Make sure the length of the S element is still inside the signature
    if (5 + lenR >= sig.size()) {
        return false;
    }

    // Extract the length of the S element
    size_t lenS = sig[5 + lenR];

    // Verify that the length of the signature matches the sum of the length
    // of the elements
    if ((size_t)(lenR + lenS + 7) != sig.size()) {
        return false;
    }

    // Check whether the R element is an integer
    if (sig[2] != 0x02) {
        return false;
    }

    // Zero-length integers are not allowed for R
    if (lenR == 0) {
        return false;
    }

    // Negative numbers are not allowed for R
    if (sig[4] & 0x80) {
        return false;
    }

    // Null bytes at the start of R are not allowed, unless R would
    // otherwise be interpreted as a negative number
    if (lenR > 1 && (sig[4] == 0x00) && !(sig[5] & 0x80)) {
        return false;
    }

    // Check whether the S element is an integer
    if (sig[lenR + 4] != 0x02) {
        return false;
    }

    // Zero-length integers are not allowed for S
    if (lenS == 0) {
        return false;
    }

    // Negative numbers are not allowed for S
    if (sig[lenR + 6] & 0x80) {
        return false;
    }

    // Null bytes at the start of S are not allowed, unless S would
    // otherwise be interpreted as a negative number
    if (lenS > 1 && (sig[lenR + 6] == 0x00) && !(sig[lenR + 7] & 0x80)) {
        return false;
    }

    return true;
}

/**
 * Verify Schnorr signature (Taproot BIP 340)
 *
 * This implements secp256k1 Schnorr signature verification.
 * Schnorr signatures are 64 bytes (no DER encoding).
 */
bool CheckSchnorrSignature(
    const std::vector<uint8_t>& signature,
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& sighash,
    uint32_t flags
) {
    auto* secp256k1_ctx = GetSecp256k1Context();

    if (sighash.size() != 32) {
        return false;
    }

    // Schnorr signature: 64 bytes (or 65 with sighash type)
    std::vector<uint8_t> sig_data = signature;

    // Remove optional sighash type byte
    if (sig_data.size() == 65) {
        uint8_t hash_type = sig_data.back();
        if (hash_type != 0x00 && hash_type != SIGHASH_ALL) {
            // Invalid sighash type for Taproot
            if (flags & SCRIPT_VERIFY_STRICTENC) {
                return false;
            }
        }
        sig_data.pop_back();
    }

    if (sig_data.size() != 64) {
        return false;  // Invalid signature length
    }

    // X-only public key must be 32 bytes
    if (pubkey.size() != 32) {
        return false;
    }

    // Parse x-only public key
    secp256k1_xonly_pubkey xonly_pk;
    if (!secp256k1_xonly_pubkey_parse(secp256k1_ctx, &xonly_pk, pubkey.data())) {
        return false;
    }

    // Verify Schnorr signature
    int result = secp256k1_schnorrsig_verify(
        secp256k1_ctx,
        sig_data.data(),
        sighash.data(),
        32,
        &xonly_pk
    );

    return (result == 1);
}

} // namespace consensus
} // namespace dinero

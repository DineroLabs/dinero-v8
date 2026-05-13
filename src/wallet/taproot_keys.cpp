#include "wallet/taproot_keys.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/tagged_hash.h"
#include "address/addr_codec.h"
#include "consensus/chainparams.h"
#include "common/logger.h"
#include "bech32/bech32.hpp"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <cstring>

namespace dinero {

static secp256k1_context* GetSecp256k1Context() {
    return dinero::crypto::GetSecp256k1ContextSignVerify();
}

bool TaprootKeys::GenerateKeypair(std::array<uint8_t, 32>& privkey,
                                  std::array<uint8_t, 32>& xonly_pubkey,
                                  int& pubkey_parity) {
    if (!dinero::crypto::GenerateSecp256k1PrivateKey(privkey.data())) {
        g_logger.error("Failed to generate valid private key");
        return false;
    }

    // Derive x-only public key
    return DeriveXOnlyPubkey(privkey, xonly_pubkey, pubkey_parity);
}

bool TaprootKeys::DeriveXOnlyPubkey(const std::array<uint8_t, 32>& privkey,
                                    std::array<uint8_t, 32>& xonly_pubkey,
                                    int& pubkey_parity) {
    auto* ctx = GetSecp256k1Context();

    // Create keypair from private key
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, privkey.data())) {
        g_logger.error("Failed to create keypair from private key");
        return false;
    }

    // Extract x-only public key
    secp256k1_xonly_pubkey xonly;
    if (!secp256k1_keypair_xonly_pub(ctx, &xonly, &pubkey_parity, &keypair)) {
        g_logger.error("Failed to extract x-only public key");
        return false;
    }

    // Serialize x-only public key (32 bytes)
    if (!secp256k1_xonly_pubkey_serialize(ctx, xonly_pubkey.data(), &xonly)) {
        g_logger.error("Failed to serialize x-only public key");
        return false;
    }

    return true;
}

bool TaprootKeys::SignSchnorr(std::array<uint8_t, 64>& sig64,
                              const std::array<uint8_t, 32>& msg32,
                              const std::array<uint8_t, 32>& privkey,
                              const uint8_t* aux_rand32) {
    auto* ctx = GetSecp256k1Context();

    // Create keypair from private key
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, privkey.data())) {
        g_logger.error("Failed to create keypair for signing");
        return false;
    }

    // Sign using BIP340 Schnorr signatures
    if (!secp256k1_schnorrsig_sign32(ctx, sig64.data(), msg32.data(), &keypair, aux_rand32)) {
        g_logger.error("Failed to create Schnorr signature");
        return false;
    }

    return true;
}

bool TaprootKeys::VerifySchnorr(const std::array<uint8_t, 64>& sig64,
                                const std::array<uint8_t, 32>& msg32,
                                const std::array<uint8_t, 32>& xonly_pubkey) {
    auto* ctx = GetSecp256k1Context();

    // Parse x-only public key
    secp256k1_xonly_pubkey pubkey;
    if (!secp256k1_xonly_pubkey_parse(ctx, &pubkey, xonly_pubkey.data())) {
        g_logger.error("Failed to parse x-only public key for verification");
        return false;
    }

    // Verify Schnorr signature
    return secp256k1_schnorrsig_verify(ctx, sig64.data(), msg32.data(), 32, &pubkey) == 1;
}

std::string TaprootKeys::CreateTaprootAddress(const std::array<uint8_t, 32>& xonly_pubkey,
                                              const std::string& hrp) {
    // Convert array to vector for bech32 encoding
    std::vector<uint8_t> witness_program(xonly_pubkey.begin(), xonly_pubkey.end());

    // Encode as bech32m (witness version 1)
    std::string address = bech32::Encode(hrp, 1, witness_program, bech32::Encoding::BECH32M);

    if (address.empty()) {
        g_logger.error("Failed to encode Taproot address");
        return "";
    }

    return address;
}

// Helper: Compute BIP340 TapTweak hash using canonical TaggedHash
static void ComputeTapTweakHash(const uint8_t* xonly_pubkey, uint8_t* tweak_out) {
    dinero::crypto::TaggedHash("TapTweak", xonly_pubkey, 32, tweak_out);
}

bool TaprootKeys::TweakPrivkey(std::array<uint8_t, 32>& privkey,
                               const std::array<uint8_t, 32>& xonly_pubkey) {
    // DEPRECATED: This function cannot correctly extract the tweaked secret key
    // because secp256k1's keypair structure is opaque and the library handles
    // Y parity internally without exposing the properly-negated secret key.
    //
    // Use SignSchnorrWithInternalKey for signing instead.

    auto* ctx = GetSecp256k1Context();

    // Create keypair from private key
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, privkey.data())) {
        g_logger.error("TweakPrivkey: Failed to create keypair");
        return false;
    }

    // Compute tweak
    unsigned char tweak[32];
    ComputeTapTweakHash(xonly_pubkey.data(), tweak);

    // Apply tweak
    if (!secp256k1_keypair_xonly_tweak_add(ctx, &keypair, tweak)) {
        g_logger.error("TweakPrivkey: Failed to tweak keypair");
        return false;
    }

    // WARNING: This extraction is unreliable. The keypair structure is opaque
    // and the library may not store the properly-negated secret key in a way
    // we can extract. This is kept for backwards compatibility but should not
    // be used for signing.
    std::memcpy(privkey.data(), keypair.data, 32);

    return true;
}

bool TaprootKeys::SignSchnorrWithInternalKey(std::array<uint8_t, 64>& sig64,
                                             const std::array<uint8_t, 32>& msg32,
                                             const std::array<uint8_t, 32>& internal_privkey,
                                             const std::array<uint8_t, 32>& internal_xonly_pubkey,
                                             const uint8_t* aux_rand32) {
    auto* ctx = GetSecp256k1Context();

    // 1. Create keypair from internal private key
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, internal_privkey.data())) {
        g_logger.error("SignSchnorrWithInternalKey: Failed to create keypair");
        return false;
    }

    // 2. Compute TapTweak hash
    unsigned char tweak[32];
    ComputeTapTweakHash(internal_xonly_pubkey.data(), tweak);

    // 3. Apply tweak to keypair
    // This function handles Y parity internally - if the output key has odd Y,
    // it negates both the secret key and public key in the keypair structure.
    if (!secp256k1_keypair_xonly_tweak_add(ctx, &keypair, tweak)) {
        g_logger.error("SignSchnorrWithInternalKey: Failed to apply tweak");
        return false;
    }

    // 4. Sign directly with the tweaked keypair
    // secp256k1_schnorrsig_sign32 uses the keypair's internal representation
    // which correctly handles the Y parity (negation if needed).
    if (!secp256k1_schnorrsig_sign32(ctx, sig64.data(), msg32.data(), &keypair, aux_rand32)) {
        g_logger.error("SignSchnorrWithInternalKey: Schnorr signing failed");
        return false;
    }

    return true;
}

bool TaprootKeys::ComputeTweakedPubkey(const std::array<uint8_t, 32>& internal_xonly_pubkey,
                                       std::array<uint8_t, 32>& tweaked_xonly_pubkey) {
    auto* ctx = GetSecp256k1Context();

    // 1. Parse internal x-only pubkey
    secp256k1_xonly_pubkey internal_pubkey;
    if (!secp256k1_xonly_pubkey_parse(ctx, &internal_pubkey, internal_xonly_pubkey.data())) {
        g_logger.error("ComputeTweakedPubkey: Failed to parse internal pubkey");
        return false;
    }

    // 2. Compute TapTweak hash
    unsigned char tweak[32];
    ComputeTapTweakHash(internal_xonly_pubkey.data(), tweak);

    // 3. Apply tweak to get output pubkey
    // secp256k1_xonly_pubkey_tweak_add computes: output = internal + tweak * G
    secp256k1_pubkey output_pubkey;
    if (!secp256k1_xonly_pubkey_tweak_add(ctx, &output_pubkey, &internal_pubkey, tweak)) {
        g_logger.error("ComputeTweakedPubkey: Failed to apply tweak");
        return false;
    }

    // 4. Convert to x-only (drops the Y coordinate, keeps only X)
    secp256k1_xonly_pubkey output_xonly;
    int parity;  // We don't need this for the scriptPubKey
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &output_xonly, &parity, &output_pubkey)) {
        g_logger.error("ComputeTweakedPubkey: Failed to convert to x-only");
        return false;
    }

    // 5. Serialize
    if (!secp256k1_xonly_pubkey_serialize(ctx, tweaked_xonly_pubkey.data(), &output_xonly)) {
        g_logger.error("ComputeTweakedPubkey: Failed to serialize tweaked pubkey");
        return false;
    }

    return true;
}

bool TaprootKeys::ComputeTweakedPrivkey(const std::array<uint8_t, 32>& internal_privkey,
                                         const std::array<uint8_t, 32>& internal_xonly,
                                         std::array<uint8_t, 32>& tweaked_privkey) {
    auto* ctx = GetSecp256k1Context();

    // 1. Create pubkey to determine Y parity
    secp256k1_pubkey P;
    if (!secp256k1_ec_pubkey_create(ctx, &P, internal_privkey.data())) {
        g_logger.error("ComputeTweakedPrivkey: Failed to create pubkey");
        return false;
    }

    secp256k1_xonly_pubkey xonly_pk;
    int pk_parity;
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly_pk, &pk_parity, &P)) {
        g_logger.error("ComputeTweakedPrivkey: Failed to get x-only pubkey");
        return false;
    }

    // 2. Compute TapTweak hash from internal x-only pubkey
    unsigned char tweak[32];
    ComputeTapTweakHash(internal_xonly.data(), tweak);

    // 3. Copy private key, negate if odd Y parity
    std::memcpy(tweaked_privkey.data(), internal_privkey.data(), 32);
    if (pk_parity) {
        if (!secp256k1_ec_seckey_negate(ctx, tweaked_privkey.data())) {
            OPENSSL_cleanse(tweaked_privkey.data(), 32);
            g_logger.error("ComputeTweakedPrivkey: Failed to negate private key");
            return false;
        }
    }

    // 4. Add tweak to private key
    if (!secp256k1_ec_seckey_tweak_add(ctx, tweaked_privkey.data(), tweak)) {
        OPENSSL_cleanse(tweaked_privkey.data(), 32);
        g_logger.error("ComputeTweakedPrivkey: Failed to add tweak");
        return false;
    }

    OPENSSL_cleanse(tweak, sizeof(tweak));
    return true;
}

// ============================================================================
// Script-Path Spending (BIP342)
// ============================================================================

// Helper: Compute BIP340 tagged hash using canonical TaggedHash
static void ComputeTaggedHash(const char* tag, const uint8_t* data, size_t len, uint8_t* out) {
    dinero::crypto::TaggedHash(tag, data, len, out);
}

bool TaprootKeys::ComputeTapleafHash(const std::vector<uint8_t>& script,
                                     uint8_t leaf_version,
                                     std::array<uint8_t, 32>& leaf_hash) {
    // BIP341: tapleaf_hash = TaggedHash("TapLeaf", leaf_version || compact_size(script) || script)

    std::vector<uint8_t> data;

    // 1. Leaf version (1 byte)
    data.push_back(leaf_version);

    // 2. Compact size of script
    if (script.size() < 253) {
        data.push_back(static_cast<uint8_t>(script.size()));
    } else if (script.size() <= 0xffff) {
        data.push_back(0xfd);
        data.push_back(script.size() & 0xff);
        data.push_back((script.size() >> 8) & 0xff);
    } else if (script.size() <= 0xffffffff) {
        data.push_back(0xfe);
        data.push_back(script.size() & 0xff);
        data.push_back((script.size() >> 8) & 0xff);
        data.push_back((script.size() >> 16) & 0xff);
        data.push_back((script.size() >> 24) & 0xff);
    } else {
        g_logger.error("ComputeTapleafHash: Script too large");
        return false;
    }

    // 3. Script bytes
    data.insert(data.end(), script.begin(), script.end());

    // 4. Compute tagged hash
    ComputeTaggedHash("TapLeaf", data.data(), data.size(), leaf_hash.data());

    return true;
}

bool TaprootKeys::ComputeTapBranchHash(const std::array<uint8_t, 32>& left,
                                       const std::array<uint8_t, 32>& right,
                                       std::array<uint8_t, 32>& branch_hash) {
    // BIP341: tapbranch_hash = TaggedHash("TapBranch", sorted(left || right))
    // The two hashes are sorted lexicographically before concatenation

    std::vector<uint8_t> data;
    data.reserve(64);

    // Sort lexicographically
    if (std::memcmp(left.data(), right.data(), 32) <= 0) {
        data.insert(data.end(), left.begin(), left.end());
        data.insert(data.end(), right.begin(), right.end());
    } else {
        data.insert(data.end(), right.begin(), right.end());
        data.insert(data.end(), left.begin(), left.end());
    }

    ComputeTaggedHash("TapBranch", data.data(), data.size(), branch_hash.data());

    return true;
}

bool TaprootKeys::ComputeOutputKeyParity(const std::array<uint8_t, 32>& internal_xonly_pubkey,
                                         const std::array<uint8_t, 32>& merkle_root,
                                         int& parity) {
    auto* ctx = GetSecp256k1Context();

    // 1. Parse internal x-only pubkey
    secp256k1_xonly_pubkey internal_pubkey;
    if (!secp256k1_xonly_pubkey_parse(ctx, &internal_pubkey, internal_xonly_pubkey.data())) {
        g_logger.error("ComputeOutputKeyParity: Failed to parse internal pubkey");
        return false;
    }

    // 2. Compute tweak: TaggedHash("TapTweak", internal_key || merkle_root)
    // For key-only (no scripts), merkle_root is all zeros and omitted from hash
    std::vector<uint8_t> tweak_data;
    tweak_data.insert(tweak_data.end(), internal_xonly_pubkey.begin(), internal_xonly_pubkey.end());

    // Check if merkle_root is non-zero
    bool has_merkle_root = false;
    for (size_t i = 0; i < 32; i++) {
        if (merkle_root[i] != 0) {
            has_merkle_root = true;
            break;
        }
    }
    if (has_merkle_root) {
        tweak_data.insert(tweak_data.end(), merkle_root.begin(), merkle_root.end());
    }

    uint8_t tweak[32];
    ComputeTaggedHash("TapTweak", tweak_data.data(), tweak_data.size(), tweak);

    // 3. Apply tweak to get output pubkey
    secp256k1_pubkey output_pubkey;
    if (!secp256k1_xonly_pubkey_tweak_add(ctx, &output_pubkey, &internal_pubkey, tweak)) {
        g_logger.error("ComputeOutputKeyParity: Failed to apply tweak");
        return false;
    }

    // 4. Extract parity
    secp256k1_xonly_pubkey output_xonly;
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &output_xonly, &parity, &output_pubkey)) {
        g_logger.error("ComputeOutputKeyParity: Failed to extract parity");
        return false;
    }

    return true;
}

} // namespace dinero

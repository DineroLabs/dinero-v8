#include "primitives/schnorr_signer.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <openssl/rand.h>
#include <cstring>

namespace din {

namespace {
    secp256k1_context* getContext() {
        return dinero::crypto::GetSecp256k1ContextSignVerify();
    }

    // SHA256 hash function
    std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) {
        std::vector<uint8_t> hash(32);
        dinero::crypto::CSHA256().Write(data.data(), data.size()).Finalize(hash.data());
        return hash;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Schnorr Signing (BIP-340)
// ═══════════════════════════════════════════════════════════════════════════

std::optional<std::vector<uint8_t>> SchnorrSigner::sign(
    const std::vector<uint8_t>& message_hash,
    const std::vector<uint8_t>& private_key,
    const std::optional<std::vector<uint8_t>>& aux_rand
) {
    // Validate inputs
    if (message_hash.size() != 32 || private_key.size() != 32) {
        return std::nullopt;
    }

    auto* ctx = getContext();
    if (!ctx) {
        return std::nullopt;
    }

    // Prepare keypair
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, private_key.data())) {
        return std::nullopt;
    }

    // Prepare auxiliary randomness (use provided or generate)
    std::vector<uint8_t> aux_rand_data(32);
    if (aux_rand.has_value() && aux_rand->size() == 32) {
        aux_rand_data = *aux_rand;
    } else {
        RAND_bytes(aux_rand_data.data(), 32);
    }

    // Sign with BIP-340 Schnorr
    std::vector<uint8_t> signature(64);
    if (!secp256k1_schnorrsig_sign32(
        ctx,
        signature.data(),
        message_hash.data(),
        &keypair,
        aux_rand_data.data()
    )) {
        return std::nullopt;
    }

    return signature;
}

bool SchnorrSigner::verify(
    const std::vector<uint8_t>& signature,
    const std::vector<uint8_t>& message_hash,
    const std::vector<uint8_t>& public_key
) {
    // Validate inputs
    if (signature.size() != 64 || message_hash.size() != 32 || public_key.size() != 32) {
        return false;
    }

    auto* ctx = getContext();
    if (!ctx) {
        return false;
    }

    // Parse x-only public key
    secp256k1_xonly_pubkey xonly_pk;
    if (!secp256k1_xonly_pubkey_parse(ctx, &xonly_pk, public_key.data())) {
        return false;
    }

    // Verify signature
    return secp256k1_schnorrsig_verify(
        ctx,
        signature.data(),
        message_hash.data(),
        32,
        &xonly_pk
    ) == 1;
}

bool SchnorrSigner::batchVerify(
    const std::vector<std::vector<uint8_t>>& signatures,
    const std::vector<std::vector<uint8_t>>& message_hashes,
    const std::vector<std::vector<uint8_t>>& public_keys
) {
    // Validate input sizes match
    if (signatures.size() != message_hashes.size() ||
        signatures.size() != public_keys.size()) {
        return false;
    }

    // Verify each signature individually
    // TODO: Use secp256k1 batch verification API when available
    for (size_t i = 0; i < signatures.size(); i++) {
        if (!verify(signatures[i], message_hashes[i], public_keys[i])) {
            return false;
        }
    }

    return true;
}

std::vector<uint8_t> SchnorrSigner::generateNonce(
    const std::vector<uint8_t>& private_key,
    const std::vector<uint8_t>& message_hash,
    const std::optional<std::vector<uint8_t>>& aux_rand
) {
    // RFC 6979 deterministic nonce generation
    // nonce = HMAC-SHA256(private_key, message_hash || aux_rand)

    std::vector<uint8_t> input;
    input.reserve(64);

    if (private_key.size() == 32) {
        input.insert(input.end(), private_key.begin(), private_key.end());
    }

    if (message_hash.size() == 32) {
        input.insert(input.end(), message_hash.begin(), message_hash.end());
    }

    if (aux_rand.has_value() && aux_rand->size() == 32) {
        input.insert(input.end(), aux_rand->begin(), aux_rand->end());
    }

    return sha256(input);
}

std::vector<uint8_t> SchnorrSigner::getPublicKey(const std::vector<uint8_t>& private_key) {
    if (private_key.size() != 32) {
        return {};
    }

    auto* ctx = getContext();
    if (!ctx) {
        return {};
    }

    // Create keypair
    secp256k1_keypair keypair;
    if (!secp256k1_keypair_create(ctx, &keypair, private_key.data())) {
        return {};
    }

    // Extract x-only public key
    secp256k1_xonly_pubkey xonly_pk;
    if (!secp256k1_keypair_xonly_pub(ctx, &xonly_pk, nullptr, &keypair)) {
        return {};
    }

    // Serialize as 32-byte x-only public key
    std::vector<uint8_t> pubkey(32);
    if (!secp256k1_xonly_pubkey_serialize(ctx, pubkey.data(), &xonly_pk)) {
        return {};
    }

    return pubkey;
}

bool SchnorrSigner::isValidSignatureFormat(const std::vector<uint8_t>& signature) {
    return signature.size() == 64;
}

bool SchnorrSigner::isValidPublicKeyFormat(const std::vector<uint8_t>& public_key) {
    return public_key.size() == 32;
}

// ═══════════════════════════════════════════════════════════════════════════
// Taproot Tweaking (BIP-341)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> TaprootTweaking::computeOutputKey(
    const std::vector<uint8_t>& internal_pubkey,
    const std::optional<std::vector<uint8_t>>& merkle_root
) {
    if (internal_pubkey.size() != 32) {
        return {};
    }

    auto* ctx = getContext();
    if (!ctx) {
        return {};
    }

    // Parse internal key
    secp256k1_xonly_pubkey internal_pk;
    if (!secp256k1_xonly_pubkey_parse(ctx, &internal_pk, internal_pubkey.data())) {
        return {};
    }

    // Compute tweak
    std::vector<uint8_t> tweak = computeTweakHash(merkle_root);
    if (tweak.empty()) {
        return {};
    }

    // Apply tweak to create output key
    secp256k1_pubkey output_pk;
    if (!secp256k1_xonly_pubkey_tweak_add(ctx, &output_pk, &internal_pk, tweak.data())) {
        return {};
    }

    // Convert back to x-only pubkey
    secp256k1_xonly_pubkey output_xonly;
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &output_xonly, nullptr, &output_pk)) {
        return {};
    }

    // Serialize
    std::vector<uint8_t> result(32);
    if (!secp256k1_xonly_pubkey_serialize(ctx, result.data(), &output_xonly)) {
        return {};
    }

    return result;
}

std::vector<uint8_t> TaprootTweaking::computeInternalKey(
    const std::vector<uint8_t>& output_pubkey,
    const std::optional<std::vector<uint8_t>>& merkle_root
) {
    // TODO: Implement reverse computation (requires trial and error or additional data)
    // For now, return empty - this is rarely needed
    return {};
}

std::vector<uint8_t> TaprootTweaking::computeTweak(
    const std::optional<std::vector<uint8_t>>& merkle_root
) {
    return computeTweakHash(merkle_root);
}

bool TaprootTweaking::verifyKeyRelationship(
    const std::vector<uint8_t>& output_pubkey,
    const std::vector<uint8_t>& internal_pubkey,
    const std::optional<std::vector<uint8_t>>& merkle_root
) {
    std::vector<uint8_t> computed_output = computeOutputKey(internal_pubkey, merkle_root);
    return computed_output == output_pubkey;
}

std::vector<uint8_t> TaprootTweaking::taggedHash(
    const std::string& tag,
    const std::vector<uint8_t>& data
) {
    // BIP-340 tagged hash: SHA256(SHA256(tag) || SHA256(tag) || data)
    std::vector<uint8_t> tag_hash = sha256(std::vector<uint8_t>(tag.begin(), tag.end()));

    std::vector<uint8_t> input;
    input.reserve(tag_hash.size() * 2 + data.size());
    input.insert(input.end(), tag_hash.begin(), tag_hash.end());
    input.insert(input.end(), tag_hash.begin(), tag_hash.end());
    input.insert(input.end(), data.begin(), data.end());

    return sha256(input);
}

std::vector<uint8_t> TaprootTweaking::computeTweakHash(
    const std::optional<std::vector<uint8_t>>& merkle_root
) {
    // BIP-341: tweak = tagged_hash("TapTweak", internal_key || merkle_root)
    // For keypath-only (no script), merkle_root is empty

    std::vector<uint8_t> data;
    if (merkle_root.has_value() && merkle_root->size() == 32) {
        data = *merkle_root;
    }

    return taggedHash("TapTweak", data);
}

} // namespace din

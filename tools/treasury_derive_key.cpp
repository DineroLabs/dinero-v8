// One-time recovery tool: derive the tweaked Taproot private key from a known seed.
// Usage: treasury_derive_key <seed_hex> <coin_type>
// Output: tweaked private key hex + corresponding address

#include "wallet/bip32_deriver.h"
#include "wallet/taproot_keys.h"
#include "crypto/sha256.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <array>
#include <vector>
#include <string>
#include <stdexcept>

// sha256 used by taproot tweak
extern void sha256(const uint8_t* data, size_t len, uint8_t out32[32]);

static std::string to_hex(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[i]);
        out += buf;
    }
    return out;
}

static bool from_hex(const char* hex, uint8_t* out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        unsigned int val;
        if (sscanf(hex + i * 2, "%2x", &val) != 1) return false;
        out[i] = (uint8_t)val;
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <seed_hex_128chars> <coin_type> [chain=0] [index=0]\n", argv[0]);
        return 1;
    }

    // Parse seed
    uint8_t seed[64];
    if (!from_hex(argv[1], seed, 64)) {
        fprintf(stderr, "Error: seed must be 128 hex chars (64 bytes)\n");
        return 1;
    }

    uint32_t coin_type = (uint32_t)atoi(argv[2]);
    uint32_t chain = (argc > 3) ? (uint32_t)atoi(argv[3]) : 0;
    uint32_t index = (argc > 4) ? (uint32_t)atoi(argv[4]) : 0;
    fprintf(stderr, "Seed: %s\n", to_hex(seed, 64).c_str());
    fprintf(stderr, "Coin type: %u\n", coin_type);
    fprintf(stderr, "Path: m/86'/%u'/0'/%u/%u\n", coin_type, chain, index);

    // BIP32 derivation: m/86'/coin_type'/0'/chain/index
    dinero::BIP32Deriver deriver(seed, 64);
    deriver.deriveHardened(86);
    deriver.deriveHardened(coin_type);
    deriver.deriveHardened(0);
    deriver.deriveNormal(chain);
    deriver.deriveNormal(index);

    auto privkey = deriver.getPrivateKey();
    auto xonly = deriver.getXOnlyPubkey();

    fprintf(stderr, "Internal privkey: %s\n", to_hex(privkey.data(), 32).c_str());
    fprintf(stderr, "Internal x-only:  %s\n", to_hex(xonly.data(), 32).c_str());

    // Compute tweaked output key (for address verification)
    std::array<uint8_t, 32> output_key;
    if (!dinero::TaprootKeys::ComputeTweakedPubkey(xonly, output_key)) {
        fprintf(stderr, "Error: ComputeTweakedPubkey failed\n");
        return 1;
    }
    fprintf(stderr, "Output key:       %s\n", to_hex(output_key.data(), 32).c_str());

    // Compute tweaked private key (same as GetTaprootPrivateKeyAt)
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    // Get parity
    secp256k1_pubkey P;
    if (!secp256k1_ec_pubkey_create(ctx, &P, privkey.data())) {
        fprintf(stderr, "Error: pubkey create failed\n");
        return 1;
    }
    secp256k1_xonly_pubkey xonly_pk;
    int pk_parity;
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly_pk, &pk_parity, &P)) {
        fprintf(stderr, "Error: xonly from pubkey failed\n");
        return 1;
    }

    // Compute TapTweak hash
    uint8_t tweak[32];
    {
        const char* tag = "TapTweak";
        uint8_t tag_hash[32];
        SHA256(reinterpret_cast<const unsigned char*>(tag), strlen(tag), tag_hash);

        dinero::crypto::CSHA256()
            .Write(tag_hash, 32)
            .Write(tag_hash, 32)
            .Write(xonly.data(), 32)
            .Finalize(tweak);
    }

    // Tweak private key
    uint8_t tweaked_privkey[32];
    memcpy(tweaked_privkey, privkey.data(), 32);

    if (pk_parity) {
        if (!secp256k1_ec_seckey_negate(ctx, tweaked_privkey)) {
            fprintf(stderr, "Error: negate failed\n");
            return 1;
        }
    }

    if (!secp256k1_ec_seckey_tweak_add(ctx, tweaked_privkey, tweak)) {
        fprintf(stderr, "Error: tweak add failed\n");
        return 1;
    }

    fprintf(stderr, "Tweaked privkey:  %s\n", to_hex(tweaked_privkey, 32).c_str());

    // Verify: compute pubkey from tweaked privkey, should match output_key
    secp256k1_pubkey tweaked_P;
    if (secp256k1_ec_pubkey_create(ctx, &tweaked_P, tweaked_privkey)) {
        secp256k1_xonly_pubkey tweaked_xonly;
        secp256k1_xonly_pubkey_from_pubkey(ctx, &tweaked_xonly, nullptr, &tweaked_P);
        uint8_t verify_key[32];
        secp256k1_xonly_pubkey_serialize(ctx, verify_key, &tweaked_xonly);
        if (memcmp(verify_key, output_key.data(), 32) == 0) {
            fprintf(stderr, "VERIFIED: tweaked privkey matches output key!\n");
        } else {
            fprintf(stderr, "WARNING: tweaked privkey does NOT match output key!\n");
            fprintf(stderr, "  Derived:  %s\n", to_hex(verify_key, 32).c_str());
            fprintf(stderr, "  Expected: %s\n", to_hex(output_key.data(), 32).c_str());
        }
    }

    // Output just the tweaked private key hex to stdout
    printf("%s\n", to_hex(tweaked_privkey, 32).c_str());

    OPENSSL_cleanse(tweaked_privkey, 32);
    OPENSSL_cleanse(seed, 64);
    secp256k1_context_destroy(ctx);

    return 0;
}

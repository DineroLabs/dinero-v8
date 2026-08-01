/*
 * treasury_decrypt_seed.c — One-time recovery tool.
 *
 * Decrypts the HD wallet seed stored in ~/.dinero/wallets/wallet_default.db
 * on Dell Tower Linux. That seed was encrypted with PBKDF2 using a BROKEN
 * hmac_sha512 (which used SHA-256 internally, not SHA-512). This tool
 * reproduces the exact broken behavior to derive the AES-256-GCM key,
 * decrypt the seed, and output the 64-byte raw seed in hex.
 *
 * The decrypted seed can then be fed to treasury_derive_key to obtain
 * the Taproot private key for sweeping.
 *
 * Build:
 *   cc -O2 -o treasury_decrypt_seed treasury_decrypt_seed.c \
 *      -I"${OPENSSL_PREFIX}/include" -L"${OPENSSL_PREFIX}/lib" -lcrypto
 *
 * Usage:
 *   ./treasury_decrypt_seed <encrypted_blob_hex_248chars>
 */

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* ───────────────────────────────────────────────────────────────────────
 * Broken HMAC-SHA512 — exact replica of the Linux bug.
 * Uses SHA-256 internally, duplicates 32 bytes to fill 64.
 * ─────────────────────────────────────────────────────────────────────── */
static void broken_hmac_sha512(const uint8_t *key, size_t keylen,
                               const uint8_t *data, size_t datalen,
                               uint8_t out64[64])
{
    unsigned int outlen = 0;
    if (keylen > INT_MAX ||
        HMAC(EVP_sha256(), key, (int)keylen, data, datalen, out64, &outlen) == NULL ||
        outlen != 32) {
        fputs("HMAC-SHA256 failed\n", stderr);
        abort();
    }

    /* Duplicate 32 bytes to fill 64  ← BUG */
    memcpy(out64 + 32, out64, 32);
}

/* ───────────────────────────────────────────────────────────────────────
 * PBKDF2 using the broken HMAC (RFC 2898 structure)
 * ─────────────────────────────────────────────────────────────────────── */
static void broken_pbkdf2(const uint8_t *password, size_t pwlen,
                          const uint8_t *salt, size_t saltlen,
                          uint32_t iterations,
                          uint8_t *output, size_t dklen)
{
    const size_t HLEN = 64;
    size_t blocks = (dklen + HLEN - 1) / HLEN;
    uint8_t *salt_block = malloc(saltlen + 4);
    memcpy(salt_block, salt, saltlen);

    for (uint32_t blk = 1; blk <= blocks; blk++) {
        /* big-endian block counter */
        salt_block[saltlen + 0] = (blk >> 24) & 0xFF;
        salt_block[saltlen + 1] = (blk >> 16) & 0xFF;
        salt_block[saltlen + 2] = (blk >> 8)  & 0xFF;
        salt_block[saltlen + 3] = blk & 0xFF;

        uint8_t U[64], T[64];
        broken_hmac_sha512(password, pwlen,
                           salt_block, saltlen + 4, U);
        memcpy(T, U, HLEN);

        for (uint32_t i = 1; i < iterations; i++) {
            broken_hmac_sha512(password, pwlen, U, HLEN, U);
            for (size_t j = 0; j < HLEN; j++)
                T[j] ^= U[j];
        }

        size_t offset = (blk - 1) * HLEN;
        size_t tocopy = dklen - offset;
        if (tocopy > HLEN) tocopy = HLEN;
        memcpy(output + offset, T, tocopy);
    }

    free(salt_block);
}

/* ─────────────────────────────────────────────────────────── helpers */
static int from_hex(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) return 0;
    for (size_t i = 0; i < out_len; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}

static void print_hex(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        printf("%02x", data[i]);
}

/* ───────────────────────────────────────────────────────────── main */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <encrypted_blob_hex_248chars>\n"
            "\n"
            "Decrypts an AES-256-GCM encrypted HD seed that was encrypted\n"
            "using the broken Linux HMAC-SHA512 (which was actually SHA-256).\n"
            "\n"
            "Blob format: salt(32) + nonce(12) + ciphertext(64) + tag(16) = 124 bytes\n",
            argv[0]);
        return 1;
    }

    /* Parse the 124-byte encrypted blob */
    const size_t BLOB_SIZE = 124;
    uint8_t blob[124];
    if (!from_hex(argv[1], blob, BLOB_SIZE)) {
        fprintf(stderr, "Error: blob must be %zu hex chars (%zu bytes)\n",
                BLOB_SIZE * 2, BLOB_SIZE);
        return 1;
    }

    const uint8_t *salt       = blob;          /* 32 bytes */
    const uint8_t *nonce      = blob + 32;     /* 12 bytes */
    const uint8_t *ciphertext = blob + 44;     /* 64 bytes */
    const uint8_t *tag        = blob + 108;    /* 16 bytes */

    fprintf(stderr, "Salt:       "); for (int i=0;i<32;i++) fprintf(stderr,"%02x",salt[i]); fprintf(stderr,"\n");
    fprintf(stderr, "Nonce:      "); for (int i=0;i<12;i++) fprintf(stderr,"%02x",nonce[i]); fprintf(stderr,"\n");
    fprintf(stderr, "Ciphertext: "); for (int i=0;i<64;i++) fprintf(stderr,"%02x",ciphertext[i]); fprintf(stderr,"\n");
    fprintf(stderr, "Tag:        "); for (int i=0;i<16;i++) fprintf(stderr,"%02x",tag[i]); fprintf(stderr,"\n");

    /* ── Step 1: Derive AES key using broken PBKDF2("", salt, 100000) ── */
    fprintf(stderr, "\nRunning broken PBKDF2 (100,000 iterations)...\n");

    uint8_t derived[64];
    const uint8_t *password = (const uint8_t *)"";
    broken_pbkdf2(password, 0, salt, 32, 100000, derived, 64);

    uint8_t aes_key[32];
    memcpy(aes_key, derived, 32);
    fprintf(stderr, "Derived AES key: "); for(int i=0;i<32;i++) fprintf(stderr,"%02x",aes_key[i]); fprintf(stderr,"\n");

    /* ── Step 2: Decrypt with AES-256-GCM ── */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        fprintf(stderr, "Error: EVP_CIPHER_CTX_new failed\n");
        return 1;
    }

    uint8_t plaintext[64];
    int len = 0, plaintext_len = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, aes_key, nonce) != 1) {
        fprintf(stderr, "Error: DecryptInit failed\n");
        return 1;
    }

    if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, 64) != 1) {
        fprintf(stderr, "Error: DecryptUpdate failed\n");
        return 1;
    }
    plaintext_len = len;

    /* Set the expected GCM tag */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag) != 1) {
        fprintf(stderr, "Error: setting GCM tag failed\n");
        return 1;
    }

    /* Finalize — this verifies the tag */
    int ret = EVP_DecryptFinal_ex(ctx, plaintext + len, &len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret <= 0) {
        fprintf(stderr, "\nERROR: GCM tag verification FAILED.\n"
                        "The broken PBKDF2 derivation did not produce the correct AES key.\n"
                        "This seed was NOT encrypted with the broken hmac_sha512.\n");
        return 1;
    }

    plaintext_len += len;

    /* ── Step 3: Output the decrypted 64-byte seed ── */
    fprintf(stderr, "\nSUCCESS: GCM tag verified! Seed decrypted.\n");
    fprintf(stderr, "Seed (%d bytes): ", plaintext_len);
    for (int i = 0; i < plaintext_len; i++) fprintf(stderr, "%02x", plaintext[i]);
    fprintf(stderr, "\n");

    /* stdout: just the hex seed for piping */
    print_hex(plaintext, plaintext_len);
    printf("\n");

    /* Scrub sensitive material */
    memset(derived, 0, sizeof(derived));
    memset(aes_key, 0, sizeof(aes_key));
    memset(plaintext, 0, sizeof(plaintext));

    return 0;
}

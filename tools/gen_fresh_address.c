/* Generate a fresh random P2TR address with correct crypto */
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Bech32m encoding (minimal) */
static const char* CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static uint32_t bech32m_polymod(const uint8_t *values, size_t len) {
    uint32_t chk = 1;
    const uint32_t GEN[] = {0x3b6a57b2,0x26508e6d,0x1ea119fa,0x3d4233dd,0x2a1462b3};
    for (size_t i = 0; i < len; i++) {
        uint32_t b = chk >> 25;
        chk = ((chk & 0x1ffffff) << 5) ^ values[i];
        for (int j = 0; j < 5; j++) if ((b >> j) & 1) chk ^= GEN[j];
    }
    return chk;
}

static int bech32m_encode(char *out, const char *hrp, const uint8_t *data5, size_t data5_len) {
    size_t hrp_len = strlen(hrp);
    /* Expand HRP */
    size_t exp_len = hrp_len * 2 + 1 + data5_len + 6;
    uint8_t *values = (uint8_t*)malloc(exp_len);
    size_t pos = 0;
    for (size_t i = 0; i < hrp_len; i++) values[pos++] = hrp[i] >> 5;
    values[pos++] = 0;
    for (size_t i = 0; i < hrp_len; i++) values[pos++] = hrp[i] & 31;
    for (size_t i = 0; i < data5_len; i++) values[pos++] = data5[i];
    for (int i = 0; i < 6; i++) values[pos++] = 0;
    uint32_t polymod = bech32m_polymod(values, pos) ^ 0x2bc830a3; /* bech32m constant */
    free(values);

    /* Build output string */
    size_t opos = 0;
    for (size_t i = 0; i < hrp_len; i++) out[opos++] = hrp[i];
    out[opos++] = '1';
    for (size_t i = 0; i < data5_len; i++) out[opos++] = CHARSET[data5[i]];
    for (int i = 0; i < 6; i++) out[opos++] = CHARSET[(polymod >> (5*(5-i))) & 31];
    out[opos] = '\0';
    return 1;
}

/* Convert 8-bit to 5-bit groups */
static int convert_bits(uint8_t *out, size_t *outlen, int tob, const uint8_t *in, size_t inlen, int fromb, int pad) {
    uint32_t acc = 0; int bits = 0;
    *outlen = 0;
    for (size_t i = 0; i < inlen; i++) {
        acc = (acc << fromb) | in[i];
        bits += fromb;
        while (bits >= tob) {
            bits -= tob;
            out[(*outlen)++] = (acc >> bits) & ((1 << tob) - 1);
        }
    }
    if (pad && bits) out[(*outlen)++] = (acc << (tob - bits)) & ((1 << tob) - 1);
    return 1;
}

int main(void) {
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    /* Generate random private key */
    uint8_t privkey[32];
    do { RAND_bytes(privkey, 32); } while (!secp256k1_ec_seckey_verify(ctx, privkey));

    /* Get x-only pubkey (internal key) */
    secp256k1_keypair kp;
    secp256k1_keypair_create(ctx, &kp, privkey);
    secp256k1_xonly_pubkey xonly;
    secp256k1_keypair_xonly_pub(ctx, &xonly, NULL, &kp);
    uint8_t internal_key[32];
    secp256k1_xonly_pubkey_serialize(ctx, internal_key, &xonly);

    /* BIP341 TapTweak: output = internal + H("TapTweak", internal)*G */
    uint8_t tag_hash[32];
    SHA256((uint8_t*)"TapTweak", 8, tag_hash);
    SHA256_CTX sha;
    uint8_t tweak[32];
    SHA256_Init(&sha);
    SHA256_Update(&sha, tag_hash, 32);
    SHA256_Update(&sha, tag_hash, 32);
    SHA256_Update(&sha, internal_key, 32);
    SHA256_Final(tweak, &sha);

    /* Tweaked output key */
    secp256k1_pubkey P;
    secp256k1_ec_pubkey_create(ctx, &P, privkey);
    secp256k1_xonly_pubkey xpk; int parity;
    secp256k1_xonly_pubkey_from_pubkey(ctx, &xpk, &parity, &P);

    uint8_t tweaked_priv[32];
    memcpy(tweaked_priv, privkey, 32);
    if (parity) secp256k1_ec_seckey_negate(ctx, tweaked_priv);
    secp256k1_ec_seckey_tweak_add(ctx, tweaked_priv, tweak);

    secp256k1_pubkey tP;
    secp256k1_ec_pubkey_create(ctx, &tP, tweaked_priv);
    secp256k1_xonly_pubkey txo;
    secp256k1_xonly_pubkey_from_pubkey(ctx, &txo, NULL, &tP);
    uint8_t output_key[32];
    secp256k1_xonly_pubkey_serialize(ctx, output_key, &txo);

    /* Encode as bech32m P2TR address */
    uint8_t data5[64]; size_t data5_len;
    data5[0] = 1; /* witness version 1 */
    convert_bits(data5 + 1, &data5_len, 5, output_key, 32, 8, 1);
    data5_len += 1;

    char address[128];
    bech32m_encode(address, "din", data5, data5_len);

    /* Output results */
    fprintf(stderr, "=== Fresh P2TR Address (correct crypto) ===\n");
    fprintf(stderr, "Internal privkey: ");
    for(int i=0;i<32;i++) fprintf(stderr,"%02x",privkey[i]);
    fprintf(stderr,"\n");
    fprintf(stderr, "Internal xonly:   ");
    for(int i=0;i<32;i++) fprintf(stderr,"%02x",internal_key[i]);
    fprintf(stderr,"\n");
    fprintf(stderr, "Tweaked privkey:  ");
    for(int i=0;i<32;i++) fprintf(stderr,"%02x",tweaked_priv[i]);
    fprintf(stderr,"\n");
    fprintf(stderr, "Output key:       ");
    for(int i=0;i<32;i++) fprintf(stderr,"%02x",output_key[i]);
    fprintf(stderr,"\n");
    fprintf(stderr, "Address:          %s\n", address);

    /* stdout: just the address */
    printf("%s\n", address);

    memset(privkey, 0, 32);
    memset(tweaked_priv, 0, 32);
    secp256k1_context_destroy(ctx);
    return 0;
}

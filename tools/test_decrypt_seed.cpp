// Standalone seed blob decryption test
// Usage: test_decrypt_seed [passphrase]
// Tries to decrypt the Tower's seed blob with given passphrase (default: "")

#include "crypto/pbkdf2.h"
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

static bool from_hex(const char* hex, std::vector<uint8_t>& out) {
    size_t len = strlen(hex);
    if (len % 2 != 0) return false;
    out.resize(len / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned int val;
        if (sscanf(hex + i * 2, "%2x", &val) != 1) return false;
        out[i] = (uint8_t)val;
    }
    return true;
}

int main(int argc, char* argv[]) {
    // Tower's encrypted seed blob (from hd_seeds table)
    const char* blob_hex =
        "6DFADCECEBFD63D4F87C50ABB27EC3D8CC7A2EE13CC361A222631977829E237A"
        "6254FC446F90B4B6586442A2B7D6C5FBAEB55B147EACC6C624FD2794C31D5C9B"
        "DE96C6BA43ED92EEAE45A276072AD1C9C7D33ABB53A99EB94B2C5A3F177869A2"
        "49F2E832690CED03F18F3151111BDDA784052FE715220DCB5C32044D";

    std::vector<uint8_t> blob;
    if (!from_hex(blob_hex, blob)) {
        fprintf(stderr, "Failed to parse blob hex\n");
        return 1;
    }
    fprintf(stderr, "Blob size: %zu bytes\n", blob.size());

    std::string passphrase = (argc > 1) ? argv[1] : "";
    fprintf(stderr, "Passphrase: \"%s\" (length=%zu)\n", passphrase.c_str(), passphrase.size());

    constexpr size_t SALT_SIZE = 32;
    constexpr size_t NONCE_SIZE = 12;
    constexpr size_t TAG_SIZE = 16;

    std::vector<uint8_t> salt(blob.begin(), blob.begin() + SALT_SIZE);
    std::vector<uint8_t> nonce(blob.begin() + SALT_SIZE, blob.begin() + SALT_SIZE + NONCE_SIZE);
    size_t ct_len = blob.size() - SALT_SIZE - NONCE_SIZE - TAG_SIZE;
    std::vector<uint8_t> ciphertext(blob.begin() + SALT_SIZE + NONCE_SIZE,
                                     blob.begin() + SALT_SIZE + NONCE_SIZE + ct_len);
    std::vector<uint8_t> tag(blob.end() - TAG_SIZE, blob.end());

    fprintf(stderr, "Salt:       %zu bytes\n", salt.size());
    fprintf(stderr, "Nonce:      %zu bytes\n", nonce.size());
    fprintf(stderr, "Ciphertext: %zu bytes\n", ciphertext.size());
    fprintf(stderr, "Tag:        %zu bytes\n", tag.size());

    uint32_t iterations_list[] = {600000, 100000, 200000, 300000, 10000, 50000};
    int num_iters = sizeof(iterations_list) / sizeof(iterations_list[0]);

    for (int i = 0; i < num_iters; ++i) {
        uint32_t iterations = iterations_list[i];
        fprintf(stderr, "\nTrying %u iterations...\n", iterations);

        uint8_t derived_key[64];
        dinero::crypto::PBKDF2_HMAC_SHA512(
            reinterpret_cast<const uint8_t*>(passphrase.data()), passphrase.size(),
            salt.data(), salt.size(),
            iterations,
            derived_key, sizeof(derived_key)
        );

        uint8_t aes_key[32];
        memcpy(aes_key, derived_key, 32);

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) continue;

        std::vector<uint8_t> plaintext(ct_len);
        bool ok = false;

        do {
            if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, aes_key, nonce.data()) != 1) break;
            int len = 0;
            if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ct_len) != 1) break;
            int pt_len = len;
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE,
                                    const_cast<uint8_t*>(tag.data())) != 1) break;
            if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) break;
            pt_len += len;
            plaintext.resize(pt_len);
            ok = true;
        } while (false);

        EVP_CIPHER_CTX_free(ctx);
        OPENSSL_cleanse(derived_key, sizeof(derived_key));
        OPENSSL_cleanse(aes_key, sizeof(aes_key));

        if (ok) {
            fprintf(stderr, "SUCCESS with %u iterations! Seed size: %zu bytes\n",
                    iterations, plaintext.size());
            // Print seed hex to stdout
            for (uint8_t b : plaintext) printf("%02x", b);
            printf("\n");
            OPENSSL_cleanse(plaintext.data(), plaintext.size());
            return 0;
        } else {
            fprintf(stderr, "  Failed (tag mismatch)\n");
        }
    }

    fprintf(stderr, "\nAll iteration counts failed.\n");
    return 1;
}

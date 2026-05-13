/*
 * AES-256-GCM encryption for v7 wallet seeds.
 * See include/wallet/aead_seed.h.
 */

#include "wallet/aead_seed.h"

#include <cstring>
#include <stdexcept>
#include <string>

#include <openssl/crypto.h>   // OPENSSL_cleanse
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace dinero::wallet {

namespace {

[[noreturn]] void ThrowOpenssl(const char* where) {
    throw std::runtime_error(std::string("aead_seed: OpenSSL failure at ") + where);
}

struct EvpCtx {
    EVP_CIPHER_CTX* ctx;
    EvpCtx() : ctx(EVP_CIPHER_CTX_new()) {
        if (!ctx) ThrowOpenssl("EVP_CIPHER_CTX_new");
    }
    ~EvpCtx() {
        if (ctx) EVP_CIPHER_CTX_free(ctx);
    }
    EvpCtx(const EvpCtx&) = delete;
    EvpCtx& operator=(const EvpCtx&) = delete;
};

} // namespace

AeadSealOutput SealSeed(const AeadSeed& plaintext, const AeadKey& key) {
    AeadSealOutput out{};
    if (RAND_bytes(out.nonce.data(), static_cast<int>(out.nonce.size())) != 1) {
        ThrowOpenssl("RAND_bytes");
    }

    EvpCtx ctx;
    if (EVP_EncryptInit_ex(ctx.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        ThrowOpenssl("EVP_EncryptInit_ex (cipher)");
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.ctx, EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(out.nonce.size()), nullptr) != 1) {
        ThrowOpenssl("EVP_CIPHER_CTX_ctrl IVLEN");
    }
    if (EVP_EncryptInit_ex(ctx.ctx, nullptr, nullptr, key.data(), out.nonce.data()) != 1) {
        ThrowOpenssl("EVP_EncryptInit_ex (key+iv)");
    }

    int outlen = 0;
    if (EVP_EncryptUpdate(ctx.ctx, out.ciphertext.data(), &outlen,
                          plaintext.data(), static_cast<int>(plaintext.size())) != 1) {
        ThrowOpenssl("EVP_EncryptUpdate");
    }
    if (static_cast<std::size_t>(outlen) != plaintext.size()) {
        ThrowOpenssl("EncryptUpdate size mismatch");
    }

    int finlen = 0;
    if (EVP_EncryptFinal_ex(ctx.ctx, out.ciphertext.data() + outlen, &finlen) != 1) {
        ThrowOpenssl("EVP_EncryptFinal_ex");
    }
    if (finlen != 0) {
        ThrowOpenssl("EncryptFinal unexpected tail bytes (GCM should emit 0)");
    }

    if (EVP_CIPHER_CTX_ctrl(ctx.ctx, EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(out.tag.size()),
                            out.tag.data()) != 1) {
        ThrowOpenssl("EVP_CIPHER_CTX_ctrl GET_TAG");
    }
    return out;
}

void SecureSeed::Wipe() noexcept {
    OPENSSL_cleanse(bytes_.data(), bytes_.size());
}

dinero::consensus::pq::ml_dsa_65::Seed SecureSeed::ToMlDsaSeed() const noexcept {
    dinero::consensus::pq::ml_dsa_65::Seed out{};
    std::memcpy(out.data(), bytes_.data(), out.size());
    return out;
}

AeadOpenResult OpenSeedSecure(const AeadCiphertext& ciphertext,
                              const AeadNonce&      nonce,
                              const AeadTag&        tag,
                              const AeadKey&        key,
                              SecureSeed*           out) {
    if (out == nullptr) return AeadOpenResult::InternalError;
    return OpenSeed(ciphertext, nonce, tag, key, &out->mutable_bytes());
}

AeadOpenResult OpenSeed(const AeadCiphertext& ciphertext,
                        const AeadNonce&      nonce,
                        const AeadTag&        tag,
                        const AeadKey&        key,
                        AeadSeed*             out) {
    if (out == nullptr) return AeadOpenResult::InternalError;

    EvpCtx ctx;
    if (EVP_DecryptInit_ex(ctx.ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        return AeadOpenResult::InternalError;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.ctx, EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1) {
        return AeadOpenResult::InternalError;
    }
    if (EVP_DecryptInit_ex(ctx.ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
        return AeadOpenResult::InternalError;
    }

    AeadSeed tmp{};
    int outlen = 0;
    if (EVP_DecryptUpdate(ctx.ctx, tmp.data(), &outlen,
                          ciphertext.data(), static_cast<int>(ciphertext.size())) != 1) {
        OPENSSL_cleanse(tmp.data(), tmp.size());
        return AeadOpenResult::InternalError;
    }

    // GCM requires the tag be set before Final.
    if (EVP_CIPHER_CTX_ctrl(ctx.ctx, EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(tag.size()),
                            const_cast<uint8_t*>(tag.data())) != 1) {
        OPENSSL_cleanse(tmp.data(), tmp.size());
        return AeadOpenResult::InternalError;
    }

    int finlen = 0;
    int rc = EVP_DecryptFinal_ex(ctx.ctx, tmp.data() + outlen, &finlen);
    if (rc != 1) {
        // Tag mismatch. Scrub anything we may have decrypted.
        OPENSSL_cleanse(tmp.data(), tmp.size());
        return AeadOpenResult::AuthFailed;
    }

    std::memcpy(out->data(), tmp.data(), tmp.size());
    OPENSSL_cleanse(tmp.data(), tmp.size());
    return AeadOpenResult::Ok;
}

} // namespace dinero::wallet

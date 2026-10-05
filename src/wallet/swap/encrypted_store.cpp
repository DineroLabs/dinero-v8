#include "wallet/swap/encrypted_store.h"

#include "crypto/secure_random.h"
#include "crypto/sha256.h"
#include "crypto/wallet_crypto.h"
#include "wallet/swap/swap_crypto.h"

#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace dinero::swap {
namespace {

constexpr char kPrefix[] = "dinswap1e";
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;

}  // namespace

Bytes32 DeriveSwapStoreKey(const Bytes32& wallet_master_key) {
    return detail::HmacSha256(wallet_master_key, "dinero/swap-store/v1");
}

EncryptedFileSwapStore::EncryptedFileSwapStore(std::string path, const Bytes32& key)
    : path_(std::move(path)), key_(key) {}

void SealToFile(const std::string& path, const Bytes32& key, const std::string& plain) {
    const auto nonce_bytes = secure_random_bytes(kNonceSize);
    const std::vector<uint8_t> nonce(nonce_bytes.begin(), nonce_bytes.end());
    const auto sealed = crypto::encryptAesGcm(std::vector<uint8_t>(plain.begin(), plain.end()), key, nonce);
    std::vector<uint8_t> blob = nonce;
    blob.insert(blob.end(), sealed.begin(), sealed.end());
    detail::WriteFileAtomically(path, kPrefix + detail::ToHex(blob) + "\n");
}

std::string OpenSealedFile(const std::string& path, const Bytes32& key) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("swap store: cannot read " + path);
    std::string text;
    std::getline(in, text);
    const std::string prefix(kPrefix);
    if (text.compare(0, prefix.size(), prefix) != 0) throw std::runtime_error("swap store: not an encrypted file");
    std::vector<uint8_t> blob;
    try {
        blob = detail::FromHex(text.substr(prefix.size()));
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("swap store: corrupted file");
    }
    if (blob.size() < kNonceSize + kTagSize + 1) throw std::runtime_error("swap store: corrupted file");
    const std::vector<uint8_t> nonce(blob.begin(), blob.begin() + kNonceSize);
    const std::vector<uint8_t> sealed(blob.begin() + kNonceSize, blob.end());
    try {
        const auto plain = crypto::decryptAesGcm(sealed, key, nonce);
        return std::string(plain.begin(), plain.end());
    } catch (const std::exception&) {
        throw std::runtime_error("swap store: wrong key or tampered file");
    }
}

void EncryptedFileSwapStore::Save(const SwapSession& session) { SealToFile(path_, key_, EncodeSession(session)); }

SwapSession EncryptedFileSwapStore::Load(const std::string& path, const Bytes32& key) {
    try {
        return DecodeSession(OpenSealedFile(path, key));
    } catch (const std::invalid_argument& e) {
        throw std::runtime_error(std::string("swap store: ") + e.what());
    }
}

}  // namespace dinero::swap

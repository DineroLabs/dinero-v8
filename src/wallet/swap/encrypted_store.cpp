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

Bytes32 HmacSha256(const Bytes32& key, const std::string& msg) {
    std::array<uint8_t, 64> block{};
    std::copy(key.begin(), key.end(), block.begin());  // 32-byte key fits the 64-byte block
    std::array<uint8_t, 64> ipad{}, opad{};
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] = block[i] ^ 0x36;
        opad[i] = block[i] ^ 0x5c;
    }
    Bytes32 inner{}, out{};
    crypto::CSHA256().Write(ipad.data(), ipad.size())
        .Write(reinterpret_cast<const uint8_t*>(msg.data()), msg.size())
        .Finalize(inner.data());
    crypto::CSHA256().Write(opad.data(), opad.size()).Write(inner.data(), inner.size()).Finalize(out.data());
    return out;
}

void WriteAtomically(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("swap store: cannot open " + tmp);
    size_t off = 0;
    while (off < text.size()) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n <= 0) { ::close(fd); throw std::runtime_error("swap store: write failed"); }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) { ::close(fd); throw std::runtime_error("swap store: fsync failed"); }
    ::close(fd);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) throw std::runtime_error("swap store: rename failed");
}

}  // namespace

Bytes32 DeriveSwapStoreKey(const Bytes32& wallet_master_key) {
    return HmacSha256(wallet_master_key, "dinero/swap-store/v1");
}

EncryptedFileSwapStore::EncryptedFileSwapStore(std::string path, const Bytes32& key)
    : path_(std::move(path)), key_(key) {}

void EncryptedFileSwapStore::Save(const SwapSession& session) {
    const std::string plain = EncodeSession(session);
    const auto nonce_bytes = secure_random_bytes(kNonceSize);
    const std::vector<uint8_t> nonce(nonce_bytes.begin(), nonce_bytes.end());
    const auto sealed = crypto::encryptAesGcm(std::vector<uint8_t>(plain.begin(), plain.end()), key_, nonce);
    std::vector<uint8_t> blob = nonce;
    blob.insert(blob.end(), sealed.begin(), sealed.end());
    WriteAtomically(path_, kPrefix + detail::ToHex(blob) + "\n");
}

SwapSession EncryptedFileSwapStore::Load(const std::string& path, const Bytes32& key) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("swap store: cannot read " + path);
    std::string text;
    std::getline(in, text);
    const std::string prefix(kPrefix);
    if (text.compare(0, prefix.size(), prefix) != 0) throw std::runtime_error("swap store: not an encrypted session");
    std::vector<uint8_t> blob;
    try {
        blob = detail::FromHex(text.substr(prefix.size()));
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("swap store: corrupted file");
    }
    if (blob.size() < kNonceSize + kTagSize + 1) throw std::runtime_error("swap store: corrupted file");
    const std::vector<uint8_t> nonce(blob.begin(), blob.begin() + kNonceSize);
    const std::vector<uint8_t> sealed(blob.begin() + kNonceSize, blob.end());
    std::vector<uint8_t> plain;
    try {
        plain = crypto::decryptAesGcm(sealed, key, nonce);
    } catch (const std::exception&) {
        throw std::runtime_error("swap store: wrong key or tampered file");
    }
    try {
        return DecodeSession(std::string(plain.begin(), plain.end()));
    } catch (const std::invalid_argument& e) {
        throw std::runtime_error(std::string("swap store: ") + e.what());
    }
}

}  // namespace dinero::swap

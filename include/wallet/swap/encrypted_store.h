#pragma once
// Encrypted swap store: the production SwapStore. The whole SwapSession
// (Alice's secret, the terms, amounts and payout scripts) is sealed with
// AES-256-GCM under a key derived from the wallet's unlocked master key, with
// a fresh random nonce on every save. File form:
//   "dinswap1e" + hex(nonce[12] || ciphertext || tag[16])
// written atomically (temp + fsync + rename). A wrong key or any modified
// byte is refused on load.

#include "wallet/swap/runner.h"

#include <string>

namespace dinero::swap {

// HMAC-SHA256(master_key, "dinero/swap-store/v1"): a dedicated subkey, so the
// wallet master key itself never encrypts swap data.
Bytes32 DeriveSwapStoreKey(const Bytes32& wallet_master_key);

// Any text sealed the same way (pending offers use it too).
void SealToFile(const std::string& path, const Bytes32& key, const std::string& plaintext);
std::string OpenSealedFile(const std::string& path, const Bytes32& key);  // throws std::runtime_error

class EncryptedFileSwapStore : public SwapStore {
public:
    EncryptedFileSwapStore(std::string path, const Bytes32& key);
    void Save(const SwapSession& session) override;
    static SwapSession Load(const std::string& path, const Bytes32& key);  // throws std::runtime_error

private:
    std::string path_;
    Bytes32 key_;
};

}  // namespace dinero::swap

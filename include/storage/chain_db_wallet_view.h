// ChainDB-backed implementation of the wallet's chain view (Task 5.2).
//
// The wallet rescans through wallet::IWalletChainView; this adapter is the
// node side of that interface. It also implements ChainHeightProvider, so
// hd_wallet maturity checks can share the same object
// (HDWallet::ConnectChainHeightProvider).
//
// ReadBlock goes through storage::ReadArchivalBlock(db, block_storage, hash)
// exactly as the wallet's legacy rescan path did; block_storage is nullable,
// in which case only the ChainDB-stored body is consulted.
#pragma once

#include "storage/chain_height_provider.h"
#include "wallet/node_interfaces.h"

#include <optional>
#include <string>

namespace dinero {
class ChainDB;
class BlockStorage;
}

namespace dinero::storage {

class ChainDbWalletView final : public wallet::IWalletChainView,
                                public ChainHeightProvider {
public:
    ChainDbWalletView(ChainDB& db, BlockStorage* block_storage /* nullable */)
        : db_(db), block_storage_(block_storage) {}

    // wallet::IWalletChainView
    std::optional<wallet::WalletChainTip> GetTip() const override;
    std::optional<uint256> GetBlockHashAtHeight(int32_t height) const override;
    std::optional<Block> ReadBlock(const uint256& hash) const override;

    // ChainHeightProvider
    uint32_t GetBestHeight() const override;
    std::string GetBestHash() const override;
    double GetDifficulty() const override;
    ::Json::Value GetBlockHeader(const std::string& hash) const override;
    bool IsAvailable() const override { return true; }

private:
    ChainDB& db_;
    BlockStorage* block_storage_;  // nullable
};

} // namespace dinero::storage

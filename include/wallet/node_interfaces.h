// Wallet-side interfaces to node state (wallet/node separation Task 5.1).
//
// The wallet observes the chain and the mempool ONLY through these
// interfaces. The node side implements them (ChainDbWalletView, Task 5.2;
// MempoolWalletView, Task 5.3); tests use the fakes in
// tests/wallet/fake_node_views.h.
//
// Include discipline (plan Task 5.1 Step 3): this header may include only
// primitives/*, consensus/outpoint.h, <optional>, <vector> and <stdexcept>.
#pragma once

#include "consensus/outpoint.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dinero::wallet {

struct WalletChainTip {
    int32_t height;
    uint256 hash;
};

class IWalletChainView {
public:
    virtual ~IWalletChainView() = default;
    virtual std::optional<WalletChainTip> GetTip() const = 0;
    virtual std::optional<uint256> GetBlockHashAtHeight(int32_t height) const = 0;
    virtual std::optional<Block> ReadBlock(const uint256& hash) const = 0;
};

// Mempool entry as the wallet sees it. Field types mirror
// daemon MempoolEntry (fee, tx_size, depends). Orchard transactions carry
// no historical transparent body; vin() then throws, with the exact
// message wallet code already uses.
class WalletMempoolEntry {
public:
    uint64_t fee = 0;
    size_t tx_size = 0;
    std::vector<uint256> depends;

    const std::vector<TxInput>& vin() const {
        if (!historical_vin_.has_value())
            throw std::logic_error("Orchard transaction has no historical body");
        return *historical_vin_;
    }

    void SetHistoricalVin(std::vector<TxInput> vin) {
        historical_vin_ = std::move(vin);
    }

private:
    std::optional<std::vector<TxInput>> historical_vin_;
};

// Stats mirror daemon Mempool::MempoolStats field types.
struct WalletMempoolStats {
    size_t total_size = 0;
    size_t tx_count = 0;
    double avg_fee_rate = 0.0;
};

class IWalletMempoolView {
public:
    virtual ~IWalletMempoolView() = default;
    virtual bool IsOutputSpent(const OutPoint& out) const = 0;
    virtual std::optional<WalletMempoolEntry> GetEntry(const uint256& txid) const = 0;
    virtual WalletMempoolStats GetStats() const = 0;
};

} // namespace dinero::wallet

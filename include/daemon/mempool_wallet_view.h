// Mempool-backed implementation of the wallet's mempool view (Task 5.3).
//
// The wallet observes the mempool only through wallet::IWalletMempoolView;
// this adapter is the node side of that interface. GetEntry copies the
// fields the wallet consumes (fee, tx_size, depends) and attaches the
// historical transparent inputs only when the transaction has them —
// an Orchard entry's vin() then throws exactly as the legacy
// MempoolTransaction::Historical() path did.
#pragma once

#include "daemon/mempool.h"
#include "wallet/node_interfaces.h"

#include <optional>

namespace dinero {

class MempoolWalletView final : public wallet::IWalletMempoolView {
public:
    explicit MempoolWalletView(const Mempool& m) : mempool_(m) {}

    bool IsOutputSpent(const OutPoint& out) const override {
        return mempool_.isOutputSpentInMempool(out);
    }

    std::optional<wallet::WalletMempoolEntry> GetEntry(const uint256& txid) const override {
        auto entry = mempool_.getMempoolEntry(txid);
        if (!entry) return std::nullopt;
        return MapEntry(*entry, entry->tx.IsOrchard());
    }

    // The whole conversion, separable so tests can drive it with real
    // MempoolEntry values for both branch directions (an actual Orchard
    // envelope needs the backend; is_orchard carries that fact instead).
    static wallet::WalletMempoolEntry MapEntry(const MempoolEntry& entry,
                                               bool is_orchard) {
        wallet::WalletMempoolEntry out;
        out.fee = entry.fee;
        out.tx_size = entry.tx_size;
        out.depends = entry.depends;
        if (!is_orchard)
            out.SetHistoricalVin(entry.tx.Historical().vin);
        return out;
    }

    wallet::WalletMempoolStats GetStats() const override {
        const auto stats = mempool_.getStats();
        wallet::WalletMempoolStats out;
        out.total_size = stats.total_size;
        out.tx_count = stats.tx_count;
        out.avg_fee_rate = stats.avg_fee_rate;
        return out;
    }

private:
    const Mempool& mempool_;
};

} // namespace dinero

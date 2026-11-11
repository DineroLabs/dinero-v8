#pragma once

#include "daemon/mempool_chain_view.h"
#include "storage/chain_db.h"

namespace dinero {

// Node-side adapter. ChainDB outlives this adapter, which outlives the pool.
// Each read is the original single database operation, with no new state or lock.
class ChainDbMempoolView final : public IMempoolChainView {
public:
    explicit ChainDbMempoolView(const ChainDB* db) : db_(db) {}
    StatusOr<MempoolChainTip> GetTip() const override {
        if (!db_) return Status::Internal;
        const auto result = db_->getTip();
        if (!result.ok()) return result.status();
        return MempoolChainTip{result.value().height, result.value().hash};
    }
    StatusOr<MempoolChainCoin> GetCoinWithConfidentialFallback(
        const uint256& txid, uint32_t vout) const override {
        if (!db_) return Status::Internal;
        const auto result = db_->getCoinWithConfidentialFallback(txid, vout);
        if (!result.ok()) return result.status();
        const auto& c = result.value();
        return MempoolChainCoin{c.amount, c.script_pubkey, c.height, c.coinbase,
            c.is_confidential, c.commitment};
    }
    bool HaveTransaction(const uint256& txid) const override {
        return db_ && db_->getTxLocation(txid).status() == Status::Ok;
    }
    bool HaveConfidentialCommitment(const std::vector<uint8_t>& commitment) const override {
        if (!db_ || commitment.empty()) return false;
        bool found = false;
        db_->forEachUTXO([&](const uint256&, uint32_t, const Coin& coin) {
            if (coin.is_confidential && coin.commitment == commitment) {
                found = true;
                return false;
            }
            return true;
        });
        return found;
    }
private:
    const ChainDB* db_;
};
} // namespace dinero

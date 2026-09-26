#pragma once
#include "daemon/runtime_block_outbox.h"
#include "primitives/block.h"
#include <map>
#include <memory>

namespace dinero {
class ChainstateService;
class RuntimeOrdinaryDelivery;
// Immutable facts for the currently known ordinary script domain at the
// checked outbox origin. Not a receipt, complete key discovery or adoption.
class RuntimeWalletOriginProjection final {
public:
    struct Spend { TxId txid; uint32_t height; uint64_t time; };
    struct Coin {
        TxOutput output;
        uint32_t height;
        uint64_t time;
        bool coinbase;
        std::optional<Spend> spent;
    };
    struct History {
        Transaction transaction; // exact relevant transaction, never a shell
        uint32_t height;
        uint64_t time;
        uint64_t credited, debited; // owned transparent amounts in una
    };
    const auto& Coins() const { return coins_; }
    const auto& Transactions() const { return history_; }
    const RuntimeOutboxCursor& FirstCursor() const { return first_.cursor; }
    const uint256& OriginHash() const { return first_.context.parent_hash; }
    uint32_t OriginHeight() const { return first_.context.height - 1; }
private:
    friend class ChainstateService;
    friend class RuntimeOrdinaryDelivery;
    friend struct RuntimeOriginProjectionTestAccess;
    RuntimeWalletOriginProjection() = default;
    void AppendValidated(const Block&,uint32_t);
    std::string identity_;
    uint64_t session_ = 0;
    uint256 scripts_digest_;
    std::map<std::vector<uint8_t>,std::string> scripts_;
    RuntimeOutboxEvent first_;
    std::map<TxOutPoint,Coin> coins_;
    std::vector<History> history_;
    size_t retained_bytes_ = 0;
};
} // namespace dinero

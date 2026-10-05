#pragma once
#include "daemon/runtime_block_outbox.h"
#include "primitives/block.h"
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace dinero {
class ChainstateService;
class RuntimeOrdinaryDelivery;
class RuntimeAccountReplay;
class RuntimeWalletCoverageProjection;
// Immutable facts for the captured ordinary and optional index script domains at the
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
    friend class RuntimeIndexDelivery;
    friend class RuntimeWalletCoverageProjection;
    friend struct RuntimeOriginProjectionTestAccess;
    RuntimeWalletOriginProjection() = default;
    void AppendValidated(const Block&,uint32_t);
    std::string identity_;
    uint64_t session_ = 0;
    bool existing_identity_only_ = false;
    uint256 scripts_digest_;
    std::map<std::vector<uint8_t>,std::string> scripts_;
    // Authenticated public predecessor ownership, separate from HD paths.
    // Captured/rechecked in the same wallet transaction as the script domain.
    std::map<std::vector<uint8_t>,std::string> historical_scripts_;
    std::optional<std::map<std::vector<uint8_t>,std::string>> index_scripts_;
    // Previous live authority is rechecked independently of the captured target.
    std::optional<std::map<std::vector<uint8_t>,std::string>> index_historical_scripts_;
    std::string index_path_;
    RuntimeOutboxEvent first_;
    std::map<TxOutPoint,Coin> coins_;
    std::vector<History> history_;
    size_t retained_bytes_ = 0;
};

// Immutable transparent facts for the captured script union through one exact
// canonical outbox head. Built from independently replayed origin history and
// the service's checked, owned runtime replay. This is source material for
// reconciliation, not a wallet receipt or key/account completeness certificate.
class RuntimeWalletCoverageProjection final {
public:
    using Coin = RuntimeWalletOriginProjection::Coin;
    struct Activity {
        TxId txid;
        uint32_t height;
        uint64_t time;
        bool coinbase;
        std::vector<TxOutPoint> inputs;
        std::vector<TxOutput> outputs;
    };
    const auto& Coins() const { return coins_; }
    const auto& Transactions() const { return history_; }
    // Every retained version comes from the checked origin or an actual
    // ordered runtime event, including branches later disconnected.
    const auto& ObservedCoins() const { return observed_coins_; }
    const auto& ObservedTransactions() const { return observed_history_; }
    const RuntimeOutboxCursor& Head() const { return head_; }
    const uint256& TipHash() const { return tip_hash_; }
    uint32_t TipHeight() const { return tip_height_; }
    const RuntimeWalletOriginProjection& Origin() const { return *origin_; }
    // Immutable exact-prefix facts collected while Build verifies every event.
    // This lookup never touches the replay source or any wallet/index owner.
    const uint256& ProfileDigest() const { return profile_digest_; }
    std::optional<std::pair<uint256,uint32_t>> PrefixTip(RuntimeOutboxCursor cursor) const;

private:
    friend class ChainstateService;
    friend class RuntimeOrdinaryDelivery;
    friend class RuntimeIndexDelivery;
    RuntimeWalletCoverageProjection() = default;
    static std::shared_ptr<const RuntimeWalletCoverageProjection> Build(
        std::shared_ptr<const RuntimeWalletOriginProjection>,
        std::shared_ptr<const RuntimeAccountReplay>);
    std::shared_ptr<const RuntimeWalletOriginProjection> origin_;
    std::shared_ptr<const RuntimeAccountReplay> replay_;
    std::map<TxOutPoint,Coin> coins_;
    std::map<TxId,Activity> history_;
    std::map<TxOutPoint,std::vector<Coin>> observed_coins_;
    std::map<TxId,std::vector<Activity>> observed_history_;
    struct PrefixFact { RuntimeOutboxCursor cursor; uint256 hash; uint32_t height; };
    std::vector<PrefixFact> prefixes_;
    uint256 profile_digest_;
    RuntimeOutboxCursor head_;
    uint256 tip_hash_;
    uint32_t tip_height_ = 0;
};
} // namespace dinero

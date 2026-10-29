#pragma once
#include "daemon/mempool.h"
#include "daemon/tx_relay_manager.h"
#include "network/bridge_node.h"
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace dinero {
// One prepared update for typed pool conflict effects and its proof caches.
// Caller holds selected-chain ownership and keeps the pool alive through this
// object's lifetime. This does not own service shutdown or other consumers.
// Lock order: selected chain -> pool -> bridge cache -> relay refresh.
class PreparedPoolTip final {
public:
    static std::unique_ptr<PreparedPoolTip> Connect(
        Mempool& pool, std::shared_ptr<network::BridgeNode> bridge,
        std::shared_ptr<TxRelayManager> relay, const Block& block,
        uint32_t height, const std::vector<uint8_t>& root) {
        ConnectedBlockEffects effects;
        effects.confirmed_txids.reserve(block.vtx.size());
        for (const auto& tx : block.vtx) {
            effects.confirmed_txids.push_back(tx.GetTxid().AsUint256());
            if (!tx.IsCoinbase()) for (const auto& input : tx.vin)
                effects.spent_transparent_inputs.emplace_back(input.prevout.txid,input.prevout.vout);
        }
        return ConnectEffects(pool,std::move(bridge),std::move(relay),effects,height,root);
    }
    // The caller owns exact validated block effects and selected-chain/service
    // lifetime. Preparation is reversible; publish only after canonical commit.
    // This accepts both transaction families without Historical conversion.
    static std::unique_ptr<PreparedPoolTip> ConnectEffects(
        Mempool& pool, std::shared_ptr<network::BridgeNode> bridge,
        std::shared_ptr<TxRelayManager> relay, const ConnectedBlockEffects& effects,
        uint32_t height, const std::vector<uint8_t>& root) {
        auto result=std::unique_ptr<PreparedPoolTip>(new PreparedPoolTip(std::move(bridge),std::move(relay)));
        result->pool_=pool.prepareBlockConnected(effects,height,root,result->Policy());
        result->PrepareCaches();
        return result;
    }
    static std::unique_ptr<PreparedPoolTip> Disconnect(
        Mempool& pool, std::shared_ptr<network::BridgeNode> bridge,
        std::shared_ptr<TxRelayManager> relay, uint32_t height) {
        auto result=std::unique_ptr<PreparedPoolTip>(new PreparedPoolTip(std::move(bridge),std::move(relay)));
        result->pool_=pool.prepareBlockDisconnected(height,result->Policy());
        result->PrepareCaches();
        return result;
    }
    static std::unique_ptr<PreparedPoolTip> DisconnectToParent(
        Mempool& pool, std::shared_ptr<network::BridgeNode> bridge,
        std::shared_ptr<TxRelayManager> relay, uint32_t height,
        const std::vector<uint8_t>& parent_root) {
        auto result=std::unique_ptr<PreparedPoolTip>(new PreparedPoolTip(std::move(bridge),std::move(relay)));
        result->pool_=pool.prepareBlockDisconnectedToParent(height,parent_root,result->Policy());
        result->PrepareCaches();
        return result;
    }
    ~PreparedPoolTip()=default;
    PreparedPoolTip(const PreparedPoolTip&)=delete;
    PreparedPoolTip& operator=(const PreparedPoolTip&)=delete;
    void PublishAfterCommit() noexcept {
        if (thread_!=std::this_thread::get_id() || published_) std::terminate();
        if (bridge_update_) bridge_update_->PublishAfterCommit();
        pool_->PublishAfterCommit();
        if (relay_update_) relay_update_->PublishAfterCommit();
        published_=true;
    }
    // Fallible outbound work occurs only after all three owners publish and
    // release their locks. An attempted send may be ambiguous; never undo the
    // already committed pool/cache publication or silently claim delivery.
    void RequestRefresh() {
        if (thread_!=std::this_thread::get_id()) throw std::logic_error("Pool tip owner is thread-affine");
        if (!published_ || requested_) throw std::logic_error("Pool tip refresh is not available");
        requested_=true;
        if (relay_ && !pool_->RefreshCandidates().empty())
            relay_->RequestProofRefresh(pool_->RefreshCandidates(),Mempool::ProofRefreshPolicy{}.batch_size);
    }
private:
    PreparedPoolTip(std::shared_ptr<network::BridgeNode> bridge,std::shared_ptr<TxRelayManager> relay)
        : bridge_(std::move(bridge)),relay_(std::move(relay)) {}
    std::optional<Mempool::ProofRefreshPolicy> Policy() const {
        return relay_ ? std::optional<Mempool::ProofRefreshPolicy>(Mempool::ProofRefreshPolicy{}) : std::nullopt;
    }
    void PrepareCaches() {
        if (bridge_) bridge_update_=bridge_->PrepareTxProofCacheInvalidation();
        if (relay_) relay_update_=relay_->PrepareTipChanged();
    }
    const std::thread::id thread_=std::this_thread::get_id();
    // Owners outlive prepared children (members are destroyed in reverse order).
    std::shared_ptr<network::BridgeNode> bridge_;
    std::shared_ptr<TxRelayManager> relay_;
    std::unique_ptr<Mempool::PreparedBlockUpdate> pool_;
    std::unique_ptr<network::BridgeNode::PreparedTxCacheUpdate> bridge_update_;
    std::unique_ptr<TxRelayManager::PreparedTipUpdate> relay_update_;
    bool published_=false,requested_=false;
};
} // namespace dinero

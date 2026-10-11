#pragma once
#include "consensus/chain_state_view.h"
#include "consensus/orchard_state_transition.h"
#include "daemon/mempool_chainstate_guard.h"
#include "storage/orchard_catalog_state.h"
#include <map>
#include <set>

namespace dinero {
class OrchardCompactChainstate;
// Immutable, bounded input snapshot from an enrolled compact owner. Only exact
// proved inputs and explicitly catalog-proved output absences are answerable.
// Unknown lookups are errors. This is not pool admission, signature validation,
// a lasting selected-parent authorization, or a general-purpose coin database.
class OrchardPoolCoinView final : public consensus::ChainStateView {
public:
    StatusOr<consensus::UTXOEntry> getCoin(const OutPoint& point) const override {
        if(const auto i=inputs_.find(point);i!=inputs_.end())return i->second;
        if(absent_.contains(point))return Status::NotFound;
        return Status::Internal;
    }
    bool hasCoin(const OutPoint& point) const override {
        const auto coin=getCoin(point);
        if(!coin.ok()&&coin.status()!=Status::NotFound)
            throw consensus::OrchardCoinLookupError(coin.status());
        return coin.ok();
    }
    uint32_t getHeight() const override {return height_;}
    const uint256& ParentHash() const noexcept {return parent_;}
    size_t CapturedInputs() const noexcept {return inputs_.size();}
    size_t ProvedOutputAbsences() const noexcept {return absent_.size();}
private:
    friend class OrchardCompactChainstate;
    static OrchardPoolCoinView Capture(const consensus::OrchardTransactionContext&,
        const BlockHeader&,const storage::catalog::State&,
        const storage::catalog::Tree::Read&,std::span<const MempoolProofView>);
    explicit OrchardPoolCoinView(uint32_t height,uint256 parent):height_(height),parent_(parent){}
    uint32_t height_;
    uint256 parent_;
    std::map<OutPoint,consensus::UTXOEntry> inputs_;
    std::set<OutPoint> absent_;
};
} // namespace dinero

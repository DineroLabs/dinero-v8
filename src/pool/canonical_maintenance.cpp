#include "pool/canonical_maintenance.h"
#include "pool/pool_manager.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/mempool_transaction.h"
#include "storage/chain_db.h"
#include "consensus/merkle_root.h"
#include "primitives/amount.h"
#include <ctime>
#include <stdexcept>
namespace dinero::pool {
namespace {void RequireMaintenance(bool ok){if(!ok)throw std::runtime_error("canonical pool accounting source unavailable or changed");}}
void CanonicalPoolMaintenance::Reconcile(const std::shared_ptr<ChainstateService>& source,PoolManager& manager) {
    RequireMaintenance(source!=nullptr);
    const auto use=ChainstateService::AcquireWalletIndexUse(source);
    const auto selected=source->AcquireBlockIngressActivationLock();
    ReconcileSelected(*source,manager);
}
void CanonicalPoolMaintenance::ReconcileSelected(ChainstateService& source,PoolManager& manager) {
    source.AssertActivationLockHeld("pool canonical accounting");
    const auto* chain=source.GetChainDB();RequireMaintenance(chain!=nullptr);
    const auto tip=chain->getTip();RequireMaintenance(tip.ok() && tip->height>=0);
    const auto selected_tip=source.getCanonicalBlockHash(static_cast<uint32_t>(tip->height));
    RequireMaintenance(selected_tip.ok() && *selected_tip==tip->hash);
    std::lock_guard<std::mutex> owner(manager.mutex_);RequireMaintenance(manager.db_ && manager.calculator_);
    const auto blocks=manager.db_->getRecordedBlocks();
    struct Observation {PoolBlock block;bool orphan;uint32_t confirmations;};std::vector<Observation> prepared;prepared.reserve(blocks.size());
    // Complete known inventory and every body/source read BEFORE accounting.
    // A read error is never evidence of non-membership in the selected chain.
    for(const auto& block:blocks) {
        uint256 hash;RequireMaintenance(block.height>0 && block.height<=INT32_MAX && block.block_hash.size()==64 && uint256::FromHex(block.block_hash,hash) && !hash.IsNull());
        const auto body=source.getBlockRpcSnapshot(hash);
        RequireMaintenance(body.ok() && body->height==block.height && body->header.GetHash()==hash && !body->transaction_ids.empty());
        std::vector<TxId> ids;ids.reserve(body->transaction_ids.size());for(const auto& id:body->transaction_ids)ids.emplace_back(id);
        bool mutated=false;RequireMaintenance(consensus::ComputeTransactionMerkleRoot(ids,&mutated)==body->header.merkle_root && !mutated);
        const auto coinbase=source.getTransactionBody(body->transaction_ids.front());
        RequireMaintenance(coinbase.ok() && !coinbase->IsOrchard() && coinbase->Historical().IsCoinbase() && coinbase->GetTxid().AsUint256()==body->transaction_ids.front());
        uint64_t reward=0;for(const auto& output:coinbase->Historical().vout) {
            RequireMaintenance(!output.is_confidential && output.value.GetUna()<=MAX_SUPPLY_UNA_CONST-reward);reward+=output.value.GetUna();
        }
        RequireMaintenance(reward>0 && reward==block.total_reward);
        bool orphan=block.height>static_cast<uint32_t>(tip->height);
        if(!orphan) {
            const auto canonical=source.getCanonicalBlockHash(block.height);RequireMaintenance(canonical.ok());orphan=*canonical!=hash;
        }
        const uint32_t confirmations=orphan?0:static_cast<uint32_t>(uint64_t(tip->height)-block.height+1);
        prepared.push_back({block,orphan,confirmations});
    }
    const auto observed_at=std::time(nullptr);RequireMaintenance(observed_at>=0);
    // Individual FULL transitions may leave an already committed prefix after
    // a later refusal. No cursor, acknowledgement or readiness is published.
    std::vector<PoolBlock> canonical_blocks;canonical_blocks.reserve(prepared.size());
    for(const auto& entry:prepared) {
        if(entry.orphan) {
            // Retention and reversal share one checked FULL transaction. Repeat
            // delivery validates the retained postimage without another debit.
            manager.db_->transitionCanonicalOrphan(entry.block,true);
        } else {
            auto updated=entry.block;
            if(updated.orphaned) {manager.db_->transitionCanonicalOrphan(updated,false);updated.orphaned=false;}
            manager.db_->updateBlockConfirmationsChecked(updated,entry.confirmations,static_cast<int64_t>(observed_at));
            updated.confirmations=entry.confirmations;
            if(updated.confirmations>=updated.required_confirmations && updated.confirmed_at==0)updated.confirmed_at=observed_at;
            canonical_blocks.push_back(std::move(updated));
        }
    }
    // Retry a previously interrupted allocation even when depth is unchanged.
    // All allocations retain their existing checked owner and origin policy.
    // The FULL allocation transaction rechecks the exact eligible subset;
    // a newly inserted/changed eligible row cannot borrow these source reads.
    (void)manager.db_->allocateConfirmedBlockPayouts(*manager.calculator_,&canonical_blocks);
}
} // namespace dinero::pool

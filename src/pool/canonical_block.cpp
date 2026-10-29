#include "pool/canonical_block.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/mempool_transaction.h"
#include "consensus/merkle_root.h"
#include "primitives/amount.h"
#include <cmath>
#include <stdexcept>
namespace dinero::pool {
namespace {
void RequirePoolBlock(bool ok){if(!ok)throw std::runtime_error("selected canonical pool block or reward unavailable");}
}
PoolManager::ShareSubmitResult RecordCanonicalPoolShare(
        const std::shared_ptr<ChainstateService>& source,PoolManager& manager,const CanonicalPoolShare& share) {
    RequirePoolBlock(source && share.height && share.reward && share.reward<=MAX_SUPPLY_UNA_CONST &&
        std::isfinite(share.difficulty) && share.difficulty>0 && share.difficulty<4294967296.0 && share.hash.size()==64);
    uint256 requested;RequirePoolBlock(uint256::FromHex(share.hash,requested) && !requested.IsNull());
    const auto lifetime=ChainstateService::AcquireWalletIndexUse(source);
    const auto selected=source->AcquireBlockIngressActivationLock();
    const auto hash=source->getCanonicalBlockHash(share.height);RequirePoolBlock(hash.ok() && *hash==requested);
    const auto block=source->getBlockRpcSnapshot(*hash);
    RequirePoolBlock(block.ok() && block->height==share.height && block->header.GetHash()==*hash && !block->transaction_ids.empty());
    std::vector<TxId> ids;ids.reserve(block->transaction_ids.size());for(const auto& id:block->transaction_ids)ids.emplace_back(id);
    bool mutated=false;RequirePoolBlock(consensus::ComputeTransactionMerkleRoot(ids,&mutated)==block->header.merkle_root && !mutated);
    const auto coinbase=source->getTransactionBody(block->transaction_ids.front());
    RequirePoolBlock(coinbase.ok() && !coinbase->IsOrchard() && coinbase->Historical().IsCoinbase() &&
        coinbase->GetTxid().AsUint256()==block->transaction_ids.front());
    uint64_t reward=0;
    for(const auto& output:coinbase->Historical().vout) {
        RequirePoolBlock(!output.is_confidential && output.value.GetUna()<=MAX_SUPPLY_UNA_CONST-reward);
        reward+=output.value.GetUna();
    }
    RequirePoolBlock(reward==share.reward);
    // Caller-controlled hexadecimal casing is not a separate block identity.
    // The manager owns all pool-local changes, including the checked transaction.
    return manager.onShareSubmit(share.worker,share.job,share.difficulty,true,false,true,
                                 hash->GetHex(),share.height,reward,share.uid);
}
} // namespace dinero::pool

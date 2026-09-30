#include "pool/canonical_payment.h"
#include "pool/pool_db.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/mempool_transaction.h"
#include "consensus/chainparams.h"
#include "consensus/merkle_root.h"
#include "address/addr_codec.h"
#include "storage/chain_db.h"
#include <openssl/sha.h>
#include <algorithm>
#include <stdexcept>
namespace dinero::pool {
namespace {
void CanonicalPaymentCheck(bool ok) {if(!ok)throw std::runtime_error("canonical pool payment observation unavailable");}
uint256 PaymentHash(const std::array<uint8_t,32>& bytes){uint256 h;std::copy(bytes.begin(),bytes.end(),h.begin());return h;}
std::array<uint8_t,32> PaymentBytes(const uint256& h){std::array<uint8_t,32> b{};std::copy(h.begin(),h.end(),b.begin());return b;}
}
bool PoolPaymentCanonicalOwner::Reconcile(const std::shared_ptr<ChainstateService>& source,PoolDB& db,const PoolPaymentAttempt& attempt) {
    CanonicalPaymentCheck(attempt.retained.has_value());
    // Missing source defers this transition; it is never an absence proof.
    if(!source)return false;
    const auto lifetime=ChainstateService::AcquireWalletIndexUse(source);
    const auto selected=source->AcquireBlockIngressActivationLock();
    const auto* chain=source->GetChainDB();if(!chain)return false;
    uint256 genesis;CanonicalPaymentCheck(uint256::FromHex(Params().genesis_hash,genesis) &&
        attempt.binding.genesis==PaymentBytes(genesis) && attempt.binding.network==static_cast<uint8_t>(GetActiveChain()));
    const auto tip=chain->getTip();CanonicalPaymentCheck(tip.ok() && tip->height>=0);
    const auto tip_hash=source->getCanonicalBlockHash(static_cast<uint32_t>(tip->height));
    CanonicalPaymentCheck(tip_hash.ok() && *tip_hash==tip->hash);
    const auto& retained=*attempt.retained;const auto txid=PaymentHash(retained.txid);
    std::optional<PoolPaymentSettlement> observed;
    const auto location=chain->getTxLocation(txid);
    if(location.ok()) {
        const auto height=chain->getBlockHeight(location->first);
        CanonicalPaymentCheck(height.ok() && *height>=0 && *height<=tip->height);
        const auto canonical=source->getCanonicalBlockHash(static_cast<uint32_t>(*height));
        CanonicalPaymentCheck(canonical.ok() && *canonical==location->first);
        const auto block=source->getBlockRpcSnapshot(*canonical);
        CanonicalPaymentCheck(block.ok() && block->height==static_cast<uint32_t>(*height) && block->header.GetHash()==*canonical);
        std::vector<TxId> ids;ids.reserve(block->transaction_ids.size());
        for(const auto& id:block->transaction_ids)ids.emplace_back(id);
        bool mutated=false;const auto merkle=consensus::ComputeTransactionMerkleRoot(ids,&mutated);
        CanonicalPaymentCheck(!mutated && merkle==block->header.merkle_root && location->second<ids.size() && ids[location->second].AsUint256()==txid);
        const auto transaction=source->getTransactionBody(txid);
        CanonicalPaymentCheck(transaction.ok() && !transaction->IsOrchard() && !transaction->Historical().IsCoinbase());
        const auto bytes=transaction->Serialize(TxSerializationMode::WithWitness);std::array<uint8_t,32> digest{};
        CanonicalPaymentCheck(SHA256(bytes.data(),bytes.size(),digest.data()) && digest==retained.body_sha256);
        const auto address=DecodeWitnessAddress(attempt.address,HrpForActiveNetworkRef());CanonicalPaymentCheck(address.is_valid && !address.script_pubkey.empty());
        const auto output=source->getCanonicalOutputInclusion(txid,retained.vout,static_cast<uint32_t>(*height));
        CanonicalPaymentCheck(output.ok() && output->block_hash==*canonical && output->MatchesTransparent(attempt.amount,address.script_pubkey));
        observed=PoolPaymentSettlement{PaymentBytes(*canonical),static_cast<uint32_t>(*height),block->header.timestamp};
    } else {
        CanonicalPaymentCheck(location.status()==Status::NotFound);
        // A missing optional transaction index never proves chain-wide absence.
        // An old settled anchor can be undone only when its exact height/hash
        // is no longer canonical. Same-anchor index loss preserves accounting.
        if(attempt.settlement && attempt.settlement->height<=static_cast<uint32_t>(tip->height)) {
            const auto old=source->getCanonicalBlockHash(attempt.settlement->height);
            CanonicalPaymentCheck(old.ok());
            if(*old==PaymentHash(attempt.settlement->block))return false;
        } else if(!attempt.settlement)return false;
    }
    // No wallet, relay, user callback or source reacquisition occurs in this
    // checked DB-only transition. The selected lock remains held through COMMIT.
    return db.reconcilePaymentSettlement(attempt,observed);
}
} // namespace dinero::pool

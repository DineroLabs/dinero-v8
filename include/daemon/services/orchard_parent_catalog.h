#pragma once
#include "daemon/services/orchard_parent_replay.h"
#include "daemon/services/orchard_history_capture.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/utreexo_stump.h"
#include "storage/chain_db.h"
#include "storage/orchard_catalog_nodes.h"
#include <map>
#include <memory>

namespace dinero {
// Prepared from one completed independent replay plus its complete captured
// bodies. Only immutable UNREACHABLE nodes are written here. The roots are
// private: no canonical catalog head, tip, READY marker or activation is written.
// A later canonical transition must own and bind this preparation before roots
// can become reachable. Failed or abandoned preparations confer no authority.
class PreparedOrchardCatalog final {
public:
    struct LegacyMetadata {uint32_t height;bool coinbase;bool operator==(const LegacyMetadata&) const=default;};
    PreparedOrchardCatalog(const PreparedOrchardCatalog&)=delete;
    PreparedOrchardCatalog& operator=(const PreparedOrchardCatalog&)=delete;
    uint64_t TransactionCount() const {Check();return transaction_count_;}
    uint64_t LegacyCoinCount() const {Check();return legacy_count_;}
    uint64_t NonTransparentCoinCount() const {Check();return nontransparent_count_;}
    bool NonTransparentCoin(const OutPoint& point) const {
        Check();const auto bytes=storage::catalog::OutpointBytes(point);
        const auto value=Tree(storage::catalog::Kind::NonTransparentCoins).Find(nontransparent_,storage::catalog::NonTransparentKey(bytes));
        if(value)Require(*value==bytes);return value.has_value();
    }
    bool ContainsTransaction(const TxId& id) const {
        Check();return Tree(storage::catalog::Kind::Transactions).Find(transactions_,storage::catalog::TransactionKey(id)).has_value();
    }
    std::optional<LegacyMetadata> LegacyCoin(const OutPoint& point) const {
        Check();const auto bytes=storage::catalog::OutpointBytes(point);
        const auto value=Tree(storage::catalog::Kind::LegacyCoins).Find(legacy_,storage::catalog::LegacyKey(bytes));
        if(!value)return {};
        Require(value->size()==41&&value->compare(0,36,bytes)==0);
        const auto height=uint32_t(storage::catalog::Number(*value,36,4));
        Require(height<leaf_activation_&&height<=target_.height);
        return LegacyMetadata{height,uint8_t((*value)[40])!=0};
    }
    const std::vector<uint8_t>& VerificationStump() const {Check();return stump_;}
private:
    friend class ChainstateService;
    friend class OrchardReindexOwner;
    friend class PreparedOrchardChainstateWrite;
    friend struct OrchardParentCatalogTestAccess;
    using Digest=storage::catalog::Digest;
    // The detached owner retains the completed replay until this catalog and
    // every writer preparation using it are destroyed. No reference escapes
    // into the published selected-state value.
    ChainDB& db_;const OrchardParentReplay& proof_;
    Digest wire_{};const storage::LegacyRetirementRecord record_;
    const OrchardParentReplay::Target target_;
    const uint32_t leaf_activation_;
    const std::thread::id thread_=std::this_thread::get_id();
    Digest transactions_{},legacy_{},nontransparent_{};uint64_t transaction_count_=0,legacy_count_=0,nontransparent_count_=0;
    std::vector<uint8_t> stump_;
    PreparedOrchardCatalog(ChainDB& db,const OrchardParentReplay& replay)
        :db_(db),proof_(replay),record_(replay.Record()),target_(replay.ValidatedTarget()),
         leaf_activation_(consensus::GetUtreexoMaturityLeafActivationHeight()){}
    static void Require(bool ok){if(!ok)throw std::runtime_error("Orchard parent catalog unavailable");}
    void Check() const {
        const auto& p=Params();
        Require(thread_==std::this_thread::get_id()&&consensus::OrchardProfileConfigurationValid(p)&&
            p.orchard_branch_id==record_.branch_id&&p.orchard_activation_height==record_.activation_height&&
            uint256::FromHexUnsafe(p.genesis_hash)==record_.genesis&&
            consensus::GetUtreexoMaturityLeafActivationHeight()==leaf_activation_);
    }
    std::string Read(const Digest& id) const {
        const auto value=db_.getOrchardCatalogNode(id);Require(value.ok());return *value;
    }
    storage::catalog::Tree Tree(storage::catalog::Kind kind) const {
        return {kind,[this](const auto& id){return Read(id);}};
    }
    static std::unique_ptr<PreparedOrchardCatalog> Create(ChainDB& db,const ChainWriteToken& token,
        const OrchardParentReplay& replay,const OrchardHistoryCapture& history) {
        // Production callers are the detached service and unpublished startup
        // reindex owner. Both run Append BEFORE RecordBody for this exact
        // hash-linked history, then finish both and compare the selected state.
        auto result=std::unique_ptr<PreparedOrchardCatalog>(new PreparedOrchardCatalog(db,replay));
        result->Check();const auto& target=result->target_;
        result->wire_=history.WireHashAt(target.height);
        Require(history.Finished()&&history.Count()==uint64_t(target.height)+1&&
            history.HeaderAt(target.height).hash==target.hash&&history.HeaderAt(target.height).work==target.chainwork&&
            history.HeaderAt(0).hash==result->record_.genesis);
        const auto* forest=replay.ProvenState().Forest();Require(forest!=nullptr);
        const auto stump=consensus::UtreexoStump::fromForest(*forest);
        Require(stump.getNumLeaves()==forest->getNumLeaves()&&stump.getCommitment()==forest->getCommitment());
        result->stump_=stump.serialize();
        const auto restored=consensus::UtreexoStump::deserialize(result->stump_);
        Require(restored.getNumLeaves()==forest->getNumLeaves()&&restored.getCommitment()==forest->getCommitment());
        // Fixed pending-node bound, separate from the full replay's work policy.
        // Sync each batch. Any prefix is immutable and unreachable until the
        // future canonical batch installs a head; no preparation receipt is a head.
        constexpr size_t pending_limit=1024;
        std::map<Digest,std::string> pending;
        const auto flush=[&] {
            if(pending.empty())return;
            rocksdb::WriteBatch batch;
            for(const auto& [id,bytes]:pending)Require(db.stageOrchardCatalogNode(token,id,bytes,batch)==Status::Ok);
            Require(db.writeBatch(token,std::move(batch),true)==Status::Ok);pending.clear();
        };
        const auto read=[&](const Digest& id) {
            const auto found=pending.find(id);return found==pending.end()?result->Read(id):found->second;
        };
        const auto write=[&](const Digest& id,const std::string& bytes) {
            Require(storage::catalog::ValidNode(id,bytes));
            if(const auto found=pending.find(id);found!=pending.end()){Require(found->second==bytes);return;}
            if(pending.size()==pending_limit)flush();pending.emplace(id,bytes);
        };
        storage::catalog::Tree transactions(storage::catalog::Kind::Transactions,read,write);
        const auto count=history.ForEachTransaction([&](const TxId& id) {
            Require(result->transaction_count_!=UINT64_MAX);
            result->transactions_=transactions.Insert(result->transactions_,storage::catalog::TransactionKey(id),{});
            ++result->transaction_count_;return true;
        });
        Require(count==history.TransactionCount()&&count==result->transaction_count_);
        storage::catalog::Tree legacy(storage::catalog::Kind::LegacyCoins,read,write);
        storage::catalog::Tree nontransparent(storage::catalog::Kind::NonTransparentCoins,read,write);
        for(const auto& [point,coin]:replay.ProvenState().ProvenUtxos()) {
            Require(coin.height<=target.height);
            if(coin.is_confidential||!coin.commitment.empty()) {
                Require(result->nontransparent_count_!=UINT64_MAX);
                const auto point_bytes=storage::catalog::OutpointBytes(point);
                result->nontransparent_=nontransparent.Insert(result->nontransparent_,storage::catalog::NonTransparentKey(point_bytes),point_bytes);
                ++result->nontransparent_count_;
            }
            if(coin.height>=result->leaf_activation_)continue;
            Require(result->legacy_count_!=UINT64_MAX);
            const auto outpoint=storage::catalog::OutpointBytes(point);auto bytes=outpoint;
            storage::catalog::Number(bytes,coin.height,4);bytes.push_back(coin.isCoinbase?1:0);
            result->legacy_=legacy.Insert(result->legacy_,storage::catalog::LegacyKey(outpoint),bytes);++result->legacy_count_;
        }
        flush();
        // Re-read from durable storage after every pending write has completed.
        // Enumerate the actual validated owners again, never raw database rows.
        Require(history.ForEachTransaction([&](const TxId& id){return result->ContainsTransaction(id);})==count);
        uint64_t found=0,found_nontransparent=0;
        for(const auto& [point,coin]:replay.ProvenState().ProvenUtxos()) {
            const bool expected_nontransparent=coin.is_confidential||!coin.commitment.empty();
            Require(result->NonTransparentCoin(point)==expected_nontransparent);
            if(expected_nontransparent)++found_nontransparent;
            const auto metadata=result->LegacyCoin(point);
            if(coin.height>=result->leaf_activation_){Require(!metadata);continue;}
            Require(metadata&&*metadata==LegacyMetadata{coin.height,coin.isCoinbase});++found;
        }
        Require(found==result->legacy_count_&&found_nontransparent==result->nontransparent_count_);result->Check();return result;
    }
};
} // namespace dinero

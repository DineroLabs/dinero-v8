#pragma once
#include "primitives/block.h"
#include "consensus/chainwork.h"
#include "consensus/merkle_root.h"
#include "crypto/sha256.h"
#include "wallet/runtime_replay_disk_membership.h"
#include <algorithm>
#include <span>
#include <thread>

namespace dinero {
// Disposable inputs for one independently replayed historical branch. This is
// neither a validity certificate nor a durable chain database. The service must
// still run the full replay and recheck captured inputs against its source.
// Fixed-size header/digest rows and transaction membership live in the existing
// authenticated temporary spool. Missing expected rows are errors; membership
// absence follows checked Patricia nodes from an owned root, never SQL absence.
// The pager is bounded; the validator's UTXO/forest/header state is separate and
// this class makes no total resident-memory or general-history capacity claim.
class OrchardHistoryCapture final {
public:
    struct Header { uint256 hash; BlockHeader header; arith_uint256 work; };
    static constexpr size_t HeaderBatchLimit=256;
    OrchardHistoryCapture(uint32_t target_height,const uint256& target_hash)
        : count_(uint64_t(target_height)+1),reverse_remaining_(count_),previous_(target_hash) {
        Require(target_height<UINT32_MAX && !target_hash.IsNull());
    }
    OrchardHistoryCapture(const OrchardHistoryCapture&)=delete;
    OrchardHistoryCapture& operator=(const OrchardHistoryCapture&)=delete;

    // Descending contiguous headers, beginning with the captured target. All
    // source reads finish BEFORE this call takes a private spool transaction.
    void CaptureReverse(std::span<const Header> headers) {
        CheckThread();const bool usable=!poisoned_&&!finished_;poisoned_=true;
        Require(usable && !headers.empty() && headers.size()<=HeaderBatchLimit &&
            headers.size()<=reverse_remaining_ && recorded_==0);
        uint64_t remaining=reverse_remaining_;auto previous=previous_;
        wallet::detail::RuntimeReplaySpool::Batch batch(spool_);
        for(const auto& h:headers) {
            Require(h.hash==previous && h.header.GetHash()==h.hash);
            --remaining;spool_.Insert(HeightKey('h',uint32_t(remaining)),Encode(h));
            previous=h.header.prev_block_hash;
        }
        if(!remaining)Require(previous.IsNull());
        batch.Commit();reverse_remaining_=remaining;previous_=previous;poisoned_=false;
    }
    uint64_t Count() const {CheckReadable();return count_;}
    Header HeaderAt(uint32_t height) const {
        CheckReadable();Require(reverse_remaining_==0 && uint64_t(height)<count_);
        const auto bytes=spool_.Get(HeightKey('h',height));Require(bytes && bytes->size()==193 && bytes->at(0)==1);
        Header h{};std::copy_n(bytes->begin()+1,32,h.hash.begin());
        const auto decoded=BlockHeader::Deserialize(bytes->data()+33,128);Require(bool(decoded));h.header=*decoded;
        for(unsigned word=0;word<4;++word)h.work.SetWord(word,Number(*bytes,161+8*word,8));
        Require(h.hash==h.header.GetHash());return h;
    }

    // Called after the genuine validator accepted this body, with no selected
    // owner or external callback inside the spool transaction. This records
    // exact bytes and enforces the prior all-history transaction-ID rule; it
    // cannot itself certify scripts, work, fees, proofs or retirement state.
    void RecordBody(uint32_t height,const Block& body) {
        CheckReadable();Require(!finished_);
        const auto captured=HeaderAt(height);
        const bool ordered=uint64_t(height)==recorded_;poisoned_=true;
        Require(ordered && body.GetHash()==captured.hash &&
            body.header.SerializeForHash()==captured.header.SerializeForHash() && !body.vtx.empty());
        bool mutated=false;
        Require(consensus::ComputeMerkleRoot(body.vtx,&mutated)==captured.header.merkle_root && !mutated);
        const auto wire=body.Serialize();uint256 digest;crypto::CSHA256().Write(wire).Finalize(digest.data);
        auto next_root=transactions_root_;auto next_count=transaction_count_;
        wallet::detail::RuntimeReplaySpool::Batch batch(spool_);
        for(const auto& tx:body.vtx) {
            const auto key=TransactionKey(tx.GetTxid());
            Require(!transactions_.Contains(next_root,key));
            Require(next_count!=UINT64_MAX);
            next_root=transactions_.With(next_root,key);++next_count;
        }
        spool_.Insert(HeightKey('w',height),std::span<const uint8_t>(digest.data,32));
        batch.Commit();transactions_root_=next_root;transaction_count_=next_count;++recorded_;poisoned_=false;
    }
    void Finish() {
        CheckReadable();const bool usable=!finished_;poisoned_=true;
        Require(usable && reverse_remaining_==0 && recorded_==count_ && transactions_root_!=0);
        // Completion must cover the entire reachable transaction inventory,
        // not merely the keys that a later candidate happens to query.
        transactions_.ForEach(transactions_root_,transaction_count_,[](const auto&){return true;});
        spool_.Freeze();finished_=true;poisoned_=false;
    }
    bool Finished() const {CheckReadable();return finished_;}
    uint256 WireHashAt(uint32_t height) const {
        CheckReadable();Require(finished_ && uint64_t(height)<count_);
        const auto bytes=spool_.Get(HeightKey('w',height));Require(bytes && bytes->size()==32);
        uint256 digest;std::copy(bytes->begin(),bytes->end(),digest.begin());return digest;
    }
    bool ContainsTransaction(const TxId& id) const {
        CheckReadable();Require(finished_);return transactions_.Contains(transactions_root_,TransactionKey(id));
    }
    uint64_t TransactionCount() const {CheckReadable();Require(finished_);return transaction_count_;}
    // Export IDs, never the disposable spool root. The capture still is NOT a
    // consensus certificate: the service must pair it with the same completed
    // independent parent replay. Consumers must stage privately and discard on
    // any exception/false visitor result; no partial durable publication.
    template<class Visitor>
    uint64_t ForEachTransaction(Visitor&& visitor) const {
        CheckReadable();Require(finished_);
        return transactions_.ForEach(transactions_root_,transaction_count_,[&](const auto& key) {
            uint256 hash;std::copy(key.begin(),key.end(),hash.begin());return visitor(TxId(hash));
        });
    }
    wallet::detail::RuntimeReplaySpool::Usage UsageNow() const {CheckReadable();return spool_.UsageNow();}
private:
    friend struct OrchardHistoryCaptureTestAccess;
    using Spool=wallet::detail::RuntimeReplaySpool;
    using Membership=wallet::detail::RuntimeReplayDiskMembership;
    using Bytes=Spool::Bytes;
    Spool spool_;
    Membership transactions_{spool_};
    Membership::Root transactions_root_=0;
    const uint64_t count_;
    uint64_t reverse_remaining_,recorded_=0,transaction_count_=0;
    uint256 previous_;
    const std::thread::id thread_=std::this_thread::get_id();
    bool poisoned_=false,finished_=false;
    static void Require(bool ok){if(!ok)throw std::runtime_error("Orchard historical capture unavailable or inconsistent");}
    void CheckThread() const {Require(thread_==std::this_thread::get_id());}
    void CheckReadable() const {CheckThread();Require(!poisoned_);}
    static void Number(Bytes& bytes,uint64_t value,unsigned size) {
        for(unsigned i=0;i<size;++i){bytes.push_back(uint8_t(value));value>>=8;}
    }
    static uint64_t Number(const Bytes& bytes,size_t at,unsigned size) {
        Require(at<=bytes.size()&&size<=bytes.size()-at);uint64_t value=0;
        for(unsigned i=0;i<size;++i)value|=uint64_t(bytes[at+i])<<(8*i);return value;
    }
    static Bytes HeightKey(uint8_t space,uint32_t height) {Bytes key{space};Number(key,height,4);return key;}
    static Membership::Key TransactionKey(const TxId& id) {
        Membership::Key key;const auto& hash=id.AsUint256();std::copy_n(hash.data,32,key.begin());return key;
    }
    static Bytes Encode(const Header& h) {
        Bytes bytes{1};bytes.reserve(193);bytes.insert(bytes.end(),h.hash.begin(),h.hash.end());
        const auto wire=h.header.SerializeForHash();bytes.insert(bytes.end(),wire.begin(),wire.end());
        for(unsigned word=0;word<4;++word)Number(bytes,h.work.GetWord(word),8);return bytes;
    }
};
} // namespace dinero

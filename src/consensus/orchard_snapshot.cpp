#include "consensus/orchard_snapshot.h"
#include "consensus/tx_validation.h"
#include "consensus/orchard_state_root.h"
#include "consensus/orchard_catalog_coin_view.h"
#include "consensus/orchard_catalog_transition.h"
#include "consensus/orchard_block_staging.h"
#include "consensus/orchard_block_filter.h"
#include "consensus/pow.h"
#include "crypto/sha256.h"
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace dinero::consensus {
namespace {
void Check(bool ok) {if(!ok)throw std::invalid_argument("invalid Orchard snapshot section");}
struct Writer {
    std::vector<uint8_t> bytes;
    const OrchardSnapshotSink* sink=nullptr;
    bool counting=false;
    uint64_t used=0;
    void Raw(std::span<const uint8_t> value) {
        Check(value.size()<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES-used);
        if(sink)(*sink)(value);
        else if(!counting)bytes.insert(bytes.end(),value.begin(),value.end());
        used+=value.size();
    }
    void Raw(const std::string& value) {
        Raw({reinterpret_cast<const uint8_t*>(value.data()),value.size()});
    }
    void Number(uint64_t n,unsigned width) {uint8_t b[8];for(unsigned i=0;i<width;++i){b[i]=uint8_t(n);n>>=8;}Raw({b,width});}
    void Hash(const uint256& h) {Raw({h.data,32});}
    void Blob(std::span<const uint8_t> value) {Number(value.size(),8);Raw(value);}
    void Blob(const std::string& value) {Blob({reinterpret_cast<const uint8_t*>(value.data()),value.size()});}
};
struct Reader {
    std::span<const uint8_t> bytes;
    uint64_t Remaining()const{return bytes.size();}
    std::span<const uint8_t> Raw(uint64_t size) {
        Check(size<=bytes.size());auto value=bytes.first(size_t(size));bytes=bytes.subspan(size_t(size));return value;
    }
    uint64_t Number(unsigned width) {const auto value=Raw(width);uint64_t n=0;for(unsigned i=0;i<width;++i)n|=uint64_t(value[i])<<(8*i);return n;}
    uint256 Hash() {uint256 h;const auto value=Raw(32);std::copy(value.begin(),value.end(),h.begin());return h;}
    std::span<const uint8_t> Blob(uint64_t maximum) {const auto size=Number(8);Check(size<=maximum);return Raw(size);}
    std::string Text(uint64_t maximum) {const auto value=Blob(maximum);return {reinterpret_cast<const char*>(value.data()),value.size()};}
    std::vector<uint8_t> ByteVector(uint64_t maximum){const auto v=Blob(maximum);return {v.begin(),v.end()};}
};
struct StreamReader {
    uint64_t remaining;
    const OrchardSnapshotSource& source;
    uint64_t Remaining()const{return remaining;}
    void Fill(std::span<uint8_t> out){Check(out.size()<=remaining);source(out);remaining-=out.size();}
    std::vector<uint8_t> Raw(uint64_t size){Check(size<=remaining);std::vector<uint8_t> v(size);Fill(v);return v;}
    uint64_t Number(unsigned width){uint8_t b[8];Check(width<=8);Fill({b,width});uint64_t n=0;for(unsigned i=0;i<width;++i)n|=uint64_t(b[i])<<(8*i);return n;}
    uint256 Hash(){uint256 h;Fill({h.data,32});return h;}
    std::string Text(uint64_t maximum){const auto n=Number(8);Check(n<=maximum&&n<=remaining);std::string s(size_t(n),'\0');Fill({reinterpret_cast<uint8_t*>(s.data()),s.size()});return s;}
    std::vector<uint8_t> ByteVector(uint64_t maximum){const auto n=Number(8);Check(n<=maximum);return Raw(n);}
};
void State(Writer& w,const storage::OrchardStoredState& s) {
    w.Number(s.height,4);w.Hash(s.block_hash);w.Hash(s.anchor);w.Number(s.pool_balance,8);w.Number(s.tree_size,8);w.Blob(s.frontier);
}
template<class R>storage::OrchardStoredState State(R& r) {
    storage::OrchardStoredState s;s.height=uint32_t(r.Number(4));s.block_hash=r.Hash();s.anchor=r.Hash();
    s.pool_balance=r.Number(8);s.tree_size=r.Number(8);s.frontier=r.Text(storage::ORCHARD_STORED_FRONTIER_LIMIT);return s;
}
void Retirement(Writer& w,const storage::LegacyRetirementState& s) {
    const auto& r=s.record;w.Number(r.network_code,1);w.Hash(r.genesis);w.Number(r.branch_id,4);
    w.Number(r.activation_height,4);w.Number(r.legacy_epoch_height,4);w.Hash(r.boundary_parent);w.Hash(r.legacy_state_root);
    w.Number(r.retired_value,8);w.Hash(r.tree_root);w.Number(r.tree_size,8);w.Number(r.nullifier_count,8);
    w.Number(s.height,4);w.Hash(s.block_hash);w.Hash(s.parent_hash);
}
template<class R>storage::LegacyRetirementState Retirement(R& in) {
    storage::LegacyRetirementState s;auto& r=s.record;r.network_code=uint8_t(in.Number(1));r.genesis=in.Hash();
    r.branch_id=uint32_t(in.Number(4));r.activation_height=uint32_t(in.Number(4));r.legacy_epoch_height=uint32_t(in.Number(4));
    r.boundary_parent=in.Hash();r.legacy_state_root=in.Hash();r.retired_value=in.Number(8);r.tree_root=in.Hash();
    r.tree_size=in.Number(8);r.nullifier_count=in.Number(8);s.height=uint32_t(in.Number(4));s.block_hash=in.Hash();s.parent_hash=in.Hash();return s;
}
void HashNumber(crypto::CSHA256& hash,uint64_t n) {
    uint8_t bytes[8];for(unsigned i=0;i<8;++i)bytes[i]=uint8_t(n>>(8*i));hash.Write(bytes,8);
}
}
uint64_t OrchardSnapshotNodeCount(const OrchardSnapshotSection& s) {
    Check(!s.disk_nodes||s.nodes.empty());return s.disk_nodes?s.disk_nodes->Size():s.nodes.size();
}
std::string ReadOrchardSnapshotNode(const OrchardSnapshotSection& s,const uint256& id) {
    Check(!s.disk_nodes||s.nodes.empty());if(s.disk_nodes)return s.disk_nodes->Read(id);
    const auto it=s.nodes.find(id);Check(it!=s.nodes.end());return it->second;
}
void ForEachOrchardSnapshotNode(const OrchardSnapshotSection& s,
    const std::function<void(const uint256&,const std::string&)>& visitor) {
    Check(!s.disk_nodes||s.nodes.empty());if(s.disk_nodes)s.disk_nodes->ForEach(visitor);
    else for(const auto& [id,bytes]:s.nodes)visitor(id,bytes);
}
namespace {
std::vector<uint8_t> NormalizeForestFrame(const std::vector<uint8_t>& bytes) {
    Check(bytes.size()<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);Reader r{bytes};
    Check(r.Number(1)==3);const auto flag=r.Number(1);Check(flag<=1);
    const auto leaves=r.Number(8);Check(leaves<=MAX_UTREEXO_LEAVES&&leaves<=bytes.size());
    const auto count=r.Number(4);Check(count<=64);const auto roots=r.Raw(count*32);
    const auto nodes=r.Number(4);Check(nodes>=leaves&&nodes<=r.bytes.size());
    const auto node_bytes=r.bytes;
    for(uint64_t i=0;i<nodes;++i){const auto present=r.Number(1);Check(present<=1);if(present)r.Raw(32);}
    const auto deleted_count=r.Number(4);Check(deleted_count<=leaves&&deleted_count<=r.bytes.size()/8);
    const auto deleted_bytes=r.Raw(deleted_count*8);Check(r.bytes.empty());Reader deleted{deleted_bytes};
    std::optional<uint64_t> previous;
    for(uint64_t i=0;i<deleted_count;++i) {
        const auto position=deleted.Number(8);Check(position<leaves&&(!previous||position>*previous));previous=position;
    }
    Writer w;w.Number(3,1);w.Number(flag,1);w.Number(leaves,8);w.Number(count,4);w.Raw(roots);w.Number(leaves,4);
    Reader material{node_bytes};deleted={deleted_bytes};uint64_t remaining=deleted_count;
    uint64_t next_deleted=remaining?deleted.Number(8):UINT64_MAX;
    for(uint64_t i=0;i<leaves;++i) {
        const auto present=material.Number(1);const auto hash=present?material.Raw(32):std::span<const uint8_t>{};
        if(i==next_deleted) {
            w.Number(0,1);--remaining;next_deleted=remaining?deleted.Number(8):UINT64_MAX;
        } else {Check(present==1);w.Number(1,1);w.Raw(hash);}
    }
    w.Number(deleted_count,4);w.Raw(deleted_bytes);
    return std::move(w.bytes);
}
}
std::vector<uint8_t> CanonicalOrchardSnapshotForest(const std::vector<uint8_t>& bytes) {
    auto canonical=NormalizeForestFrame(bytes);
    const auto forest=UtreexoForest::deserialize(canonical);Check(forest.serialize()==canonical);return canonical;
}
UtreexoForest DecodeOrchardSnapshotForest(const std::vector<uint8_t>& bytes) {
    // Screen canonical framing before allocation, then rebuild/verify exactly
    // once. Calling the public normalizer here would rebuild a second forest.
    Check(NormalizeForestFrame(bytes)==bytes);auto forest=UtreexoForest::deserialize(bytes);
    Check(forest.serialize()==bytes);return forest;
}
storage::OrchardCommitmentSets ComputeOrchardSnapshotSets(const OrchardSnapshotSection& s) {
    storage::OrchardCommitmentSets result;crypto::CSHA256 nullifiers,anchors;
    nullifiers.Write(std::string("ONF1\x01",5));anchors.Write(std::string("OAN1\x01",5));
    const OrchardSnapshotRawLess less;
    std::optional<uint256> previous;
    for(const auto& n:s.nullifiers) {
        Check(!previous||less(*previous,n));previous=n;nullifiers.Write(n.data,32);++result.nullifier_count;
    }
    HashNumber(nullifiers,result.nullifier_count);nullifiers.Finalize(result.nullifiers.data);
    previous.reset();
    for(const auto& a:s.anchors) {
        Check((!previous||less(*previous,a.anchor))&&a.references&&
            a.references<=UINT64_MAX-result.anchor_references);
        previous=a.anchor;anchors.Write(a.anchor.data,32);HashNumber(anchors,a.references);
        ++result.anchor_count;result.anchor_references+=a.references;
    }
    HashNumber(anchors,result.anchor_count);HashNumber(anchors,result.anchor_references);anchors.Finalize(result.anchors.data);
    return result;
}
void ValidateOrchardSnapshotSection(const OrchardSnapshotSection& s) {
    Check(s.profile==OrchardSnapshotProfile::Full||s.profile==OrchardSnapshotProfile::Compact);
    const auto& state=s.state;const auto& retired=s.retirement;const auto& r=retired.record;
    Check(r.network_code<=2&&!r.genesis.IsNull()&&r.branch_id&&r.activation_height&&r.activation_height!=UINT32_MAX&&
        r.legacy_epoch_height<r.activation_height&&!r.boundary_parent.IsNull()&&
        state.height>=r.activation_height&&state.height<=uint32_t(INT32_MAX)&&!state.block_hash.IsNull()&&
        state.block_hash!=r.boundary_parent&&r.retired_value<=MAX_MONEY&&state.pool_balance<=MAX_MONEY-r.retired_value&&
        state.tree_size<=(uint64_t(1)<<32)&&!state.frontier.empty()&&state.frontier.size()<=storage::ORCHARD_STORED_FRONTIER_LIMIT&&
        retired.height==state.height&&retired.block_hash==state.block_hash&&!retired.parent_hash.IsNull()&&
        retired.parent_hash!=state.block_hash&&
        (state.height!=r.activation_height||retired.parent_hash==r.boundary_parent));
    const auto sets=ComputeOrchardSnapshotSets(s);
    Check(sets.nullifier_count==state.tree_size&&sets.anchor_count&&
        sets.anchor_references==uint64_t(state.height)-r.activation_height+1);
    Check(std::any_of(s.anchors.begin(),s.anchors.end(),[&](const auto& a){return a.anchor==state.anchor;}));
    const auto& parent=s.parent_catalog;const auto& base=s.base_catalog;
    parent.Validate();base.Validate();
    for(const auto* catalog:{&parent,&base})
        Check(catalog->network==r.network_code&&catalog->genesis==r.genesis&&catalog->branch==r.branch_id&&
            catalog->activation==r.activation_height&&catalog->leaf_activation==parent.leaf_activation);
    Check(uint64_t(parent.height)+1==state.height&&parent.block==retired.parent_hash&&
        base.height==state.height&&base.block==state.block_hash&&base.parent==parent.block&&base.work>parent.work&&
        base.previous_record==storage::catalog::Hash(parent.Encode()));
    Check(!s.base_body.empty()&&s.base_body.size()<=ORCHARD_SNAPSHOT_MAX_BASE_BODY_BYTES&&!s.accumulator.empty()&&
        s.accumulator.size()<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);
    if(s.profile==OrchardSnapshotProfile::Compact)Check(s.accumulator==base.stump);
    Check(!s.disk_nodes||s.nodes.empty());
    if(s.disk_nodes){s.disk_nodes->ValidateReachability(parent,base);return;}
    // Pointers to immutable map entries require 8 bytes per visit, rather than
    // a separately allocated 32-byte-key tree for every reachable node. Parent
    // and base trees may share records; sort/unique after complete traversal.
    std::vector<const void*> reached;reached.reserve(s.nodes.size());
    const auto read=[&](const uint256& id) {
        const auto it=s.nodes.find(id);Check(it!=s.nodes.end());reached.push_back(&*it);return it->second;
    };
    for(const auto* catalog:{&parent,&base}) {
        const auto walk=[&](storage::catalog::Kind kind,const uint256& root,uint64_t count) {
            storage::catalog::Tree(kind,read).ForEachNode(root,count,[&](const auto& id,const auto& bytes) {
                Check(bytes.size()<=ORCHARD_SNAPSHOT_MAX_NODE_BYTES);return true;
            });
        };
        walk(storage::catalog::Kind::Transactions,catalog->transactions,catalog->transaction_count);
        walk(storage::catalog::Kind::LegacyCoins,catalog->legacy,catalog->legacy_count);
        walk(storage::catalog::Kind::NonTransparentCoins,catalog->nontransparent,catalog->nontransparent_count);
    }
    std::sort(reached.begin(),reached.end(),std::less<const void*>{});
    Check(size_t(std::unique(reached.begin(),reached.end())-reached.begin())==s.nodes.size());
}
namespace {
void EncodeSection(Writer& w,const OrchardSnapshotSection& s) {
    w.Raw(std::string("ORCH"));w.Number(ORCHARD_SNAPSHOT_SECTION_VERSION,4);
    w.Number(uint8_t(s.profile),1);State(w,s.state);Retirement(w,s.retirement);
    w.Number(s.nullifiers.size(),8);for(const auto& n:s.nullifiers)w.Hash(n);
    w.Number(s.anchors.size(),8);for(const auto& a:s.anchors){w.Hash(a.anchor);w.Number(a.references,8);}
    w.Blob(s.parent_catalog.Encode());w.Blob(s.base_catalog.Encode());
    w.Number(OrchardSnapshotNodeCount(s),8);
    if(w.counting&&s.disk_nodes) {
        const auto size=s.disk_nodes->WireBytes();Check(size<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES-w.used);w.used+=size;
    } else ForEachOrchardSnapshotNode(s,[&](const auto& id,const auto& bytes){w.Hash(id);w.Blob(bytes);});
    w.Blob(s.base_body);w.Blob(s.accumulator);
}
template<class R>OrchardSnapshotSection DecodeSection(R& r,bool disk,const std::filesystem::path& workspace={}) {
    const auto magic=r.Raw(4);Check(std::memcmp(magic.data(),"ORCH",4)==0&&r.Number(4)==ORCHARD_SNAPSHOT_SECTION_VERSION);
    OrchardSnapshotSection s;s.profile=OrchardSnapshotProfile(r.Number(1));
    Check(s.profile==OrchardSnapshotProfile::Full||s.profile==OrchardSnapshotProfile::Compact);
    s.state=State(r);s.retirement=Retirement(r);
    auto count=r.Number(8);Check(count<=r.Remaining()/32&&count==s.state.tree_size);s.nullifiers.reserve(size_t(count));
    for(uint64_t i=0;i<count;++i)s.nullifiers.push_back(r.Hash());
    count=r.Number(8);Check(count<=r.Remaining()/40);s.anchors.reserve(size_t(count));
    for(uint64_t i=0;i<count;++i){const auto anchor=r.Hash();const auto refs=r.Number(8);s.anchors.push_back({anchor,refs});}
    s.parent_catalog=storage::catalog::State::Decode(r.Text(2493));s.base_catalog=storage::catalog::State::Decode(r.Text(2493));
    count=r.Number(8);Check(count<=r.Remaining()/49);std::optional<uint256> previous;
    auto inventory=disk?std::make_shared<OrchardSnapshotCatalog>(workspace):nullptr;
    for(uint64_t i=0;i<count;++i) {
        const auto id=r.Hash();Check(!previous||OrchardSnapshotRawLess{}(*previous,id));previous=id;
        auto value=r.Text(ORCHARD_SNAPSHOT_MAX_NODE_BYTES);Check(storage::catalog::ValidNode(id,value));
        if(inventory)inventory->InsertUnique(id,value);
        else Check(s.nodes.emplace(id,std::move(value)).second);
    }
    if(inventory){inventory->Seal();s.disk_nodes=std::move(inventory);}
    s.base_body=r.ByteVector(ORCHARD_SNAPSHOT_MAX_BASE_BODY_BYTES);
    const auto accumulator_limit=s.profile==OrchardSnapshotProfile::Compact?uint64_t(9+64*33):ORCHARD_SNAPSHOT_MAX_SECTION_BYTES;
    s.accumulator=r.ByteVector(accumulator_limit);Check(r.Remaining()==0);
    ValidateOrchardSnapshotSection(s);return s;
}
}
std::vector<uint8_t> EncodeOrchardSnapshotSection(const OrchardSnapshotSection& s) {
    ValidateOrchardSnapshotSection(s);Writer measure;measure.counting=true;EncodeSection(measure,s);
    Writer w;w.bytes.reserve(size_t(measure.used));EncodeSection(w,s);return std::move(w.bytes);
}
uint64_t OrchardSnapshotSectionEncodedSize(const OrchardSnapshotSection& s) {
    ValidateOrchardSnapshotSection(s);Writer w;w.counting=true;EncodeSection(w,s);return w.used;
}
uint64_t StreamOrchardSnapshotSection(const OrchardSnapshotSection& s,const OrchardSnapshotSink& sink) {
    Check(bool(sink));ValidateOrchardSnapshotSection(s);Writer w;w.sink=&sink;EncodeSection(w,s);return w.used;
}
uint256 HashOrchardSnapshotSection(const OrchardSnapshotSection& s) {
    crypto::CSHA256 hash;StreamOrchardSnapshotSection(s,[&](auto bytes){hash.Write(bytes.data(),bytes.size());});
    uint256 result;hash.Finalize(result.data);return result;
}
OrchardSnapshotSection DecodeOrchardSnapshotSection(std::span<const uint8_t> bytes) {
    Check(bytes.size()<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);Reader r{bytes};return DecodeSection(r,false);
}
OrchardSnapshotSection ReadOrchardSnapshotSection(uint64_t size,const OrchardSnapshotSource& source,const std::filesystem::path& workspace) {
    Check(bool(source)&&size<=ORCHARD_SNAPSHOT_MAX_SECTION_BYTES);StreamReader r{size,source};return DecodeSection(r,true,workspace);
}

void VerifyOrchardSnapshotSection(const OrchardSnapshotSection& s,OrchardSnapshotProfile expected_profile,
    const OrchardBlockContext& context,const BlockHeader& parent,const BlockHeader& base,
    const arith_uint256& parent_work,const arith_uint256& base_work,
    const std::optional<uint256>& selected_ancestor_at_base,uint32_t best_header_height,
    uint32_t burial_depth,const OrchardBranchMtpLookup& mtp,bool witness) {
    Check(s.profile==expected_profile);
    Check(context.height==s.state.height&&context.block_hash==s.state.block_hash&&
        context.parent_hash==s.retirement.parent_hash&&parent.GetHash()==context.parent_hash&&
        base.GetHash()==context.block_hash&&base.prev_block_hash==context.parent_hash&&
        context.activation_height==s.retirement.record.activation_height&&
        base_work>parent_work&&parent_work+GetBlockProof(base.difficulty)==base_work&&
        s.parent_catalog.work==parent_work&&s.base_catalog.work==base_work);
    Check(EvaluateSnapshotBurial(context.block_hash,context.height,selected_ancestor_at_base,
        best_header_height,burial_depth)==SnapshotBindingVerdict::Ok);
    // Reject unknown/unburied bases before walking an attacker-supplied
    // catalog. The outer importer should also screen transport and counts
    // before materializing this staged in-memory representation.
    ValidateOrchardSnapshotSection(s);
    const auto block=OrchardBlockCandidate::DecodeExact(s.base_body);std::string error;
    Check(block.Header().SerializeForHash()==base.SerializeForHash()&&
        block.CheckSizeLimits(error)&&block.CheckIdentityCommitments(witness,error));
    const auto sets=ComputeOrchardSnapshotSets(s);
    const auto root=ComputeOrchardCatalogStateRoot(
        {context.domain,context.activation_height,context.height,context.parent_hash},
        s.retirement.record,s.state,sets,s.parent_catalog);
    const auto commitment=FindStateCommitment(block.Transactions().at(0).Historical(),StateCommitmentEncoding::Orchard);
    Check(commitment.status==StateCommitmentStatus::Ok&&commitment.root==root);
    OrchardSnapshotCatalog pending(s.disk_nodes?s.disk_nodes->WorkDirectory():std::filesystem::path{});
    const auto read=[&](const uint256& id) {
        if(const auto value=pending.Find(id))return *value;
        return ReadOrchardSnapshotNode(s,id);
    };
    const auto write=[&](const uint256& id,const std::string& bytes) {
        pending.Insert(id,bytes);
    };
    const auto view=CaptureOrchardCatalogCoins(block,context,parent,s.parent_catalog,read);
    const auto coins=PrepareOrchardBlockCoinsUnderChainstateLock(block,context,view,mtp,witness);
    (void)CheckOrchardBlockFilter(block,coins);
    const auto stump=DeriveOrchardCompactBlockStump(block,coins,parent,UtreexoStump::deserialize(s.parent_catalog.stump));
    const auto derived=AdvanceOrchardCatalog(s.parent_catalog,coins,stump,base_work,
        BuildOrchardCoinUndo(coins).Serialize(),read,write);
    Check(derived.Encode()==s.base_catalog.Encode());
    if(s.profile==OrchardSnapshotProfile::Full) {
        const auto forest=DecodeOrchardSnapshotForest(s.accumulator);
        Check(UtreexoStump::fromForest(forest).serialize()==s.base_catalog.stump);
    } else Check(s.accumulator==stump.serialize());
}
} // namespace dinero::consensus

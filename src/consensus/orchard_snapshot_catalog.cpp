#include "consensus/orchard_snapshot_catalog.h"
#include "wallet/runtime_replay_spool.h"
#include <mutex>
#include <map>
#include <tuple>
#include <cstring>

namespace dinero::consensus {
namespace {
using Spool=wallet::detail::RuntimeReplaySpool;
std::vector<uint8_t> Key(const uint256& id){std::vector<uint8_t> key{1};key.insert(key.end(),id.begin(),id.end());return key;}
void Check(bool ok){if(!ok)throw std::runtime_error("Orchard snapshot disk catalog inconsistent");}
struct RawLess {bool operator()(const uint256& a,const uint256& b)const{return std::memcmp(a.data,b.data,32)<0;}};
uint256 RootsIdentity(const storage::catalog::State& parent,const storage::catalog::State& base) {
    std::string roots="DNO-SNAPSHOT-REACH-1";
    for(const auto* state:{&parent,&base}) {
        for(const auto& root:{state->transactions,state->legacy,state->nontransparent})roots.append(reinterpret_cast<const char*>(root.data),32);
        for(const auto count:{state->transaction_count,state->legacy_count,state->nontransparent_count})storage::catalog::Number(roots,count,8);
    }
    return storage::catalog::Hash(roots);
}
template<class Reader>void WalkRoots(const storage::catalog::State& parent,const storage::catalog::State& base,const Reader& reader) {
    struct Walked{storage::catalog::Kind kind;uint256 root;uint64_t count;};
    std::vector<Walked> walked;walked.reserve(6);
    for(const auto* state:{&parent,&base}) {
        const auto walk=[&](storage::catalog::Kind kind,const uint256& root,uint64_t count) {
            bool first_kind=true;
            for(const auto& prior:walked)if(prior.kind==kind) {
                first_kind=false;
                if(prior.root==root){Check(prior.count==count);return;}
            }
            storage::catalog::Tree(kind,reader(first_kind)).ForEachNode(root,count,[](const auto&,const auto&){return true;});
            walked.push_back({kind,root,count});
        };
        walk(storage::catalog::Kind::Transactions,state->transactions,state->transaction_count);
        walk(storage::catalog::Kind::LegacyCoins,state->legacy,state->legacy_count);
        walk(storage::catalog::Kind::NonTransparentCoins,state->nontransparent,state->nontransparent_count);
    }
}
// The ordinal is private scratch metadata, MAC-bound to the record key/value,
// and never changes snapshot wire bytes. Build this new, disposable inventory
// in one unpublished transaction: recommitting small batches made later random
// inserts repeatedly journal the same old pages. The pager still spills to
// disk at its fixed 16 MiB catalog cache bound and the complete wire bound is
// 1 GiB. The generic wallet replay spool retains its 1 MiB default.
struct Inventory {
    Spool spool;std::unique_ptr<Spool::Batch> batch;uint64_t count=0,wire_bytes=0;bool sealed=false;
    explicit Inventory(const std::filesystem::path& work_directory={}):spool(work_directory,true,16*1024){}
    std::pair<uint64_t,std::string> Decode(const Spool::Bytes& bytes)const {
        Check(bytes.size()>=8);uint64_t ordinal=0;
        for(unsigned i=0;i<8;++i)ordinal|=uint64_t(bytes[i])<<(8*i);
        Check(ordinal<count);
        return {ordinal,std::string(bytes.begin()+8,bytes.end())};
    }
    void Add(const uint256& id,const std::string& value,bool unique) {
        // The private MAC is minted only after the content-address and node
        // encoding check. Future successful MAC reads therefore establish
        // these exact same id/bytes without another software SHA-256 pass.
        Check(!sealed&&storage::catalog::ValidNode(id,value));
        const auto key=Key(id);
        if(!unique)if(const auto old=spool.Get(key)) {
            Check(Decode(*old).second==value);return;
        }
        if(!batch)batch=std::make_unique<Spool::Batch>(spool);
        Check(value.size()+40<=(uint64_t(1)<<30)-wire_bytes);
        Spool::Bytes record;record.reserve(8+value.size());
        for(unsigned i=0;i<8;++i)record.push_back(uint8_t(count>>(8*i)));
        record.insert(record.end(),value.begin(),value.end());spool.Insert(key,record);++count;
        wire_bytes+=40+value.size();
    }
    void Flush(){if(batch){batch->Commit();batch.reset();}}
    void Seal(){Check(!sealed);Flush();spool.Freeze();sealed=true;}
    struct Cursor {
        const Inventory& owner;Spool::Bytes after{1};uint64_t count=0;size_t at=0;
        std::vector<std::pair<Spool::Bytes,Spool::Bytes>> rows;
        std::optional<std::tuple<uint256,uint64_t,std::string>> Next() {
            if(at==rows.size()){rows=owner.spool.NextBatch(1,after);at=0;}
            if(rows.empty()){Check(count==owner.count);return {};}
            const auto& row=rows[at++];Check(row.first.size()==33);after=row.first;
            uint256 id;std::copy(row.first.begin()+1,row.first.end(),id.begin());
            auto [ordinal,value]=owner.Decode(row.second);
            Check(++count<=owner.count);return std::tuple{id,ordinal,std::move(value)};
        }
    };
};
}
struct OrchardSnapshotCatalog::Impl {
    const std::filesystem::path work_directory;
    Inventory records;
    std::shared_ptr<const OrchardSnapshotCatalog> parent;
    std::vector<uint8_t> included;
    uint64_t included_count=0,included_bytes=0;
    explicit Impl(const std::filesystem::path& path):work_directory(path),records(path){}
    mutable std::mutex validation;
    mutable std::optional<uint256> validated_roots;
    std::optional<std::pair<uint64_t,std::string>> Lookup(const uint256& id)const {
        if(parent)if(const auto raw=parent->impl_->records.spool.Get(Key(id))) {
            auto value=parent->impl_->records.Decode(*raw);
            if(!(included[size_t(value.first/8)]&uint8_t(1u<<(value.first%8))))return {};
            return value;
        }
        const auto raw=records.spool.Get(Key(id));if(!raw)return {};
        auto value=records.Decode(*raw);if(parent)value.first+=parent->Size();return value;
    }
};
OrchardSnapshotCatalog::OrchardSnapshotCatalog(const std::filesystem::path& path):impl_(std::make_unique<Impl>(path)){}
OrchardSnapshotCatalog::~OrchardSnapshotCatalog()=default;
std::shared_ptr<const OrchardSnapshotCatalog> OrchardSnapshotCatalog::Project(
    std::shared_ptr<const OrchardSnapshotCatalog> original,const storage::catalog::State& parent,
    const storage::catalog::State& base,const std::function<std::string(const uint256&)>& load) {
    Check(original&&original->impl_->records.sealed&&!original->impl_->parent&&bool(load));
    auto out=std::make_shared<OrchardSnapshotCatalog>(original->WorkDirectory());auto& target=*out->impl_;
    target.parent=std::move(original);target.included.resize(size_t((target.parent->Size()+7)/8),0);
    // Missing-node callbacks run outside every read view and write batch.
    // Only a bounded <=1024-node buffer waits for a sorted overlay flush.
    std::map<uint256,std::string,RawLess> pending;uint64_t pending_bytes=0;
    std::unique_ptr<Spool::ReadBatch> view=std::make_unique<Spool::ReadBatch>(target.parent->impl_->records.spool);
    const auto space=[&]{Check(target.included_bytes+target.records.wire_bytes+pending_bytes<=(uint64_t(1)<<30));};
    const auto flush=[&] {
        if(pending.empty())return;
        view.reset();
        for(const auto& [id,bytes]:pending)target.records.Add(id,bytes,true);
        target.records.Flush();pending.clear();pending_bytes=0;
        view=std::make_unique<Spool::ReadBatch>(target.parent->impl_->records.spool);
    };
    WalkRoots(parent,base,[&](bool first_kind) {
        return [&,first_kind](const uint256& id) {
            if(const auto raw=target.parent->impl_->records.spool.Get(Key(id))) {
                auto [ordinal,bytes]=target.parent->impl_->records.Decode(*raw);
                auto& byte=target.included[size_t(ordinal/8)];const auto bit=uint8_t(1u<<(ordinal%8));
                Check(!first_kind||!(byte&bit));
                if(!(byte&bit)){byte|=bit;++target.included_count;target.included_bytes+=40+bytes.size();space();}
                return bytes;
            }
            if(const auto raw=target.records.spool.Get(Key(id))) {
                Check(!first_kind);auto [ordinal,bytes]=target.records.Decode(*raw);return bytes;
            }
            if(const auto row=pending.find(id);row!=pending.end()){Check(!first_kind);return row->second;}
            view.reset();auto bytes=load(id);Check(storage::catalog::ValidNode(id,bytes));
            pending.emplace(id,bytes);pending_bytes+=40+bytes.size();space();
            view=std::make_unique<Spool::ReadBatch>(target.parent->impl_->records.spool);
            if(pending.size()==1024)flush();return bytes;
        };
    });
    flush();view.reset();out->Seal();space();
    // Exactly the same complete six-root structural walk as validation. The
    // projection contains only records observed by that walk. Cached uses
    // still authenticate every private record, and the live-source comparison
    // remains the caller's separate mandatory responsibility.
    target.validated_roots=RootsIdentity(parent,base);return out;
}
void OrchardSnapshotCatalog::Insert(const uint256& id,const std::string& value){impl_->records.Add(id,value,false);}
void OrchardSnapshotCatalog::InsertUnique(const uint256& id,const std::string& value){impl_->records.Add(id,value,true);}
void OrchardSnapshotCatalog::Seal(){impl_->records.Seal();}
uint64_t OrchardSnapshotCatalog::Size()const{Check(impl_->records.sealed);return impl_->included_count+impl_->records.count;}
uint64_t OrchardSnapshotCatalog::WireBytes()const{Check(impl_->records.sealed);return impl_->included_bytes+impl_->records.wire_bytes;}
const std::filesystem::path& OrchardSnapshotCatalog::WorkDirectory()const{return impl_->work_directory;}
std::string OrchardSnapshotCatalog::Read(const uint256& id)const {
    Check(impl_->records.sealed);const auto value=Find(id);Check(bool(value));return *value;
}
std::optional<std::string> OrchardSnapshotCatalog::Find(const uint256& id)const {
    auto value=impl_->Lookup(id);if(!value)return {};
    auto [ordinal,bytes]=std::move(*value);
    return bytes;
}
void OrchardSnapshotCatalog::ForEach(const std::function<void(const uint256&,const std::string&)>& visitor)const {
    Check(impl_->records.sealed&&bool(visitor));uint64_t count=0;
    Inventory::Cursor own{impl_->records};
    auto next=own.Next();
    std::unique_ptr<Inventory::Cursor> parent;
    if(impl_->parent)parent=std::make_unique<Inventory::Cursor>(Inventory::Cursor{impl_->parent->impl_->records});
    const auto previous=[&]()->std::optional<std::tuple<uint256,uint64_t,std::string>> {
        while(parent) {
            auto row=parent->Next();if(!row)return {};
            const auto ordinal=std::get<1>(*row);
            if(impl_->included[size_t(ordinal/8)]&uint8_t(1u<<(ordinal%8)))return row;
        }
        return {};
    };
    auto prior=previous();
    while(next||prior) {
        Check(!next||!prior||std::get<0>(*next)!=std::get<0>(*prior));
        const bool old=prior&&(!next||RawLess{}(std::get<0>(*prior),std::get<0>(*next)));
        const auto& row=old?*prior:*next;visitor(std::get<0>(row),std::get<2>(row));
        Check(++count<=Size());if(old)prior=previous();else next=own.Next();
    }
    Check(count==Size());
}
void OrchardSnapshotCatalog::ValidateReachability(const storage::catalog::State& parent,const storage::catalog::State& base)const {
    Check(impl_->records.sealed);std::lock_guard lock(impl_->validation);
    // Only cache the structural walk of immutable, process-MAC-checked records.
    // The key covers all six kinds/roots/counts. No domain, frontier, body,
    // header, checksum or selected-chain authentication is cached here.
    const auto identity=RootsIdentity(parent,base);
    if(impl_->validated_roots==identity) {
        // Immutable ownership is not permission to ignore disk corruption or
        // I/O errors. Recheck every key/value MAC and the complete count. The
        // MAC authenticates the content hash/encoding checked before tagging;
        // it grants no domain, selected-state, body or proof authority.
        ForEach([](const auto&,const auto&){});return;
    }
    // A MAC-bound ordinal lets the complete walk mark one bit per stored
    // record instead of building a second randomly written SQLite inventory.
    // Each flat inventory's 1 GiB wire bound caps its marks at 3.2 MiB. A
    // projection addresses two such inventories, never a chain of overlays.
    // No node values/keys live in this bitmap.
    Check(Size()<=(uint64_t(1)<<30)/40);
    const auto slots=impl_->records.count+(impl_->parent?impl_->parent->Size():0);
    std::vector<uint8_t> reached(size_t((slots+7)/8),0);uint64_t reached_count=0;
    std::unique_ptr<Spool::ReadBatch> parent_view;
    if(impl_->parent)parent_view=std::make_unique<Spool::ReadBatch>(impl_->parent->impl_->records.spool);
    Spool::ReadBatch view(impl_->records.spool);
    WalkRoots(parent,base,[&](bool first_kind) {
            return [&,first_kind](const uint256& id) {
                auto record=impl_->Lookup(id);Check(bool(record));auto [ordinal,bytes]=std::move(*record);
                auto& byte=reached[size_t(ordinal/8)];const auto bit=uint8_t(1u<<(ordinal%8));
                Check(!first_kind||!(byte&bit));
                if(!(byte&bit)){byte|=bit;++reached_count;}
                return bytes;
            };
    });
    Check(reached_count==Size());impl_->validated_roots=identity;
}
} // namespace dinero::consensus

#include "consensus/orchard_snapshot_catalog.h"
#include "wallet/runtime_replay_spool.h"
#include <mutex>

namespace dinero::consensus {
namespace {
using Spool=wallet::detail::RuntimeReplaySpool;
std::vector<uint8_t> Key(const uint256& id){std::vector<uint8_t> key{1};key.insert(key.end(),id.begin(),id.end());return key;}
void Check(bool ok){if(!ok)throw std::runtime_error("Orchard snapshot disk catalog inconsistent");}
// Fixed pager and transaction bounds, independent of the snapshot's node count.
struct Inventory {
    Spool spool;std::unique_ptr<Spool::Batch> batch;uint64_t count=0,wire_bytes=0;unsigned pending=0;bool sealed=false;
    explicit Inventory(const std::filesystem::path& work_directory={}):spool(work_directory,true){}
    void Add(const uint256& id,const std::string& value,bool unique) {
        Check(!sealed);
        const auto key=Key(id);
        if(!unique)if(const auto old=spool.Get(key)) {
            Check(std::string(old->begin(),old->end())==value);return;
        }
        if(!batch)batch=std::make_unique<Spool::Batch>(spool);
        Check(value.size()+40<=(uint64_t(1)<<30)-wire_bytes);
        spool.Insert(key,{reinterpret_cast<const uint8_t*>(value.data()),value.size()});++count;
        wire_bytes+=40+value.size();
        if(++pending==1024){batch->Commit();batch.reset();pending=0;}
    }
    void Seal(){Check(!sealed);if(batch){batch->Commit();batch.reset();}spool.Freeze();sealed=true;}
    void CheckMarks()const {
        Check(sealed);Spool::Bytes after{1};uint64_t actual=0;
        while(const auto row=spool.Next(1,after)) {
            Check(row->first.size()==33&&row->second.empty()&&++actual<=count);after=row->first;
        }
        Check(actual==count);
    }
};
}
struct OrchardSnapshotCatalog::Impl {
    const std::filesystem::path work_directory;
    Inventory records;
    explicit Impl(const std::filesystem::path& path):work_directory(path),records(path){}
    mutable std::mutex validation;
    mutable std::optional<uint256> validated_roots;
};
OrchardSnapshotCatalog::OrchardSnapshotCatalog(const std::filesystem::path& path):impl_(std::make_unique<Impl>(path)){}
OrchardSnapshotCatalog::~OrchardSnapshotCatalog()=default;
void OrchardSnapshotCatalog::Insert(const uint256& id,const std::string& value){Check(storage::catalog::ValidNode(id,value));impl_->records.Add(id,value,false);}
void OrchardSnapshotCatalog::InsertUnique(const uint256& id,const std::string& value){Check(storage::catalog::ValidNode(id,value));impl_->records.Add(id,value,true);}
void OrchardSnapshotCatalog::Seal(){impl_->records.Seal();}
uint64_t OrchardSnapshotCatalog::Size()const{Check(impl_->records.sealed);return impl_->records.count;}
uint64_t OrchardSnapshotCatalog::WireBytes()const{Check(impl_->records.sealed);return impl_->records.wire_bytes;}
const std::filesystem::path& OrchardSnapshotCatalog::WorkDirectory()const{return impl_->work_directory;}
std::string OrchardSnapshotCatalog::Read(const uint256& id)const {
    Check(impl_->records.sealed);const auto value=Find(id);Check(bool(value));return *value;
}
std::optional<std::string> OrchardSnapshotCatalog::Find(const uint256& id)const {
    const auto value=impl_->records.spool.Get(Key(id));if(!value)return {};
    std::string bytes(value->begin(),value->end());Check(storage::catalog::ValidNode(id,bytes));return bytes;
}
void OrchardSnapshotCatalog::ForEach(const std::function<void(const uint256&,const std::string&)>& visitor)const {
    Check(impl_->records.sealed&&bool(visitor));Spool::Bytes after{1};uint64_t count=0;
    while(const auto row=impl_->records.spool.Next(1,after)) {
        Check(row->first.size()==33);uint256 id;std::copy(row->first.begin()+1,row->first.end(),id.begin());
        const std::string value(row->second.begin(),row->second.end());Check(storage::catalog::ValidNode(id,value));
        visitor(id,value);after=row->first;Check(++count<=impl_->records.count);
    }
    Check(count==impl_->records.count);
}
void OrchardSnapshotCatalog::ValidateReachability(const storage::catalog::State& parent,const storage::catalog::State& base)const {
    Check(impl_->records.sealed);std::lock_guard lock(impl_->validation);
    // Only cache the structural walk of immutable, process-MAC-checked records.
    // The key covers all six kinds/roots/counts. No domain, frontier, body,
    // header, checksum or selected-chain authentication is cached here.
    std::string roots="DNO-SNAPSHOT-REACH-1";
    for(const auto* state:{&parent,&base}) {
        for(const auto& root:{state->transactions,state->legacy,state->nontransparent})roots.append(reinterpret_cast<const char*>(root.data),32);
        for(const auto count:{state->transaction_count,state->legacy_count,state->nontransparent_count})storage::catalog::Number(roots,count,8);
    }
    const auto identity=storage::catalog::Hash(roots);
    if(impl_->validated_roots==identity) {
        // Immutable ownership is not permission to ignore disk corruption or
        // I/O errors. Recheck every MAC/hash/count, without another random-I/O
        // Patricia walk or a catalog-sized in-memory visited set.
        ForEach([](const auto&,const auto&){});return;
    }
    Inventory reached(impl_->work_directory);
    const auto read=[&](const uint256& id){return Read(id);};
    struct Walked{storage::catalog::Kind kind;uint256 root;uint64_t count;};
    std::vector<Walked> walked;walked.reserve(6);
    for(const auto* state:{&parent,&base}) {
        const auto walk=[&](storage::catalog::Kind kind,const uint256& root,uint64_t count) {
            bool first_kind=true;
            for(const auto& prior:walked)if(prior.kind==kind) {
                first_kind=false;
                if(prior.root==root){Check(prior.count==count);return;}
            }
            storage::catalog::Tree(kind,read).ForEachNode(root,count,[&](const auto& id,const auto&) {
                reached.Add(id,"",first_kind);Check(reached.count<=Size());return true;
            });
            walked.push_back({kind,root,count});
        };
        walk(storage::catalog::Kind::Transactions,state->transactions,state->transaction_count);
        walk(storage::catalog::Kind::LegacyCoins,state->legacy,state->legacy_count);
        walk(storage::catalog::Kind::NonTransparentCoins,state->nontransparent,state->nontransparent_count);
    }
    reached.Seal();reached.CheckMarks();Check(reached.count==Size());impl_->validated_roots=identity;
}
} // namespace dinero::consensus

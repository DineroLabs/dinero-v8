#include "daemon/runtime_reorg_store.h"
#include "consensus/orchard_profile.h"
#include "consensus/merkle_root.h"
#include "crypto/sha256.h"
#include "storage/chain_db.h"
#include <rocksdb/write_batch.h>
#include <cstring>

namespace dinero {
namespace {
using consensus::OrchardStateLookupError;
constexpr size_t max_bytes=64*1024*1024, max_blocks=2048;
constexpr const char* head_key="runtime_reorg_intent:v1:head";
[[noreturn]] void Fail(Status status=Status::Corruption) { throw OrchardStateLookupError(status); }
std::optional<std::string> Get(const ChainDB& db,const std::string& key) {
    std::string value;auto status=db.getRaw(key,value);
    if(status==Status::NotFound)return {};if(status!=Status::Ok)Fail(status);return value;
}
std::string Key(uint64_t n) {
    std::string key="runtime_reorg_intent:v1:record:";
    for(int shift=60;shift>=0;shift-=4)key.push_back("0123456789abcdef"[(n>>shift)&15]);return key;
}
void Number(std::string& s,uint64_t n,size_t width) {
    for(size_t i=0;i<width;++i)s.push_back(static_cast<char>(n>>(8*i)));
}
void Hash(std::string& s,const uint256& hash) { s.append(reinterpret_cast<const char*>(hash.data),32); }
uint256 Digest(std::string_view s) {
    uint256 hash;crypto::CSHA256().Write(reinterpret_cast<const uint8_t*>(s.data()),s.size()).Finalize(hash.data);return hash;
}
void Seal(std::string& s) { Hash(s,Digest(s)); }
struct Reader {
    std::string_view s;
    std::string_view Take(size_t n) { if(n>s.size())Fail();auto v=s.substr(0,n);s.remove_prefix(n);return v; }
    uint64_t Number(size_t n) {auto s=Take(n);uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(uint8_t(s[i]))<<(8*i);return v;}
    uint256 Hash() {auto s=Take(32);uint256 h;std::memcpy(h.data,s.data(),32);return h;}
    RuntimeOutboxCursor Cursor() {return {Number(8),Hash()};}
};
Reader Open(const std::string& s,size_t limit) {
    if(s.size()<32 || s.size()>limit)Fail();Reader tail{std::string_view(s).substr(s.size()-32)};
    auto payload=std::string_view(s).substr(0,s.size()-32);
    if(tail.Hash()!=Digest(payload))Fail();return {payload};
}
std::string Profile() {
    const auto c=consensus::SelectedOrchardBlockContext(BlockHeader{},Params().orchard_activation_height);
    if(!c)Fail(Status::Invalid);
    std::string s;Number(s,c->domain.network_code,1);
    s.append(reinterpret_cast<const char*>(c->domain.genesis_wire.data()),32);
    Number(s,c->domain.branch_id,4);Number(s,c->activation_height,4);return s;
}
const BlockHeader& Header(const RuntimeReorgBlock& b) {
    return b.body.IsOrchardProfile()?b.body.Orchard().Header():b.body.Historical().header;
}
void CheckPlan(const RuntimeReorgPlan& plan) {
    if(plan.disconnect.empty() || plan.disconnect.size()>max_blocks || plan.connect.size()>max_blocks-plan.disconnect.size())Fail();
    uint256 current=plan.disconnect.front().hash;uint32_t height=plan.disconnect.front().height;bool mixed=false;
    const auto check=[&](const RuntimeReorgBlock& b) {
        if(!b.height || b.height>INT32_MAX || b.hash.IsNull() || Header(b).GetHash()!=b.hash ||
            b.body.IsOrchardProfile()!=consensus::OrchardActiveForHeight(Params(),b.height))Fail();
        if(b.body.IsOrchardProfile()) {
            mixed=true;const auto selected=consensus::SelectedOrchardBlockContext(Header(b),b.height);
            const auto& c=*b.body.Context();
            if(!selected || c.height!=b.height || c.block_hash!=b.hash || c.parent_hash!=Header(b).prev_block_hash ||
                c.domain.network_code!=selected->domain.network_code || c.domain.genesis_wire!=selected->domain.genesis_wire ||
                c.domain.branch_id!=selected->domain.branch_id || c.activation_height!=selected->activation_height)Fail();
            std::string error;
            const bool witness=Params().enforce_witness_commitment && b.height>=Params().witness_commitment_enforcement_height;
            if(!b.body.Orchard().CheckSizeLimits(error) || !b.body.Orchard().CheckIdentityCommitments(witness,error))Fail();
        } else if(consensus::ComputeMerkleRoot(b.body.Historical().vtx)!=Header(b).merkle_root)Fail();
    };
    for(const auto& b:plan.disconnect) {
        check(b);if(b.hash!=current || b.height!=height)Fail();current=Header(b).prev_block_hash;--height;
    }
    for(const auto& b:plan.connect) {
        check(b);if(Header(b).prev_block_hash!=current || uint64_t(height)+1!=b.height)Fail();current=b.hash;height=b.height;
    }
    if(!mixed)Fail(Status::Invalid);
}
std::string Encode(const RuntimeReorgIntent& intent) {
    CheckPlan(*intent.plan);std::string s="DNRI01";Number(s,intent.cursor.sequence,8);Hash(s,intent.previous_digest);
    s+=Profile();Number(s,intent.outbox_origin.sequence,8);Hash(s,intent.outbox_origin.digest);
    Number(s,intent.plan->disconnect.size(),4);Number(s,intent.plan->connect.size(),4);
    const auto append=[&](const RuntimeReorgBlock& b) {
        const auto body=b.body.Serialize();
        // Includes framing and footer, not merely the bodies. Bound before append.
        if(body.size()>max_bytes || s.size()>max_bytes-32-41 || body.size()>max_bytes-32-41-s.size())Fail(Status::Invalid);
        Number(s,b.body.IsOrchardProfile()?1:0,1);Number(s,b.height,4);Hash(s,b.hash);Number(s,body.size(),4);
        s.append(reinterpret_cast<const char*>(body.data()),body.size());
    };
    for(const auto& b:intent.plan->disconnect)append(b);for(const auto& b:intent.plan->connect)append(b);
    Seal(s);return s;
}
RuntimeReorgIntent Decode(const std::string& bytes,uint64_t sequence) {
    auto r=Open(bytes,max_bytes);if(r.Take(6)!="DNRI01")Fail();
    RuntimeReorgIntent intent;intent.cursor.sequence=r.Number(8);intent.previous_digest=r.Hash();
    if(!sequence || intent.cursor.sequence!=sequence || (sequence==1)!=intent.previous_digest.IsNull() || r.Take(41)!=Profile())Fail();
    intent.outbox_origin=r.Cursor();if((intent.outbox_origin.sequence==0)!=intent.outbox_origin.digest.IsNull())Fail();
    auto old_count=r.Number(4),new_count=r.Number(4);
    if(!old_count || old_count>max_blocks || new_count>max_blocks-old_count)Fail();
    std::vector<RuntimeReorgBlock> old_blocks,new_blocks;old_blocks.reserve(old_count);new_blocks.reserve(new_count);
    auto read=[&](std::vector<RuntimeReorgBlock>& blocks) {
        const auto kind=r.Number(1);const uint32_t height=r.Number(4);const auto hash=r.Hash();const auto size=r.Number(4);
        if(kind>1 || !size || size>max_bytes || size>r.s.size())Fail();const auto view=r.Take(size);
        std::vector<uint8_t> bytes(view.begin(),view.end());
        if(kind) {
            auto body=OrchardBlockCandidate::DecodeExact(bytes);auto c=consensus::SelectedOrchardBlockContext(body.Header(),height);
            if(!c)Fail();blocks.push_back({hash,height,RuntimeBlockBody(std::move(body),*c)});
        } else {
            auto body=Block::Deserialize(bytes);if(!body)Fail();
            // Historical serialization is the retained typed representation;
            // it is not a claim about every historically accepted wire encoding.
            if(body->Serialize()!=std::string(view))Fail();blocks.push_back({hash,height,RuntimeBlockBody(std::move(*body))});
        }
    };
    for(size_t i=0;i<old_count;++i)read(old_blocks);for(size_t i=0;i<new_count;++i)read(new_blocks);
    if(!r.s.empty())Fail();intent.plan=std::make_shared<const RuntimeReorgPlan>(RuntimeReorgPlan{std::move(old_blocks),std::move(new_blocks)});
    CheckPlan(*intent.plan);intent.cursor.digest=Digest(std::string_view(bytes).substr(0,bytes.size()-32));return intent;
}
RuntimeReorgIntent Record(const ChainDB& db,uint64_t sequence) {
    const auto raw=Get(db,Key(sequence));if(!raw)Fail();
    try {return Decode(*raw,sequence);} catch(const std::bad_alloc&){throw;}
    catch(const OrchardStateLookupError&){throw;} catch(...){Fail();}
}
RuntimeOutboxCursor Head(const ChainDB& db) {
    const auto raw=Get(db,head_key);if(!raw){if(Get(db,Key(1)))Fail();return {};}
    auto r=Open(*raw,78);if(r.Take(6)!="DNRH01")Fail();auto c=r.Cursor();
    if(!c.sequence || c.digest.IsNull() || !r.s.empty() || Record(db,c.sequence).cursor!=c)Fail();
    if(c.sequence!=UINT64_MAX && Get(db,Key(c.sequence+1)))Fail();return c;
}
} // namespace
RuntimeOutboxCursor PersistRuntimeReorgIntentUnderLock(ChainDB& db,const ChainWriteToken& token,const RuntimeReorgPlan& plan) {
    CheckPlan(plan);const auto tip=db.getTip();
    if(!tip.ok())Fail(tip.status());
    if(tip->height!=int32_t(plan.disconnect.front().height) || tip->hash!=plan.disconnect.front().hash)Fail();
    const auto head=Head(db);if(head.sequence==UINT64_MAX)Fail(Status::Invalid);
    const auto c=consensus::SelectedOrchardBlockContext(BlockHeader{},Params().orchard_activation_height);if(!c)Fail(Status::Invalid);
    RuntimeReorgIntent intent{{head.sequence+1,{}},head.digest,ReadRuntimeOutboxUnderLock(db,*c,{},1).head,
        std::make_shared<const RuntimeReorgPlan>(plan)};
    const auto bytes=Encode(intent);intent.cursor.digest=Digest(std::string_view(bytes).substr(0,bytes.size()-32));
    std::string marker="DNRH01";Number(marker,intent.cursor.sequence,8);Hash(marker,intent.cursor.digest);Seal(marker);
    rocksdb::WriteBatch batch;batch.Put(Key(intent.cursor.sequence),bytes);batch.Put(head_key,marker);
    // This batch contains only recovery intent. A reported write failure blocks
    // the reorg, even if the intent actually reached disk; no canonical change
    // has started, and any retained attempt must still be reconciled.
    const auto status=db.writeBatch(token,std::move(batch),true);if(status!=Status::Ok)Fail(status);
    return intent.cursor;
}
std::optional<RuntimeReorgIntent> ReadRuntimeReorgIntentUnderLock(const ChainDB& db,RuntimeOutboxCursor after) {
    const auto head=Head(db);
    if(after.sequence>head.sequence || (!after.sequence && !after.digest.IsNull()))Fail();
    if(after.sequence && Record(db,after.sequence).cursor!=after)Fail();
    if(after==head)return {};
    auto result=Record(db,after.sequence+1);if(result.previous_digest!=after.digest)Fail();
    if(result.cursor.sequence==head.sequence && result.cursor!=head)Fail();
    return result;
}
} // namespace dinero

#include "wallet/runtime_index_delivery.h"
#include "primitives/orchard_block_reader.h"
#include "primitives/block.h"
#include <algorithm>
#include "wallet/utxo_index.h"
#include "crypto/sha256.h"
#include "consensus/merkle_root.h"
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace dinero {
namespace {
constexpr const char* receipt_key="runtime_delivery:v1:receipt";
constexpr const char* invalid_key="runtime_delivery:v1:invalidated";
[[noreturn]] void Fail(const char* reason) { throw std::runtime_error(reason); }
void Exec(sqlite3* db,const char* sql) {
    if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK)Fail("Index delivery SQL failed");
}
struct Statement {
    sqlite3_stmt* value=nullptr;
    Statement(sqlite3* db,const char* sql) {
        if(sqlite3_prepare_v2(db,sql,-1,&value,nullptr)!=SQLITE_OK) {
            sqlite3_finalize(value);Fail("Index delivery prepare failed");
        }
    }
    ~Statement(){sqlite3_finalize(value);}
    void Text(int n,const std::string& value_) {
        if(sqlite3_bind_text(value,n,value_.data(),int(value_.size()),SQLITE_TRANSIENT)!=SQLITE_OK)
            Fail("Index delivery bind failed");
    }
    void Blob(int n,const std::string& value_) {
        if(sqlite3_bind_blob(value,n,value_.data(),int(value_.size()),SQLITE_TRANSIENT)!=SQLITE_OK)
            Fail("Index delivery bind failed");
    }
    void Int(int n,int64_t value_) {
        if(sqlite3_bind_int64(value,n,value_)!=SQLITE_OK)Fail("Index delivery bind failed");
    }
    void Done(){if(sqlite3_step(value)!=SQLITE_DONE)Fail("Index delivery statement failed");}
};
std::optional<std::string> Metadata(sqlite3* db,const char* key) {
    Statement stmt(db,"SELECT value FROM utxo_metadata WHERE key=?");stmt.Text(1,key);
    const auto rc=sqlite3_step(stmt.value);if(rc==SQLITE_DONE)return {};
    if(rc!=SQLITE_ROW)Fail("Index delivery metadata read failed");
    const auto size=sqlite3_column_bytes(stmt.value,0);
    const auto bytes=static_cast<const char*>(sqlite3_column_blob(stmt.value,0));
    if(!bytes || size<1 || size>2048)Fail("Index delivery metadata malformed");
    std::string result(bytes,size);
    if(sqlite3_step(stmt.value)!=SQLITE_DONE)Fail("Index delivery metadata read failed");
    return result;
}
void Number(std::string& s,uint64_t n,size_t width){for(size_t i=0;i<width;++i)s.push_back(char(n>>(8*i)));}
void Hash(std::string& s,const uint256& h){s.append(reinterpret_cast<const char*>(h.data),32);}
uint256 Digest(const std::string& s){uint256 h;crypto::CSHA256().Write(s).Finalize(h.data);return h;}
struct Reader {
    std::string_view s;
    std::string_view Take(size_t n){if(n>s.size())Fail("Index delivery receipt malformed");auto v=s.substr(0,n);s.remove_prefix(n);return v;}
    uint64_t Number(size_t n){auto v=Take(n);uint64_t result=0;for(size_t i=0;i<n;++i)result|=uint64_t(uint8_t(v[i]))<<(i*8);return result;}
    uint256 Hash(){auto v=Take(32);uint256 h;std::memcpy(h.data,v.data(),32);return h;}
};
uint256 Scripts(const std::map<std::vector<uint8_t>,std::string>& scripts) {
    crypto::CSHA256 hash;
    for(const auto& [script,path]:scripts) {
        std::string item;Number(item,script.size(),8);if(!script.empty())item.append(reinterpret_cast<const char*>(script.data()),script.size());
        Number(item,path.size(),8);item+=path;hash.Write(item);
    }
    uint256 result;hash.Finalize(result.data);return result;
}
uint256 Profile(const consensus::OrchardBlockContext& c) {
    std::string bytes;Number(bytes,c.domain.network_code,1);
    bytes.append(reinterpret_cast<const char*>(c.domain.genesis_wire.data()),32);
    Number(bytes,c.domain.branch_id,4);Number(bytes,c.activation_height,4);return Digest(bytes);
}
struct Receipt { RuntimeIndexProgress progress;uint256 profile; };
Receipt Decode(const std::string& bytes,const std::string& identity,const uint256& scripts) {
    if(bytes.size()<32)Fail("Index delivery receipt malformed");
    Reader tail{std::string_view(bytes).substr(bytes.size()-32)};
    const auto payload=bytes.substr(0,bytes.size()-32);
    if(tail.Hash()!=Digest(payload))Fail("Index delivery receipt checksum mismatch");
    Reader r{payload};if(r.Take(6)!="DNUI01" || r.Take(r.Number(2))!=identity || r.Hash()!=scripts)
        Fail("Index delivery ownership changed; baseline reconciliation required");
    Receipt result;result.profile=r.Hash();auto& p=result.progress;
    p.cursor.sequence=r.Number(8);p.cursor.digest=r.Hash();p.origin_height=r.Number(4);p.origin_hash=r.Hash();
    p.tip_height=r.Number(4);p.tip_hash=r.Hash();
    if(!r.s.empty() || !p.cursor.sequence || p.cursor.digest.IsNull() || p.origin_hash.IsNull() || p.tip_hash.IsNull() ||
        p.tip_height>INT32_MAX || p.origin_height>INT32_MAX)Fail("Index delivery receipt malformed");
    return result;
}
std::string Encode(const Receipt& receipt,const std::string& identity,const uint256& scripts) {
    std::string s="DNUI01";Number(s,identity.size(),2);s+=identity;Hash(s,scripts);Hash(s,receipt.profile);
    const auto& p=receipt.progress;Number(s,p.cursor.sequence,8);Hash(s,p.cursor.digest);
    Number(s,p.origin_height,4);Hash(s,p.origin_hash);Number(s,p.tip_height,4);Hash(s,p.tip_hash);Hash(s,Digest(s));return s;
}
void Identity(const std::string& identity){if(identity.empty() || identity.size()>256)Fail("Index delivery wallet identity unavailable");}
std::string Trigger(const char* action) {
    return std::string("CREATE TRIGGER runtime_delivery_")+action+" AFTER "+action+
        " ON wallet_utxos BEGIN INSERT OR REPLACE INTO utxo_metadata(key,value) SELECT 'runtime_delivery:v1:invalidated','1' WHERE EXISTS(SELECT 1 FROM utxo_metadata WHERE key='runtime_delivery:v1:receipt'); DELETE FROM utxo_metadata WHERE key='runtime_delivery:v1:receipt'; END";
}
void CheckTriggers(sqlite3* db) {
    for(const auto* action:{"INSERT","UPDATE","DELETE"}) {
        Statement query(db,"SELECT sql FROM sqlite_master WHERE type='trigger' AND name=?");
        query.Text(1,std::string("runtime_delivery_")+action);
        if(sqlite3_step(query.value)!=SQLITE_ROW)Fail("Index delivery invalidation guard unavailable");
        const auto* text=reinterpret_cast<const char*>(sqlite3_column_text(query.value,0));
        if(!text || std::string(text,sqlite3_column_bytes(query.value,0))!=Trigger(action) || sqlite3_step(query.value)!=SQLITE_DONE)
            Fail("Index delivery invalidation guard changed");
    }
}
std::optional<Receipt> Read(sqlite3* db,const std::string& identity,const uint256& scripts) {
    Identity(identity);
    if(Metadata(db,invalid_key))Fail("Index delivery invalidated; baseline reconciliation required");
    const auto bytes=Metadata(db,receipt_key);if(!bytes)return {};
    CheckTriggers(db);return Decode(*bytes,identity,scripts);
}
using Tip=std::pair<uint256,uint32_t>;
Tip Before(const RuntimeOutboxEvent& e){return e.direction==RuntimeBlockDirection::Connect?Tip{e.context.parent_hash,e.context.height-1}:Tip{e.context.block_hash,e.context.height};}
Tip After(const RuntimeOutboxEvent& e){return e.direction==RuntimeBlockDirection::Connect?Tip{e.context.block_hash,e.context.height}:Tip{e.context.parent_hash,e.context.height-1};}
struct Output { TxId txid;uint32_t n;uint64_t amount;std::vector<uint8_t> script;bool coinbase; };
struct Effect {std::vector<TxOutPoint> spent;std::vector<Output> created;};
std::vector<Effect> Effects(const RuntimeOutboxEvent& e) {
    const auto& c=e.context;
    if(!c.height || c.height>INT32_MAX || c.block_hash.IsNull() || c.parent_hash.IsNull() ||
        !c.activation_height || c.activation_height>INT32_MAX || !c.domain.branch_id || c.domain.network_code>2 ||
        e.body.size()>16*1024*1024 || std::all_of(c.domain.genesis_wire.begin(),c.domain.genesis_wire.end(),[](auto x){return x==0;}) ||
        !e.cursor.sequence || e.cursor.digest.IsNull() || (e.cursor.sequence==1)!=e.previous_digest.IsNull() ||
        (e.direction!=RuntimeBlockDirection::Connect && e.direction!=RuntimeBlockDirection::Disconnect))
        Fail("Index delivery event malformed");
    std::vector<Effect> result;
    const auto historical=[&](const Transaction& tx) {
        Effect effect;const auto id=tx.GetTxid();const bool coinbase=tx.IsCoinbase();
        if(!coinbase)for(const auto& input:tx.vin)effect.spent.push_back(input.prevout);
        for(size_t n=0;n<tx.vout.size();++n) {
            const auto& output=tx.vout[n];
            if(output.is_confidential)Fail("Index delivery confidential output unsupported");
            effect.created.push_back({id,uint32_t(n),output.value.GetUna(),output.scriptPubKey,coinbase});
        }
        result.push_back(std::move(effect));
    };
    if(e.IsOrchardProfile()) {
        const auto block=OrchardBlockCandidate::DecodeExact(e.body);std::string error;
        if(block.Header().GetHash()!=c.block_hash || block.Header().prev_block_hash!=c.parent_hash ||
            !block.CheckIdentityCommitments(false,error))Fail("Index delivery body mismatch");
        for(const auto& tx:block.Transactions()) {
            if(!tx.IsOrchard()){historical(tx.Historical());continue;}
            Effect effect;for(const auto& input:tx.Orchard().Inputs()) {
                uint256 hash;std::copy(input.txid_wire.begin(),input.txid_wire.end(),hash.begin());
                effect.spent.emplace_back(TxId(hash),input.output_index);
            }
            const auto& outputs=tx.Orchard().Outputs();
            for(size_t n=0;n<outputs.size();++n)effect.created.push_back({tx.GetTxid(),uint32_t(n),outputs[n].amount_una,outputs[n].script_pub_key,false});
            result.push_back(std::move(effect));
        }
    } else {
        const auto block=Block::Deserialize(e.body);
        if(!block || block->Serialize()!=std::string(e.body.begin(),e.body.end()) || block->GetHash()!=c.block_hash ||
            block->header.prev_block_hash!=c.parent_hash || consensus::ComputeMerkleRoot(block->vtx)!=block->header.merkle_root)
            Fail("Index delivery body mismatch");
        for(const auto& tx:block->vtx)historical(tx);
    }
    return result;
}
} // namespace

std::optional<RuntimeIndexProgress> RuntimeIndexDelivery::Read(UTXOIndex& index,const std::string& identity) {
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_ || !sqlite3_get_autocommit(index.db_))Fail("Index delivery read ownership unavailable");
    const auto receipt=dinero::Read(index.db_,identity,Scripts(index.watched_scripts_));
    return receipt?std::optional(receipt->progress):std::nullopt;
}
RuntimeIndexProgress RuntimeIndexDelivery::Apply(UTXOIndex& index,const std::string& identity,const RuntimeOutboxEvent& event) {
    const auto effects=Effects(event);Identity(identity);
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_ || index.atomic_write_active_ || !sqlite3_get_autocommit(index.db_))Fail("Index delivery write ownership unavailable");
    const auto ownership=Scripts(index.watched_scripts_);const auto existing=dinero::Read(index.db_,identity,ownership);
    const auto before=Before(event),after=After(event);const auto profile=Profile(event.context);
    if(existing && existing->progress.cursor==event.cursor) {
        if(existing->profile!=profile || Tip{existing->progress.tip_hash,existing->progress.tip_height}!=after)
            Fail("Index delivery replay mismatch");
        return existing->progress;
    }
    if(existing) {
        const auto& p=existing->progress;
        if(p.cursor.sequence==UINT64_MAX || event.cursor.sequence!=p.cursor.sequence+1 || event.previous_digest!=p.cursor.digest ||
            existing->profile!=profile || Tip{p.tip_hash,p.tip_height}!=before)Fail("Index delivery source discontinuity");
    } else {
        if(event.cursor.sequence!=1)Fail("Index delivery origin unavailable");
        Statement ahead(index.db_,"SELECT 1 FROM wallet_utxos WHERE height>? OR spend_height>? LIMIT 1");
        ahead.Int(1,before.second);ahead.Int(2,before.second);
        const auto rc=sqlite3_step(ahead.value);
        if(rc==SQLITE_ROW)Fail("Index delivery baseline ahead of origin");
        if(rc!=SQLITE_DONE)Fail("Index delivery baseline read failed");
    }
    Receipt next;next.profile=profile;next.progress={event.cursor,existing?existing->progress.origin_hash:before.first,after.first,
        existing?existing->progress.origin_height:before.second,after.second};
    const auto bytes=Encode(next,identity,ownership);
    Exec(index.db_,"PRAGMA synchronous=FULL");
    Statement durability(index.db_,"PRAGMA synchronous");
    if(sqlite3_step(durability.value)!=SQLITE_ROW || sqlite3_column_int(durability.value,0)!=2 || sqlite3_step(durability.value)!=SQLITE_DONE)
        Fail("Index delivery durability unavailable");
    index.ApplyAtomically([&] {
        // Ordinary writers atomically invalidate tracked progress. The source
        // owner replaces it only after all effects below succeed.
        for(const auto* action:{"INSERT","UPDATE","DELETE"}) {
            auto sql=Trigger(action);sql.insert(15,"IF NOT EXISTS ");Exec(index.db_,sql.c_str());
        }
        CheckTriggers(index.db_);
        if(event.direction==RuntimeBlockDirection::Disconnect) {
            Statement remove(index.db_,"DELETE FROM wallet_utxos WHERE height=?");remove.Int(1,event.context.height);remove.Done();
            Statement restore(index.db_,"UPDATE wallet_utxos SET spend_height=NULL WHERE spend_height=?");restore.Int(1,event.context.height);restore.Done();
        } else for(const auto& effect:effects) {
            for(const auto& point:effect.spent) {
                Statement spend(index.db_,"UPDATE wallet_utxos SET spend_height=? WHERE txid=? AND vout=?");
                spend.Int(1,event.context.height);spend.Text(2,point.txid.AsUint256().GetHex());spend.Int(3,point.vout);spend.Done();
            }
            for(const auto& output:effect.created) {
                const auto owned=index.watched_scripts_.find(output.script);if(owned==index.watched_scripts_.end())continue;
                if(output.amount>uint64_t(INT64_MAX))Fail("Index delivery amount out of range");
                WalletUTXO row(output.txid,output.n,AmountUna::Una(output.amount),output.script,owned->second,event.context.height,output.coinbase);
                if(!index.AddUTXO(row))Fail("Index delivery output failed");
            }
        }
        Statement write(index.db_,"INSERT OR REPLACE INTO utxo_metadata(key,value) VALUES(?,?)");write.Text(1,receipt_key);write.Blob(2,bytes);write.Done();
        Exec(index.db_,"DELETE FROM utxo_metadata WHERE key='runtime_delivery:v1:invalidated'");
    });
    return next.progress;
}
} // namespace dinero

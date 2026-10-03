#include "wallet/runtime_index_delivery.h"
#include "wallet/runtime_origin_projection.h"
#include "wallet/wallet_manager.h"
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
#include <set>

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
Receipt Decode(const std::string& bytes,const std::string& identity,const uint256& scripts, std::string_view magic="DNUI01") {
    if(bytes.size()<32)Fail("Index delivery receipt malformed");
    Reader tail{std::string_view(bytes).substr(bytes.size()-32)};
    const auto payload=bytes.substr(0,bytes.size()-32);
    if(tail.Hash()!=Digest(payload))Fail("Index delivery receipt checksum mismatch");
    Reader r{payload};if(r.Take(6)!=magic || r.Take(r.Number(2))!=identity || r.Hash()!=scripts)
        Fail("Index delivery ownership changed; baseline reconciliation required");
    Receipt result;result.profile=r.Hash();auto& p=result.progress;
    p.cursor.sequence=r.Number(8);p.cursor.digest=r.Hash();p.origin_height=r.Number(4);p.origin_hash=r.Hash();
    p.tip_height=r.Number(4);p.tip_hash=r.Hash();
    if(!r.s.empty() || !p.cursor.sequence || p.cursor.digest.IsNull() || p.origin_hash.IsNull() || p.tip_hash.IsNull() ||
        p.tip_height>INT32_MAX || p.origin_height>INT32_MAX)Fail("Index delivery receipt malformed");
    return result;
}
std::string Encode(const Receipt& receipt,const std::string& identity,const uint256& scripts, std::string_view magic="DNUI01") {
    std::string s(magic);Number(s,identity.size(),2);s+=identity;Hash(s,scripts);Hash(s,receipt.profile);
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
struct Effect {TxId id; bool coinbase=false; uint64_t time=0; std::vector<TxOutPoint> spent;std::vector<Output> created;};
std::vector<Effect> Effects(const RuntimeOutboxEvent& e) {
    const auto& c=e.context;
    if(!c.height || c.height>INT32_MAX || c.block_hash.IsNull() || c.parent_hash.IsNull() ||
        !c.activation_height || c.activation_height>INT32_MAX || !c.domain.branch_id || c.domain.network_code>2 ||
        e.body.size()>16*1024*1024 || std::all_of(c.domain.genesis_wire.begin(),c.domain.genesis_wire.end(),[](auto x){return x==0;}) ||
        !e.cursor.sequence || e.cursor.digest.IsNull() || (e.cursor.sequence==1)!=e.previous_digest.IsNull() ||
        (e.direction!=RuntimeBlockDirection::Connect && e.direction!=RuntimeBlockDirection::Disconnect))
        Fail("Index delivery event malformed");
    std::vector<Effect> result;uint64_t timestamp=0;
    const auto historical=[&](const Transaction& tx) {
        Effect effect;const auto id=tx.GetTxid();const bool coinbase=tx.IsCoinbase();effect.id=id;effect.coinbase=coinbase;
        if(!coinbase)for(const auto& input:tx.vin)effect.spent.push_back(input.prevout);
        for(size_t n=0;n<tx.vout.size();++n) {
            const auto& output=tx.vout[n];
            if(output.is_confidential)Fail("Index delivery confidential output unsupported");
            effect.created.push_back({id,uint32_t(n),output.value.GetUna(),output.scriptPubKey,coinbase});
        }
        result.push_back(std::move(effect));
    };
    if(e.IsOrchardProfile()) {
        const auto block=OrchardBlockCandidate::DecodeExact(e.body);std::string error;timestamp=block.Header().timestamp;
        if(block.Header().GetHash()!=c.block_hash || block.Header().prev_block_hash!=c.parent_hash ||
            !block.CheckIdentityCommitments(false,error))Fail("Index delivery body mismatch");
        for(const auto& tx:block.Transactions()) {
            if(!tx.IsOrchard()){historical(tx.Historical());continue;}
            Effect effect;effect.id=tx.GetTxid();for(const auto& input:tx.Orchard().Inputs()) {
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
        timestamp=block->header.timestamp;for(const auto& tx:block->vtx)historical(tx);
    }
    if(timestamp>uint64_t(INT64_MAX))Fail("Delivery timestamp out of range");
    for(auto& effect:result)effect.time=timestamp;
    return result;
}
auto OriginCoins(const RuntimeWalletOriginProjection& source,const RuntimeOutboxEvent* applied) {
    auto coins=source.Coins();
    if(applied)for(const auto& effect:Effects(*applied)) {
        for(const auto& point:effect.spent) {
            const auto c=coins.find(point);if(c!=coins.end())c->second.spent=RuntimeWalletOriginProjection::Spend{effect.id,applied->context.height,effect.time};
        }
        for(const auto& out:effect.created) {
            const TxOutPoint point{out.txid,out.n};
            if(!coins.emplace(point,RuntimeWalletOriginProjection::Coin{
                TxOutput(AmountUna::Una(out.amount),out.script),applied->context.height,effect.time,out.coinbase,{}}).second)
                Fail("Origin repeated first-event creation");
        }
    }
    return coins;
}
std::set<TxOutPoint> IndexOriginRows(sqlite3* db,const std::map<std::vector<uint8_t>,std::string>& scripts,
        const RuntimeWalletOriginProjection& source,const RuntimeOutboxEvent* applied) {
    const auto coins=OriginCoins(source,applied);std::set<TxOutPoint> retained;
    Statement rows(db,"SELECT txid,vout,value,spk,path,height,spend_height,is_coinbase,is_confidential FROM wallet_utxos");int rc;
    while((rc=sqlite3_step(rows.value))==SQLITE_ROW) {
        const auto* id=reinterpret_cast<const char*>(sqlite3_column_text(rows.value,0));
        const auto* path=reinterpret_cast<const char*>(sqlite3_column_text(rows.value,4));
        const auto* spk=static_cast<const uint8_t*>(sqlite3_column_blob(rows.value,3));
        if(!id || std::strlen(id)!=64 || !path || !spk || sqlite3_column_int64(rows.value,1)<0 || sqlite3_column_int64(rows.value,1)>UINT32_MAX)
            Fail("Origin index row malformed");
        for(const auto col:{1,2,5,7,8})if(sqlite3_column_type(rows.value,col)!=SQLITE_INTEGER)Fail("Origin index integer malformed");
        const auto found=coins.find(TxOutPoint{TxId(uint256::FromHexUnsafe(id)),uint32_t(sqlite3_column_int64(rows.value,1))});
        if(found==coins.end() || found->first.txid.AsUint256().GetHex()!=id)Fail("Origin index row outside selected history");
        retained.insert(found->first);
        const auto& c=found->second;const auto owned=scripts.find(c.output.scriptPubKey);
        if(owned==scripts.end() || owned->second!=path ||
           c.output.scriptPubKey!=std::vector<uint8_t>(spk,spk+sqlite3_column_bytes(rows.value,3)) ||
           sqlite3_column_int64(rows.value,2)!=int64_t(c.output.value.GetUna()) ||
           sqlite3_column_int64(rows.value,5)!=c.height || sqlite3_column_int64(rows.value,7)!=c.coinbase ||
           sqlite3_column_int64(rows.value,8)!=0 ||
           (sqlite3_column_type(rows.value,6)!=SQLITE_NULL && (!c.spent || sqlite3_column_type(rows.value,6)!=SQLITE_INTEGER || sqlite3_column_int64(rows.value,6)!=c.spent->height)) ||
           (applied && c.spent && sqlite3_column_type(rows.value,6)==SQLITE_NULL))
            Fail("Origin index row reconciliation required");
    }
    if(rc!=SQLITE_DONE)Fail("Origin index inventory failed");
    if(applied)for(const auto& [point,c]:coins)if(scripts.count(c.output.scriptPubKey) && !retained.count(point))
        Fail("Origin index receipt lacks baseline");
    return retained;
}
} // namespace

void RuntimeIndexDelivery::CaptureOriginDomain(UTXOIndex& index,RuntimeWalletOriginProjection& source) {
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_ || index.atomic_write_active_ || !sqlite3_get_autocommit(index.db_))Fail("Origin index ownership unavailable");
    source.index_scripts_=index.watched_scripts_;source.index_path_=index.db_path_;
    for(const auto& [script,path]:index.watched_scripts_) {
        if(script.empty() || script.size()>10000 || path.empty())Fail("Origin index script malformed");
        source.scripts_.try_emplace(script,"");
    }
}
void RuntimeIndexDelivery::CheckOriginDomain(UTXOIndex& index,const RuntimeWalletOriginProjection& source) {
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_ || index.atomic_write_active_ || !sqlite3_get_autocommit(index.db_) ||
       !source.index_scripts_ || *source.index_scripts_!=index.watched_scripts_ || source.index_path_!=index.db_path_)
        Fail("Origin index domain changed");
}

std::optional<RuntimeIndexProgress> RuntimeIndexDelivery::Read(UTXOIndex& index,const std::string& identity) {
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_ || !sqlite3_get_autocommit(index.db_))Fail("Index delivery read ownership unavailable");
    const auto receipt=dinero::Read(index.db_,identity,Scripts(index.watched_scripts_));
    return receipt?std::optional(receipt->progress):std::nullopt;
}
RuntimeIndexProgress RuntimeIndexDelivery::Apply(UTXOIndex& index,const std::string& identity,const RuntimeOutboxEvent& event,const RuntimeWalletOriginProjection* origin,const std::function<void()>& finish) {
    const auto effects=Effects(event);Identity(identity);
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_ || index.atomic_write_active_ || !sqlite3_get_autocommit(index.db_))Fail("Index delivery write ownership unavailable");
    if(origin && (!origin->index_scripts_ || *origin->index_scripts_!=index.watched_scripts_ || origin->index_path_!=index.db_path_))
        Fail("Origin index domain changed");
    const auto ownership=Scripts(index.watched_scripts_);const auto existing=dinero::Read(index.db_,identity,ownership);
    const auto before=Before(event),after=After(event);const auto profile=Profile(event.context);
    if(existing && existing->progress.cursor==event.cursor) {
        if(existing->profile!=profile || Tip{existing->progress.tip_hash,existing->progress.tip_height}!=after)
            Fail("Index delivery replay mismatch");
        if(origin)IndexOriginRows(index.db_,index.watched_scripts_,*origin,&event);
        if(finish)finish();
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
        if(origin) {
            // Existing rows must agree with complete selected-origin facts.
            // Refuse unmatched/CT/ahead rows; never erase unrelated metadata.
            const auto retained=IndexOriginRows(index.db_,index.watched_scripts_,*origin,nullptr);
            for(const auto& [point,c]:origin->Coins()) {
                const auto owned=index.watched_scripts_.find(c.output.scriptPubKey);if(owned==index.watched_scripts_.end())continue;
                WalletUTXO row(point.txid,point.vout,c.output.value,c.output.scriptPubKey,owned->second,c.height,c.coinbase);
                if(!retained.count(point) && !index.AddUTXO(row))Fail("Origin index creation failed");
                if(c.spent && !index.SpendUTXO(point.txid,point.vout,c.spent->height))Fail("Origin index spend failed");
            }
        }
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
    if(finish)finish();
    return next.progress;
}

namespace {
// Progress stays in the existing wallet database. No independent delivery log.
class OrdinaryTransaction {
    sqlite3* db;
public:
    explicit OrdinaryTransaction(sqlite3* value):db(value){Exec(db,"BEGIN IMMEDIATE");}
    ~OrdinaryTransaction(){if(!sqlite3_get_autocommit(db) && sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)!=SQLITE_OK && !sqlite3_get_autocommit(db))std::terminate();}
    void Commit(){Exec(db,"COMMIT");}
};
bool OrdinaryColumns(sqlite3* db) {
    Statement query(db,"PRAGMA table_info(wallet_meta)");int rc;unsigned found=0;
    while((rc=sqlite3_step(query.value))==SQLITE_ROW) {
        const auto* name=reinterpret_cast<const char*>(sqlite3_column_text(query.value,1));
        if(!name)Fail("Ordinary delivery schema malformed");
        if(std::string_view(name)=="runtime_ordinary_receipt")found|=1;
        if(std::string_view(name)=="runtime_ordinary_invalid")found|=2;
    }
    if(rc!=SQLITE_DONE || (found!=0 && found!=3))Fail("Ordinary delivery schema incomplete");
    return found==3;
}
std::string OrdinaryTrigger(const char* table,const char* action) {
    return std::string("CREATE TRIGGER runtime_ordinary_")+table+"_"+action+" AFTER "+action+" ON "+table+
        " BEGIN UPDATE wallet_meta SET runtime_ordinary_invalid=1,runtime_ordinary_receipt=NULL WHERE id=1 AND runtime_ordinary_receipt IS NOT NULL; END";
}
void OrdinaryGuards(sqlite3* db,bool create) {
    for(const auto* table:{"utxos","transactions","watch_scripts","addresses","tip","sync_meta"})for(const auto* action:{"INSERT","UPDATE","DELETE"}) {
        const auto expected=OrdinaryTrigger(table,action);
        if(create){auto sql=expected;sql.insert(15,"IF NOT EXISTS ");Exec(db,sql.c_str());}
        Statement query(db,"SELECT sql FROM sqlite_master WHERE type='trigger' AND name=?");
        query.Text(1,std::string("runtime_ordinary_")+table+"_"+action);
        if(sqlite3_step(query.value)!=SQLITE_ROW)Fail("Ordinary delivery guard unavailable");
        const auto* text=reinterpret_cast<const char*>(sqlite3_column_text(query.value,0));
        if(!text || std::string(text,sqlite3_column_bytes(query.value,0))!=expected || sqlite3_step(query.value)!=SQLITE_DONE)
            Fail("Ordinary delivery guard changed");
    }
}
std::string Hex(const std::vector<uint8_t>& bytes) {
    constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(bytes.size()*2);
    for(const auto b:bytes){out+=digits[b>>4];out+=digits[b&15];}return out;
}
std::vector<uint8_t> Unhex(std::string_view value) {
    if(value.empty() || value.size()%2)Fail("Ordinary delivery address script malformed");
    auto nibble=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    std::vector<uint8_t> out;out.reserve(value.size()/2);
    for(size_t i=0;i<value.size();i+=2){const int a=nibble(value[i]),b=nibble(value[i+1]);if(a<0||b<0)Fail("Ordinary delivery address script malformed");out.push_back(uint8_t(a*16+b));}return out;
}
struct OrdinaryOwnership {std::map<std::vector<uint8_t>,std::string> addresses;uint256 digest;};
OrdinaryOwnership OrdinaryScripts(sqlite3* db) {
    OrdinaryOwnership result;std::map<std::vector<uint8_t>,std::string> paths;
    Statement watched(db,"SELECT script_pubkey,path FROM watch_scripts ORDER BY script_pubkey");int rc;
    while((rc=sqlite3_step(watched.value))==SQLITE_ROW) {
        const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(watched.value,0));const int n=sqlite3_column_bytes(watched.value,0);
        if(sqlite3_column_type(watched.value,0)!=SQLITE_BLOB || !bytes || n<=0 || n>10000)Fail("Ordinary delivery watched script malformed");
        std::vector<uint8_t> script(bytes,bytes+n);const auto* path=reinterpret_cast<const char*>(sqlite3_column_text(watched.value,1));
        paths[script]=path?std::string(path,sqlite3_column_bytes(watched.value,1)):std::string();result.addresses[script]="";
    }
    if(rc!=SQLITE_DONE)Fail("Ordinary delivery scripts read failed");
    Statement addresses(db,"SELECT script_pubkey,address FROM addresses WHERE script_pubkey IS NOT NULL AND script_pubkey<>'' ORDER BY script_pubkey,address");
    while((rc=sqlite3_step(addresses.value))==SQLITE_ROW) {
        const auto* script=reinterpret_cast<const char*>(sqlite3_column_text(addresses.value,0));const auto* address=reinterpret_cast<const char*>(sqlite3_column_text(addresses.value,1));
        if(!script || !address)Fail("Ordinary delivery address malformed");
        auto bytes=Unhex(std::string_view(script,sqlite3_column_bytes(addresses.value,0)));std::string display(address,sqlite3_column_bytes(addresses.value,1));
        auto& selected=result.addresses[bytes];if(!selected.empty() && selected!=display)Fail("Ordinary delivery ambiguous address");selected=std::move(display);
    }
    if(rc!=SQLITE_DONE)Fail("Ordinary delivery addresses read failed");
    std::string binding;Hash(binding,Scripts(paths));Hash(binding,Scripts(result.addresses));result.digest=Digest(binding);return result;
}
std::optional<Receipt> OrdinaryRead(sqlite3* db,const std::string& identity,const uint256& scripts) {
    if(!OrdinaryColumns(db))return {};
    Statement query(db,"SELECT runtime_ordinary_receipt,runtime_ordinary_invalid FROM wallet_meta WHERE id=1");
    if(sqlite3_step(query.value)!=SQLITE_ROW)Fail("Ordinary delivery metadata unavailable");
    if(sqlite3_column_type(query.value,1)!=SQLITE_INTEGER || sqlite3_column_int64(query.value,1)!=0)Fail("Ordinary delivery invalidated; baseline reconciliation required");
    std::optional<Receipt> result;
    if(sqlite3_column_type(query.value,0)!=SQLITE_NULL) {
        const auto* bytes=static_cast<const char*>(sqlite3_column_blob(query.value,0));const int n=sqlite3_column_bytes(query.value,0);
        if(sqlite3_column_type(query.value,0)!=SQLITE_BLOB || !bytes || n<1 || n>2048)Fail("Ordinary delivery receipt malformed");
        result=Decode(std::string(bytes,n),identity,scripts,"DNOW01");OrdinaryGuards(db,false);
    }
    if(sqlite3_step(query.value)!=SQLITE_DONE)Fail("Ordinary delivery metadata read failed");return result;
}
// Conservative adoption of selected-chain facts. Unknown rows and missing
// originated history require explicit reconciliation, never guessed categories
// or blanket deletion. Live lock/abandonment sets are not touched here.
void OrdinaryOrigin(sqlite3* db,int wallet_id,const OrdinaryOwnership& scripts,
                    const RuntimeWalletOriginProjection& source,bool write,const RuntimeOutboxEvent* applied=nullptr,
                    const RuntimeOutboxEvent* first_event=nullptr) {
    const auto text=[](sqlite3_stmt* row,int n) {
        const auto* p=reinterpret_cast<const char*>(sqlite3_column_text(row,n));
        return p?std::string(p,sqlite3_column_bytes(row,n)):std::string();
    };
    const auto coins=OriginCoins(source,applied);std::set<TxOutPoint> retained;std::set<std::string> first_ids;
    if(applied)for(const auto& e:Effects(*applied))first_ids.insert(e.id.AsUint256().GetHex());
    std::map<std::string,const RuntimeWalletOriginProjection::History*> history;
    for(const auto& h:source.Transactions()) {
        bool relevant=false;
        for(const auto& out:h.transaction.vout)relevant|=scripts.addresses.count(out.scriptPubKey)!=0;
        for(const auto& in:h.transaction.vin) {
            const auto c=source.Coins().find(in.prevout);
            relevant|=c!=source.Coins().end() && scripts.addresses.count(c->second.output.scriptPubKey)!=0;
        }
        if(relevant)history.emplace(h.transaction.GetTxid().AsUint256().GetHex(),&h);
    }
    Statement rows(db,"SELECT wallet_id,txid,vout,amount,script_pubkey,height,is_coinbase,is_spent,spent_txid,spent_height FROM utxos");int rc;
    while((rc=sqlite3_step(rows.value))==SQLITE_ROW) {
        const auto id=text(rows.value,1);const auto n=sqlite3_column_int64(rows.value,2);
        if(sqlite3_column_int64(rows.value,0)!=wallet_id || id.size()!=64 || n<0 || n>UINT32_MAX)
            Fail("Origin ordinary row ownership mismatch");
        const auto c=coins.find(TxOutPoint{TxId(uint256::FromHexUnsafe(id)),uint32_t(n)});
        if(c==coins.end() || c->first.txid.AsUint256().GetHex()!=id || !scripts.addresses.count(c->second.output.scriptPubKey))
            Fail("Origin ordinary row outside selected history");
        retained.insert(c->first);
        for(const auto col:{0,2,3,5,6,7})if(sqlite3_column_type(rows.value,col)!=SQLITE_INTEGER)Fail("Origin ordinary integer malformed");
        const auto& coin=c->second;const auto flag=sqlite3_column_int64(rows.value,7);
        if(sqlite3_column_int64(rows.value,3)!=int64_t(coin.output.value.GetUna()) ||
           Unhex(text(rows.value,4))!=coin.output.scriptPubKey || sqlite3_column_int64(rows.value,5)!=coin.height ||
           sqlite3_column_int64(rows.value,6)!=coin.coinbase || (flag!=0 && flag!=1) ||
           (flag && !coin.spent) || (applied && bool(flag)!=bool(coin.spent)) ||
           (applied && coin.spent && (sqlite3_column_type(rows.value,8)==SQLITE_NULL || sqlite3_column_type(rows.value,9)==SQLITE_NULL)) ||
           (sqlite3_column_type(rows.value,8)!=SQLITE_NULL && (!coin.spent || text(rows.value,8)!=coin.spent->txid.AsUint256().GetHex())) ||
           (sqlite3_column_type(rows.value,9)!=SQLITE_NULL && (!coin.spent || sqlite3_column_type(rows.value,9)!=SQLITE_INTEGER || sqlite3_column_int64(rows.value,9)!=coin.spent->height)))
            Fail("Origin ordinary row reconciliation required");
    }
    if(rc!=SQLITE_DONE)Fail("Origin ordinary inventory failed");
    if(applied)for(const auto& [point,c]:coins)if(scripts.addresses.count(c.output.scriptPubKey) && !retained.count(point))
        Fail("Origin ordinary receipt lacks baseline");
    std::set<std::string> existing;
    Statement txs(db,"SELECT wallet_id,txid,height FROM transactions");
    while((rc=sqlite3_step(txs.value))==SQLITE_ROW) {
        if(sqlite3_column_int64(txs.value,0)!=wallet_id)Fail("Origin history ownership mismatch");
        const auto id=text(txs.value,1);existing.insert(id);
        if(sqlite3_column_int64(txs.value,2)>0 && !history.count(id) && !first_ids.count(id))Fail("Origin orphan history reconciliation required");
    }
    if(rc!=SQLITE_DONE)Fail("Origin history inventory failed");
    // Event 1 can spend a pre-origin owned coin, or a known owned output
    // created earlier in that same event. Existing local history is required
    // for either case; selected-chain amounts cannot establish send metadata.
    if(!first_event)first_event=applied;
    if(first_event) {
        if(first_event->cursor.sequence!=1 || first_event->direction!=RuntimeBlockDirection::Connect)
            Fail("Origin first history event mismatch");
        std::set<TxOutPoint> owned;
        for(const auto& [point,coin]:source.Coins())
            if(!coin.spent && scripts.addresses.count(coin.output.scriptPubKey))owned.insert(point);
        for(const auto& effect:Effects(*first_event)) {
            bool spends=false;
            for(const auto& point:effect.spent)spends|=owned.erase(point)!=0;
            if(spends && !existing.count(effect.id.AsUint256().GetHex()))
                Fail("Origin first-event originated history reconciliation required");
            for(const auto& output:effect.created)
                if(scripts.addresses.count(output.script))owned.insert(TxOutPoint{output.txid,output.n});
        }
    }
    for(const auto& [id,h]:history) {
        bool spends=false,receives=false;uint64_t credit=0;std::string address;
        for(const auto& in:h->transaction.vin) {
            const auto c=source.Coins().find(in.prevout);
            spends|=c!=source.Coins().end() && scripts.addresses.count(c->second.output.scriptPubKey)!=0;
        }
        for(const auto& out:h->transaction.vout) {
            const auto owned=scripts.addresses.find(out.scriptPubKey);if(owned==scripts.addresses.end())continue;
            receives=true;credit+=out.value.GetUna();if(address.empty())address=owned->second;
        }
        if((spends || applied) && !existing.count(id))Fail("Origin originated history reconciliation required");
        if(!write)continue;
        if(existing.count(id)) {
            Statement confirm(db,"UPDATE transactions SET height=? WHERE wallet_id=? AND txid=?");
            confirm.Int(1,h->height);confirm.Int(2,wallet_id);confirm.Text(3,id);confirm.Done();
        } else if(receives) {
            Statement add(db,"INSERT INTO transactions(wallet_id,txid,address,amount,confirmations,category,label,time,is_coinbase,height) VALUES(?,?,?,?,0,?,'',?,?,?)");
            add.Int(1,wallet_id);add.Text(2,id);add.Text(3,address);
            if(sqlite3_bind_double(add.value,4,double(credit)/100000000.0)!=SQLITE_OK)Fail("Origin history bind failed");
            add.Text(5,h->transaction.IsCoinbase()?"generate":"receive");add.Int(6,h->time);add.Int(7,h->transaction.IsCoinbase());add.Int(8,h->height);add.Done();
        }
    }
    if(!write)return;
    for(const auto& [point,c]:source.Coins()) {
        const auto owned=scripts.addresses.find(c.output.scriptPubKey);if(owned==scripts.addresses.end())continue;
        Statement add(db,"INSERT INTO utxos(wallet_id,txid,vout,address,amount,script_pubkey,height,is_coinbase,is_spent,created_at,spent_txid,spent_height) VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT DO UPDATE SET is_spent=excluded.is_spent,spent_txid=excluded.spent_txid,spent_height=excluded.spent_height WHERE utxos.wallet_id=excluded.wallet_id AND utxos.txid=excluded.txid AND utxos.vout=excluded.vout");
        add.Int(1,wallet_id);add.Text(2,point.txid.AsUint256().GetHex());add.Int(3,point.vout);add.Text(4,owned->second);add.Int(5,c.output.value.GetUna());
        add.Text(6,Hex(c.output.scriptPubKey));add.Int(7,c.height);add.Int(8,c.coinbase);add.Int(9,c.spent.has_value());add.Int(10,c.time);
        if(c.spent){add.Text(11,c.spent->txid.AsUint256().GetHex());add.Int(12,c.spent->height);}
        add.Done();if(sqlite3_changes(db)!=1)Fail("Origin ordinary creation ownership mismatch");
    }
}
void OrdinaryInt(sqlite3* db,const char* sql,int64_t value){Statement statement(db,sql);statement.Int(1,value);statement.Done();}
} // namespace

std::unique_ptr<RuntimeWalletOriginProjection> RuntimeOrdinaryDelivery::CaptureOriginDomain(
        WalletManager& wallet,uint64_t session,UTXOIndex* index) {
    const auto lease=wallet.AcquireDatabaseLease();
    if(wallet.database_leases_!=1 || lease->Session()!=session)
        Fail("Origin projection wallet ownership changed");
    auto result=std::unique_ptr<RuntimeWalletOriginProjection>(new RuntimeWalletOriginProjection());
    result->identity_=lease->EnsureDeliveryIdentity();result->session_=session;
    OrdinaryTransaction transaction(lease->Database());
    const auto domain=OrdinaryScripts(lease->Database());
    result->scripts_=domain.addresses;result->scripts_digest_=domain.digest;
    if(index)RuntimeIndexDelivery::CaptureOriginDomain(*index,*result);
    transaction.Commit();return result;
}
void RuntimeOrdinaryDelivery::CheckOriginDomain(WalletManager& wallet,const RuntimeWalletOriginProjection& source,UTXOIndex* index) {
    const auto lease=wallet.AcquireDatabaseLease();
    if(wallet.database_leases_!=1 || lease->Session()!=source.session_ ||
       lease->EnsureDeliveryIdentity()!=source.identity_)Fail("Origin projection wallet ownership changed");
    OrdinaryTransaction transaction(lease->Database());
    if(OrdinaryScripts(lease->Database()).digest!=source.scripts_digest_)
        Fail("Origin projection script domain changed");
    if(index)RuntimeIndexDelivery::CheckOriginDomain(*index,source);
    transaction.Commit();
}
void RuntimeWalletOriginProjection::AppendValidated(const Block& block,uint32_t height) {
    // The service calls this only AFTER owned consensus replay of this body.
    // Failure discards the entire in-memory projection, never wallet state.
    constexpr size_t limit=64*1024*1024;
    const auto charge=[&](size_t n) {
        if(n>limit-retained_bytes_)Fail("Origin projection material limit");
        retained_bytes_+=n;
    };
    if(block.header.timestamp>uint64_t(INT64_MAX))Fail("Origin projection time out of range");
    for(const auto& tx:block.vtx) {
        const auto id=tx.GetTxid();uint64_t credited=0,debited=0;bool relevant=false;
        auto sum=[](uint64_t& n,uint64_t value) {
            if(value>uint64_t(INT64_MAX) || n>uint64_t(INT64_MAX)-value)
                Fail("Origin projection amount out of range");
            n+=value;
        };
        if(!tx.IsCoinbase())for(const auto& input:tx.vin) {
            const auto owned=coins_.find(input.prevout);if(owned==coins_.end())continue;
            if(owned->second.spent)Fail("Origin projection repeated owned spend");
            owned->second.spent=Spend{id,height,block.header.timestamp};
            sum(debited,owned->second.output.value.GetUna());relevant=true;
        }
        for(size_t i=0;i<tx.vout.size();++i) {
            const auto& output=tx.vout[i];
            if(output.is_confidential)Fail("Origin projection CT history unsupported");
            if(!scripts_.count(output.scriptPubKey))continue;
            if(i>UINT32_MAX)Fail("Origin projection output index out of range");
            charge(sizeof(Coin)+output.scriptPubKey.size());
            if(!coins_.emplace(TxOutPoint{id,uint32_t(i)},Coin{output,height,block.header.timestamp,tx.IsCoinbase(),{}}).second)
                Fail("Origin projection repeated owned creation");
            sum(credited,output.value.GetUna());relevant=true;
        }
        if(relevant) {
            charge(tx.Serialize(TxSerializationMode::WithWitness).size()+sizeof(History));
            history_.push_back({tx,height,block.header.timestamp,credited,debited});
        }
    }
}

std::optional<RuntimeIndexProgress> RuntimeOrdinaryDelivery::ReadForWallet(WalletManager& wallet,uint64_t session) {
    const auto lease=wallet.AcquireDatabaseLease();if(lease->Session()!=session)Fail("Wallet delivery selection changed");
    const auto identity=lease->EnsureDeliveryIdentity();auto* db=lease->Database();
    OrdinaryTransaction transaction(db);const auto scripts=OrdinaryScripts(db);const auto receipt=OrdinaryRead(db,identity,scripts.digest);
    transaction.Commit();return receipt?std::optional(receipt->progress):std::nullopt;
}
void RuntimeOrdinaryDelivery::AdoptOrigin(WalletManager& wallet,UTXOIndex& index,const RuntimeWalletOriginProjection& source) {
    const auto lease=wallet.AcquireDatabaseLease();
    if(wallet.database_leases_!=1 || lease->Session()!=source.session_ || lease->EnsureDeliveryIdentity()!=source.identity_ ||
       !source.index_scripts_ || source.first_.cursor.sequence!=1 || source.first_.direction!=RuntimeBlockDirection::Connect)
        Fail("Origin adoption ownership unavailable");
    { OrdinaryTransaction transaction(lease->Database());
      const auto domain=OrdinaryScripts(lease->Database());
      if(domain.digest!=source.scripts_digest_)Fail("Origin projection script domain changed");
      const auto receipt=OrdinaryRead(lease->Database(),source.identity_,domain.digest);
      if(receipt) {
          if(receipt->progress.cursor!=source.first_.cursor)Fail("Origin adoption already advanced");
          OrdinaryOrigin(lease->Database(),wallet.current_wallet_id_,domain,source,false,&source.first_);
      } else OrdinaryOrigin(lease->Database(),wallet.current_wallet_id_,domain,source,false,nullptr,&source.first_);
      transaction.Commit(); }
    // Index holds its script/database locks through ordinary commit, while the
    // actual wallet lease pins the other domain. A failed second store leaves
    // a genuine first-event index receipt for idempotent ordered retry.
    RuntimeIndexDelivery::Apply(index,source.identity_,source.first_,&source,[&] {
        Apply(wallet,source.session_,source.first_,&source);
    });
}

RuntimeIndexProgress RuntimeOrdinaryDelivery::ApplyForWallet(WalletManager& wallet,uint64_t session,const RuntimeOutboxEvent& event) {
    return Apply(wallet,session,event,nullptr);
}
RuntimeIndexProgress RuntimeOrdinaryDelivery::Apply(WalletManager& wallet,uint64_t session,const RuntimeOutboxEvent& event,const RuntimeWalletOriginProjection* origin) {
    const auto effects=Effects(event); // Exact typed decoding before taking wallet ownership.
    const auto lease=wallet.AcquireDatabaseLease();if(lease->Session()!=session)Fail("Wallet delivery selection changed");
    const auto identity=lease->EnsureDeliveryIdentity();auto* db=lease->Database();
    if(wallet.current_wallet_id_<0)Fail("Ordinary delivery wallet unavailable");
    OrdinaryTransaction transaction(db);
    { Statement foreign(db,"SELECT 1 FROM utxos WHERE wallet_id<>? UNION ALL SELECT 1 FROM transactions WHERE wallet_id<>? LIMIT 1");
      foreign.Int(1,wallet.current_wallet_id_);foreign.Int(2,wallet.current_wallet_id_);const auto rc=sqlite3_step(foreign.value);
      if(rc==SQLITE_ROW)Fail("Ordinary delivery baseline wallet mismatch");if(rc!=SQLITE_DONE)Fail("Ordinary delivery ownership read failed"); }
    const auto scripts=OrdinaryScripts(db);const auto old=OrdinaryRead(db,identity,scripts.digest);
    const auto before=Before(event),after=After(event);const auto profile=Profile(event.context);
    if(old && old->progress.cursor==event.cursor) {
        if(old->profile!=profile || Tip{old->progress.tip_hash,old->progress.tip_height}!=after)Fail("Ordinary delivery replay mismatch");
        if(origin)OrdinaryOrigin(db,wallet.current_wallet_id_,scripts,*origin,false,&event);
        transaction.Commit();return old->progress;
    }
    if(old) {
        const auto& p=old->progress;
        if(p.cursor.sequence==UINT64_MAX || event.cursor.sequence!=p.cursor.sequence+1 || event.previous_digest!=p.cursor.digest || old->profile!=profile || Tip{p.tip_hash,p.tip_height}!=before)
            Fail("Ordinary delivery source discontinuity");
    } else {
        if(event.cursor.sequence!=1)Fail("Ordinary delivery origin unavailable");
        Statement ahead(db,"SELECT 1 FROM utxos WHERE height>? OR spent_height>? UNION ALL SELECT 1 FROM transactions WHERE height>? LIMIT 1");
        ahead.Int(1,before.second);ahead.Int(2,before.second);ahead.Int(3,before.second);const auto rc=sqlite3_step(ahead.value);
        if(rc==SQLITE_ROW)Fail("Ordinary delivery baseline ahead of origin");if(rc!=SQLITE_DONE)Fail("Ordinary delivery baseline read failed");
    }
    if(!OrdinaryColumns(db)) {
        Exec(db,"ALTER TABLE wallet_meta ADD COLUMN runtime_ordinary_receipt BLOB");
        Exec(db,"ALTER TABLE wallet_meta ADD COLUMN runtime_ordinary_invalid INTEGER NOT NULL DEFAULT 0");
    }
    OrdinaryGuards(db,true);
    if(origin)OrdinaryOrigin(db,wallet.current_wallet_id_,scripts,*origin,true,nullptr,&origin->first_);
    if(event.direction==RuntimeBlockDirection::Disconnect) {
        // Keep existing history for transactions that spent our coins. A
        // disconnect removes confirmation, not the recorded local metadata.
        // Inspect before deleting same-block creations, which can themselves
        // be inputs to a later transaction in this block. This is history
        // retention only, not pending admission or reservation restoration.
        for(const auto& effect:effects) {
            bool owned_spend=false;
            const auto id=effect.id.AsUint256().GetHex();
            for(const auto& point:effect.spent) {
                Statement owned(db,"SELECT 1 FROM utxos WHERE wallet_id=? AND txid=? AND vout=? AND is_spent=1 AND spent_txid=? AND spent_height=?");
                owned.Int(1,wallet.current_wallet_id_);owned.Text(2,point.txid.AsUint256().GetHex());owned.Int(3,point.vout);
                owned.Text(4,id);owned.Int(5,event.context.height);
                const auto rc=sqlite3_step(owned.value);
                if(rc==SQLITE_ROW) {owned_spend=true;if(sqlite3_step(owned.value)!=SQLITE_DONE)Fail("Ordinary undo spend history read failed");}
                else if(rc!=SQLITE_DONE)Fail("Ordinary undo spend history read failed");
            }
            if(owned_spend) {
                Statement retain(db,"UPDATE transactions SET height=0,confirmations=0 WHERE wallet_id=? AND txid=? AND height=?");
                retain.Int(1,wallet.current_wallet_id_);retain.Text(2,id);retain.Int(3,event.context.height);retain.Done();
            }
        }
        OrdinaryInt(db,"DELETE FROM utxos WHERE height=?",event.context.height);
        OrdinaryInt(db,"DELETE FROM transactions WHERE height=?",event.context.height);
        for(const auto& effect:effects)for(const auto& point:effect.spent) {
            Statement restore(db,"UPDATE utxos SET is_spent=0,spent_txid=NULL,spent_height=NULL WHERE txid=? AND vout=?");
            restore.Text(1,point.txid.AsUint256().GetHex());restore.Int(2,point.vout);restore.Done();
        }
    } else for(const auto& effect:effects) {
        const auto id=effect.id.AsUint256().GetHex();
        Statement confirmation(db,"UPDATE transactions SET height=?,confirmations=1 WHERE wallet_id=? AND txid=?");
        confirmation.Int(1,event.context.height);confirmation.Int(2,wallet.current_wallet_id_);confirmation.Text(3,id);confirmation.Done();const bool existing_history=sqlite3_changes(db)>0;
        bool spends=false,owns_output=false;uint64_t received=0;std::string receiving_address;
        for(const auto& point:effect.spent) {
            Statement spend(db,"UPDATE utxos SET is_spent=1,spent_txid=?,spent_height=? WHERE wallet_id=? AND txid=? AND vout=?");
            spend.Text(1,id);spend.Int(2,event.context.height);spend.Int(3,wallet.current_wallet_id_);spend.Text(4,point.txid.AsUint256().GetHex());spend.Int(5,point.vout);spend.Done();spends|=sqlite3_changes(db)>0;
        }
        for(const auto& output:effect.created) {
            const auto owned=scripts.addresses.find(output.script);if(owned==scripts.addresses.end())continue;
            if(output.amount>uint64_t(INT64_MAX) || received>uint64_t(INT64_MAX)-output.amount)Fail("Ordinary delivery amount out of range");
            owns_output=true;received+=output.amount;if(receiving_address.empty())receiving_address=owned->second;
            Statement add(db,"INSERT INTO utxos(wallet_id,txid,vout,address,amount,script_pubkey,height,is_coinbase,is_spent,created_at) VALUES(?,?,?,?,?,?,?,?,0,?) ON CONFLICT DO UPDATE SET address=excluded.address,amount=excluded.amount,script_pubkey=excluded.script_pubkey,height=excluded.height,is_coinbase=excluded.is_coinbase WHERE utxos.wallet_id=excluded.wallet_id AND utxos.txid=excluded.txid AND utxos.vout=excluded.vout");
            add.Int(1,wallet.current_wallet_id_);add.Text(2,id);add.Int(3,output.n);add.Text(4,owned->second);add.Int(5,output.amount);add.Text(6,Hex(output.script));add.Int(7,event.context.height);add.Int(8,output.coinbase);add.Int(9,effect.time);add.Done();
            if(sqlite3_changes(db)!=1)Fail("Ordinary delivery output ownership mismatch");
        }
        // Preserve originating send/self-spend history, as the existing worker
        // does. New receive/mining rows use exact source data and a checked write.
        if(owns_output && (effect.coinbase || (!spends && !existing_history))) {
            Statement history(db,"INSERT INTO transactions(wallet_id,txid,address,amount,confirmations,category,label,time,is_coinbase,height) VALUES(?,?,?,?,1,?,'',?,?,?) ON CONFLICT(wallet_id,txid,address,category) DO UPDATE SET amount=excluded.amount,confirmations=1,height=excluded.height,is_coinbase=excluded.is_coinbase");
            history.Int(1,wallet.current_wallet_id_);history.Text(2,id);history.Text(3,receiving_address);
            if(sqlite3_bind_double(history.value,4,double(received)/100000000.0)!=SQLITE_OK)Fail("Ordinary delivery history bind failed");
            history.Text(5,effect.coinbase?"generate":"receive");history.Int(6,effect.time);history.Int(7,effect.coinbase);history.Int(8,event.context.height);history.Done();
        }
    }
    // Derived ordinary metadata belongs to the same commit, without publishing
    // process-wide height or claiming other consumers have caught up.
    OrdinaryInt(db,"UPDATE utxos SET confirmations=CASE WHEN height>0 THEN MAX(0,?-height+1) ELSE 0 END",after.second);
    Exec(db,"UPDATE utxos SET is_mature=CASE WHEN is_coinbase=0 OR confirmations>=100 THEN 1 ELSE 0 END");
    OrdinaryInt(db,"UPDATE transactions SET confirmations=CASE WHEN height>0 THEN MAX(0,?-height+1) ELSE 0 END",after.second);
    OrdinaryInt(db,"INSERT INTO tip(rowid,height) VALUES(1,?) ON CONFLICT(rowid) DO UPDATE SET height=excluded.height",after.second);
    Receipt next;next.profile=profile;next.progress={event.cursor,old?old->progress.origin_hash:before.first,after.first,old?old->progress.origin_height:before.second,after.second};
    const auto bytes=Encode(next,identity,scripts.digest,"DNOW01");
    Statement checkpoint(db,"UPDATE wallet_meta SET runtime_ordinary_receipt=?,runtime_ordinary_invalid=0 WHERE id=1");checkpoint.Blob(1,bytes);checkpoint.Done();
    if(sqlite3_changes(db)!=1)Fail("Ordinary delivery receipt unavailable");
    transaction.Commit();return next.progress;
}
} // namespace dinero

#include "wallet/runtime_index_delivery.h"
#include "wallet/runtime_origin_projection.h"
#include "wallet/orchard_account_delivery.h"
#include "wallet/orchard_ownership_inventory.h"
#include "wallet/runtime_account_replay.h"
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
using ScriptOwners=std::map<std::vector<uint8_t>,std::string>;
void CheckIndexOwners(const ScriptOwners& paths,const ScriptOwners& historical) {
    for(const auto& [script,address]:historical) {
        if(script.size()!=34 || script[0]!=0x51 || script[1]!=0x20 || address.empty() || address.size()>128 ||
           address.find('\0')!=std::string::npos || paths.contains(script))
            Fail("Index historical ownership malformed or overlaps a recorded path");
    }
}
uint256 IndexScripts(const ScriptOwners& paths,const ScriptOwners& historical) {
    CheckIndexOwners(paths,historical);
    if(historical.empty())return Scripts(paths); // Existing receipt bytes stay valid.
    std::string binding="DNIH01";Hash(binding,Scripts(paths));Hash(binding,Scripts(historical));return Digest(binding);
}
bool SetIndexOwner(WalletUTXO& row,const ScriptOwners& paths,const ScriptOwners& historical) {
    const auto path=paths.find(row.spk),imported=historical.find(row.spk);
    if(path!=paths.end() && imported!=historical.end())Fail("Index script has ambiguous ownership");
    if(imported!=historical.end()) {
        row.path.clear();row.owner_kind=WalletOutputOwner::HistoricalImport;row.owner_reference=imported->second;return true;
    }
    if(path==paths.end())return false;
    row.path=path->second;row.owner_kind=WalletOutputOwner::RecordedPath;row.owner_reference.clear();return true;
}
bool IndexRowOwner(sqlite3_stmt* q,const std::vector<uint8_t>& script,const ScriptOwners& paths,
                   const ScriptOwners& historical) {
    const auto text=[&](int n) {
        if(sqlite3_column_type(q,n)!=SQLITE_TEXT)Fail("Index owner text type invalid");
        const auto* p=reinterpret_cast<const char*>(sqlite3_column_text(q,n));const int size=sqlite3_column_bytes(q,n);
        if(!p || size<0 || std::memchr(p,0,size))Fail("Index owner text malformed");return std::string(p,size);
    };
    if(sqlite3_column_type(q,9)!=SQLITE_INTEGER)Fail("Index owner kind type invalid");
    WalletUTXO expected;expected.spk=script;
    return SetIndexOwner(expected,paths,historical) && text(4)==expected.path &&
        sqlite3_column_int64(q,9)==static_cast<uint8_t>(expected.owner_kind) && text(10)==expected.owner_reference;
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
std::string ExistingIdentity(sqlite3* db) {
    Statement q(db,"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1");
    if(sqlite3_step(q.value)!=SQLITE_ROW || sqlite3_column_type(q.value,0)!=SQLITE_BLOB ||
       sqlite3_column_bytes(q.value,0)!=32)Fail("Coverage existing identity unavailable");
    const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q.value,0));
    if(!bytes || std::all_of(bytes,bytes+32,[](uint8_t n){return n==0;}))Fail("Coverage existing identity malformed");
    static constexpr char hex[]="0123456789abcdef";
    std::string id="DNWI01:";for(size_t i=0;i<32;++i){id+=hex[bytes[i]>>4];id+=hex[bytes[i]&15];}
    if(sqlite3_step(q.value)!=SQLITE_DONE)Fail("Coverage existing identity read failed");
    return id;
}
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
std::set<TxOutPoint> IndexOriginRows(sqlite3* db,const ScriptOwners& scripts,const ScriptOwners& historical,
        const RuntimeWalletOriginProjection& source,const RuntimeOutboxEvent* applied) {
    const auto coins=OriginCoins(source,applied);std::set<TxOutPoint> retained;
    Statement rows(db,"SELECT txid,vout,value,spk,path,height,spend_height,is_coinbase,is_confidential,owner_kind,owner_reference FROM wallet_utxos");int rc;
    while((rc=sqlite3_step(rows.value))==SQLITE_ROW) {
        if(sqlite3_column_type(rows.value,0)!=SQLITE_TEXT || sqlite3_column_type(rows.value,3)!=SQLITE_BLOB)
            Fail("Origin index outpoint or script type invalid");
        const auto* id_bytes=reinterpret_cast<const char*>(sqlite3_column_text(rows.value,0));
        const int id_size=sqlite3_column_bytes(rows.value,0);
        const auto* spk=static_cast<const uint8_t*>(sqlite3_column_blob(rows.value,3));
        const int script_size=sqlite3_column_bytes(rows.value,3);
        if(!id_bytes || id_size!=64 || !spk || script_size<=0 || script_size>10000)
            Fail("Origin index row malformed");
        const std::string id(id_bytes,id_size);
        if(!std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))
            Fail("Origin index transaction id malformed");
        for(const auto col:{1,2,5,7,8})if(sqlite3_column_type(rows.value,col)!=SQLITE_INTEGER)Fail("Origin index integer malformed");
        const auto output_index=sqlite3_column_int64(rows.value,1);
        if(output_index<0 || output_index>UINT32_MAX)Fail("Origin index output index invalid");
        const auto found=coins.find(TxOutPoint{TxId(uint256::FromHexUnsafe(id)),uint32_t(output_index)});
        if(found==coins.end() || found->first.txid.AsUint256().GetHex()!=id)Fail("Origin index row outside selected history");
        retained.insert(found->first);
        const auto& c=found->second;
        if(!IndexRowOwner(rows.value,c.output.scriptPubKey,scripts,historical) ||
           c.output.scriptPubKey!=std::vector<uint8_t>(spk,spk+script_size) ||
           sqlite3_column_int64(rows.value,2)!=int64_t(c.output.value.GetUna()) ||
           sqlite3_column_int64(rows.value,5)!=c.height || sqlite3_column_int64(rows.value,7)!=c.coinbase ||
           sqlite3_column_int64(rows.value,8)!=0 ||
           (sqlite3_column_type(rows.value,6)!=SQLITE_NULL && (!c.spent || sqlite3_column_type(rows.value,6)!=SQLITE_INTEGER || sqlite3_column_int64(rows.value,6)!=c.spent->height)) ||
           (applied && c.spent && sqlite3_column_type(rows.value,6)==SQLITE_NULL))
            Fail("Origin index row reconciliation required");
    }
    if(rc!=SQLITE_DONE)Fail("Origin index inventory failed");
    if(applied)for(const auto& [point,c]:coins)if((scripts.count(c.output.scriptPubKey)||historical.count(c.output.scriptPubKey)) && !retained.count(point))
        Fail("Origin index receipt lacks baseline");
    return retained;
}
} // namespace

void RuntimeIndexDelivery::CaptureOriginDomain(UTXOIndex& index,RuntimeWalletOriginProjection& source) {
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_ || index.atomic_write_active_ || !sqlite3_get_autocommit(index.db_))Fail("Origin index ownership unavailable");
    source.index_scripts_=index.watched_scripts_;source.index_historical_scripts_=index.historical_scripts_;source.index_path_=index.db_path_;
    CheckIndexOwners(index.watched_scripts_,source.historical_scripts_);
    for(const auto& [script,path]:index.watched_scripts_) {
        if(script.empty() || script.size()>10000 || path.empty())Fail("Origin index script malformed");
        source.scripts_.try_emplace(script,"");
    }
}
void RuntimeIndexDelivery::CheckOriginDomain(UTXOIndex& index,const RuntimeWalletOriginProjection& source) {
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_ || index.atomic_write_active_ || !sqlite3_get_autocommit(index.db_) ||
       !source.index_scripts_ || *source.index_scripts_!=index.watched_scripts_ ||
       !source.index_historical_scripts_ || (*source.index_historical_scripts_!=index.historical_scripts_ && source.historical_scripts_!=index.historical_scripts_) || source.index_path_!=index.db_path_)
        Fail("Origin index domain changed");
}

std::optional<RuntimeIndexProgress> RuntimeIndexDelivery::Read(UTXOIndex& index,const std::string& identity,const HistoricalScripts* historical) {
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_ || !sqlite3_get_autocommit(index.db_))Fail("Index delivery read ownership unavailable");
    if((historical && *historical!=index.historical_scripts_) || (!historical && !index.historical_scripts_.empty()))
        Fail("Index historical owner changed; coverage reconciliation required");
    const auto receipt=dinero::Read(index.db_,identity,IndexScripts(index.watched_scripts_,index.historical_scripts_));
    return receipt?std::optional(receipt->progress):std::nullopt;
}
RuntimeIndexProgress RuntimeIndexDelivery::Apply(UTXOIndex& index,const std::string& identity,const RuntimeOutboxEvent& event,const RuntimeWalletOriginProjection* origin,const std::function<void()>& finish,const HistoricalScripts* historical) {
    const auto effects=Effects(event);Identity(identity);
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_ || index.atomic_write_active_ || !sqlite3_get_autocommit(index.db_))Fail("Index delivery write ownership unavailable");
    if(origin && (!origin->index_scripts_ || *origin->index_scripts_!=index.watched_scripts_ ||
        !origin->index_historical_scripts_ || (*origin->index_historical_scripts_!=index.historical_scripts_ && origin->historical_scripts_!=index.historical_scripts_) || origin->index_path_!=index.db_path_))
        Fail("Origin index domain changed");
    if(!origin && ((historical && *historical!=index.historical_scripts_) || (!historical && !index.historical_scripts_.empty())))
        Fail("Index historical owner requires proved coverage before delivery");
    auto published=origin?origin->historical_scripts_:index.historical_scripts_; // Allocate before effects.
    const auto ownership=IndexScripts(index.watched_scripts_,published);const auto existing=dinero::Read(index.db_,identity,ownership);
    const auto before=Before(event),after=After(event);const auto profile=Profile(event.context);
    if(existing && existing->progress.cursor==event.cursor) {
        if(existing->profile!=profile || Tip{existing->progress.tip_hash,existing->progress.tip_height}!=after)
            Fail("Index delivery replay mismatch");
        if(origin)IndexOriginRows(index.db_,index.watched_scripts_,published,*origin,&event);
        if(finish)finish();
        index.historical_scripts_.swap(published);
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
            const auto retained=IndexOriginRows(index.db_,index.watched_scripts_,published,*origin,nullptr);
            for(const auto& [point,c]:origin->Coins()) {
                WalletUTXO row(point.txid,point.vout,c.output.value,c.output.scriptPubKey,"",c.height,c.coinbase);
                if(!SetIndexOwner(row,index.watched_scripts_,published))continue;
                if(!retained.count(point) && !index.AddUTXOForDelivery(row,published))Fail("Origin index creation failed");
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
                if(output.amount>uint64_t(INT64_MAX))Fail("Index delivery amount out of range");
                WalletUTXO row(output.txid,output.n,AmountUna::Una(output.amount),output.script,"",event.context.height,output.coinbase);
                if(!SetIndexOwner(row,index.watched_scripts_,published))continue;
                if(!index.AddUTXOForDelivery(row,published))Fail("Index delivery output failed");
            }
        }
        Statement write(index.db_,"INSERT OR REPLACE INTO utxo_metadata(key,value) VALUES(?,?)");write.Text(1,receipt_key);write.Blob(2,bytes);write.Done();
        Exec(index.db_,"DELETE FROM utxo_metadata WHERE key='runtime_delivery:v1:invalidated'");
    });
    if(finish)finish();
    index.historical_scripts_.swap(published);
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
struct OrdinaryOwnership {std::map<std::vector<uint8_t>,std::string> addresses,historical;uint256 digest;};
OrdinaryOwnership OrdinaryScripts(WalletManager::DatabaseLease& lease) {
    auto* db=lease.Database();
    OrdinaryOwnership result;std::map<std::vector<uint8_t>,std::string> paths;
    result.historical=lease.ReadHistoricalImportScriptsInTransaction();
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
    for(const auto& [script,address]:result.historical) {
        auto [it,inserted]=result.addresses.emplace(script,address);
        if(!inserted) {
            if(!it->second.empty()&&it->second!=address)Fail("Historical import display ownership conflicts");
            it->second=address;
        }
    }
    std::string binding;Hash(binding,Scripts(paths));Hash(binding,Scripts(result.addresses));
    // Preserve the established empty-import digest. A real predecessor owner
    // adds an explicit domain tag; it never masquerades as a recorded HD path.
    if(!result.historical.empty()){binding.append("DNHIS01",7);Hash(binding,Scripts(result.historical));}
    result.digest=Digest(binding);return result;
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
        WalletManager& wallet,uint64_t session,UTXOIndex* index,bool existing_identity_only) {
    const auto lease=wallet.AcquireDatabaseLease();
    if(wallet.database_leases_!=1 || lease->Session()!=session)
        Fail("Origin projection wallet ownership changed");
    auto result=std::unique_ptr<RuntimeWalletOriginProjection>(new RuntimeWalletOriginProjection());
    result->existing_identity_only_=existing_identity_only;result->session_=session;
    if(!existing_identity_only)result->identity_=lease->EnsureDeliveryIdentity();
    OrdinaryTransaction transaction(lease->Database());
    if(existing_identity_only)result->identity_=ExistingIdentity(lease->Database());
    const auto domain=OrdinaryScripts(*lease);
    result->scripts_=domain.addresses;result->scripts_digest_=domain.digest;
    result->historical_scripts_=domain.historical;
    if(index)RuntimeIndexDelivery::CaptureOriginDomain(*index,*result);
    transaction.Commit();return result;
}
void RuntimeOrdinaryDelivery::CheckOriginDomain(WalletManager& wallet,const RuntimeWalletOriginProjection& source,UTXOIndex* index) {
    const auto lease=wallet.AcquireDatabaseLease();
    if(wallet.database_leases_!=1 || lease->Session()!=source.session_)
        Fail("Origin projection wallet ownership changed");
    if(!source.existing_identity_only_ && lease->EnsureDeliveryIdentity()!=source.identity_)
        Fail("Origin projection wallet ownership changed");
    OrdinaryTransaction transaction(lease->Database());
    if(source.existing_identity_only_ && ExistingIdentity(lease->Database())!=source.identity_)
        Fail("Origin projection wallet ownership changed");
    const auto domain=OrdinaryScripts(*lease);
    if(domain.digest!=source.scripts_digest_ || domain.historical!=source.historical_scripts_)
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

std::optional<std::pair<uint256,uint32_t>> RuntimeWalletCoverageProjection::PrefixTip(
        RuntimeOutboxCursor cursor) const {
    if(!cursor.sequence || cursor.sequence>prefixes_.size())return std::nullopt;
    const auto& prefix=prefixes_[static_cast<size_t>(cursor.sequence-1)];
    if(prefix.cursor!=cursor)return std::nullopt;
    return std::pair<uint256,uint32_t>{prefix.hash,prefix.height};
}

std::shared_ptr<const RuntimeWalletCoverageProjection> RuntimeWalletCoverageProjection::Build(
        std::shared_ptr<const RuntimeWalletOriginProjection> origin,
        std::shared_ptr<const RuntimeAccountReplay> replay) {
    if(!origin || !origin->existing_identity_only_ || !replay || !replay->Head().sequence ||
       origin->first_.cursor!=replay->Event(1)->cursor ||
       origin->first_.context.parent_hash!=replay->Event(1)->context.parent_hash)
        Fail("Coverage origin and runtime source differ");
    auto result=std::shared_ptr<RuntimeWalletCoverageProjection>(new RuntimeWalletCoverageProjection());
    result->origin_=std::move(origin);result->replay_=std::move(replay);
    result->profile_digest_=Profile(result->origin_->first_.context);
    result->coins_=result->origin_->Coins();
    result->tip_hash_=result->origin_->OriginHash();result->tip_height_=result->origin_->OriginHeight();
    // Operational retained-material bound, not a resident-memory certificate.
    // Charge the whole traversal, including activity subsequently disconnected.
    size_t charged=result->origin_->retained_bytes_;
    constexpr size_t limit=64*1024*1024;
    const auto charge=[&](size_t n){if(charged>limit || n>limit-charged)Fail("Coverage material limit");charged+=n;};
    const auto activity=[&](const Effect& e,uint32_t height) {
        Activity h{e.id,height,e.time,e.coinbase,e.spent,{}};
        charge(sizeof(Activity));
        for(const auto& point:e.spent){(void)point;charge(sizeof(TxOutPoint));}
        for(const auto& output:e.created) {
            if(output.txid!=e.id || output.n!=h.outputs.size() || output.amount>uint64_t(INT64_MAX))
                Fail("Coverage output malformed");
            charge(sizeof(TxOutput));charge(output.script.size());
            h.outputs.emplace_back(AmountUna::Una(output.amount),output.script);
        }
        return h;
    };
    const auto same=[](const Activity& a,const Activity& b) {
        if(a.txid!=b.txid || a.height!=b.height || a.time!=b.time || a.coinbase!=b.coinbase ||
           a.inputs!=b.inputs || a.outputs.size()!=b.outputs.size())return false;
        for(size_t i=0;i<a.outputs.size();++i)
            if(a.outputs[i].value!=b.outputs[i].value || a.outputs[i].scriptPubKey!=b.outputs[i].scriptPubKey)return false;
        return true;
    };
    const auto observe_coin=[&](const TxOutPoint& point,const Coin& coin) {
        charge(sizeof(TxOutPoint)+sizeof(Coin));charge(coin.output.scriptPubKey.size());
        result->observed_coins_[point].push_back(coin);
    };
    const auto observe_activity=[&](const Activity& h) {
        charge(sizeof(TxId)+sizeof(Activity));
        for(const auto& input:h.inputs){(void)input;charge(sizeof(TxOutPoint));}
        for(const auto& output:h.outputs){charge(sizeof(TxOutput));charge(output.scriptPubKey.size());}
        result->observed_history_[h.txid].push_back(h);
    };
    for(const auto& [point,coin]:result->coins_) {
        observe_coin(point,coin);
        // Independent origin replay proved creation before any later spend.
        if(coin.spent){auto creation=coin;creation.spent.reset();observe_coin(point,creation);}
    }
    for(const auto& h:result->origin_->Transactions()) {
        Effect e;e.id=h.transaction.GetTxid();e.coinbase=h.transaction.IsCoinbase();e.time=h.time;
        if(!e.coinbase)for(const auto& in:h.transaction.vin)e.spent.push_back(in.prevout);
        for(size_t i=0;i<h.transaction.vout.size();++i){const auto& out=h.transaction.vout[i];
            if(i>UINT32_MAX || out.is_confidential)Fail("Coverage historical output unsupported");
            e.created.push_back({e.id,uint32_t(i),out.value.GetUna(),out.scriptPubKey,e.coinbase});}
        auto recorded=activity(e,h.height);observe_activity(recorded);
        if(!result->history_.emplace(e.id,std::move(recorded)).second)Fail("Coverage historical transaction repeated");
    }
    const auto target=result->replay_->Head();
    for(uint64_t sequence=1;;++sequence) {
        const auto event_handle=result->replay_->Event(sequence);const auto& event=*event_handle;
        if(event.cursor.sequence!=sequence || event.previous_digest!=result->head_.digest ||
           Profile(event.context)!=Profile(result->origin_->first_.context) ||
           Before(event)!=Tip{result->tip_hash_,result->tip_height_})Fail("Coverage source discontinuity");
        const auto effects=Effects(event);
        const auto connect=[&](const Effect& e) {
            bool relevant=false;
            for(const auto& point:e.spent) {
                const auto c=result->coins_.find(point);if(c==result->coins_.end())continue;
                if(c->second.spent)Fail("Coverage owned coin spent twice");
                c->second.spent=RuntimeWalletOriginProjection::Spend{e.id,event.context.height,e.time};
                observe_coin(point,c->second);relevant=true;
            }
            for(const auto& out:e.created) {
                if(!result->origin_->scripts_.count(out.script))continue;
                if(out.amount>uint64_t(INT64_MAX))Fail("Coverage owned amount out of range");
                charge(sizeof(Coin));charge(out.script.size());
                if(!result->coins_.emplace(TxOutPoint{out.txid,out.n},Coin{
                    TxOutput(AmountUna::Una(out.amount),out.script),event.context.height,e.time,e.coinbase,{}}).second)
                    Fail("Coverage owned output repeated");
                observe_coin(TxOutPoint{out.txid,out.n},result->coins_.at(TxOutPoint{out.txid,out.n}));
                relevant=true;
            }
            if(relevant) {
                auto recorded=activity(e,event.context.height);observe_activity(recorded);
                if(!result->history_.emplace(e.id,std::move(recorded)).second)Fail("Coverage selected transaction repeated");
            }
        };
        const auto disconnect=[&](const Effect& e) {
            bool relevant=false;
            // Reverse transaction order below ensures a same-block child is
            // undone before its parent's output is removed.
            for(const auto& out:e.created) {
                if(!result->origin_->scripts_.count(out.script))continue;
                const auto c=result->coins_.find(TxOutPoint{out.txid,out.n});
                if(c==result->coins_.end() || c->second.spent || c->second.height!=event.context.height ||
                   c->second.time!=e.time || c->second.coinbase!=e.coinbase ||
                   c->second.output.value.GetUna()!=out.amount || c->second.output.scriptPubKey!=out.script)
                    Fail("Coverage disconnect creation mismatch");
                result->coins_.erase(c);relevant=true;
            }
            for(const auto& point:e.spent) {
                const auto c=result->coins_.find(point);if(c==result->coins_.end())continue;
                if(!c->second.spent || c->second.spent->txid!=e.id ||
                   c->second.spent->height!=event.context.height || c->second.spent->time!=e.time)
                    Fail("Coverage disconnect spend mismatch");
                c->second.spent.reset();observe_coin(point,c->second);relevant=true;
            }
            const auto h=result->history_.find(e.id);
            if(relevant) {
                if(h==result->history_.end() || !same(h->second,activity(e,event.context.height)))
                    Fail("Coverage disconnect history mismatch");
                result->history_.erase(h);
            } else if(h!=result->history_.end())Fail("Coverage unexplained transaction history");
        };
        if(event.direction==RuntimeBlockDirection::Connect)for(const auto& e:effects)connect(e);
        else for(auto i=effects.rbegin();i!=effects.rend();++i)disconnect(*i);
        const auto after=After(event);const auto point=result->replay_->Point(event.cursor).checkpoint;
        if(point.block_hash!=after.first || point.height!=after.second)Fail("Coverage runtime checkpoint mismatch");
        // Preserve the already verified prefix before any writer takes locks.
        // Charge retained values to the existing operational material budget.
        charge(sizeof(PrefixFact));
        result->prefixes_.push_back({event.cursor,after.first,after.second});
        result->tip_hash_=after.first;result->tip_height_=after.second;result->head_=event.cursor;
        if(sequence==target.sequence)break;
        if(sequence==UINT64_MAX)Fail("Coverage sequence overflow");
    }
    if(result->head_!=target)Fail("Coverage incomplete runtime source");
    return result;
}

std::optional<RuntimeIndexProgress> RuntimeOrdinaryDelivery::ReadForWallet(WalletManager& wallet,uint64_t session) {
    const auto lease=wallet.AcquireDatabaseLease();if(lease->Session()!=session)Fail("Wallet delivery selection changed");
    const auto identity=lease->EnsureDeliveryIdentity();auto* db=lease->Database();
    OrdinaryTransaction transaction(db);const auto scripts=OrdinaryScripts(*lease);const auto receipt=OrdinaryRead(db,identity,scripts.digest);
    transaction.Commit();return receipt?std::optional(receipt->progress):std::nullopt;
}
void RuntimeOrdinaryDelivery::AdoptOrigin(WalletManager& wallet,UTXOIndex& index,const RuntimeWalletOriginProjection& source) {
    const auto lease=wallet.AcquireDatabaseLease();
    if(wallet.database_leases_!=1 || lease->Session()!=source.session_ || lease->EnsureDeliveryIdentity()!=source.identity_ ||
       !source.index_scripts_ || source.first_.cursor.sequence!=1 || source.first_.direction!=RuntimeBlockDirection::Connect)
        Fail("Origin adoption ownership unavailable");
    { OrdinaryTransaction transaction(lease->Database());
      const auto domain=OrdinaryScripts(*lease);
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

namespace {
std::string CoverageText(sqlite3_stmt* q,int col) {
    if(sqlite3_column_type(q,col)!=SQLITE_TEXT)Fail("Coverage text type invalid");
    const auto* p=static_cast<const char*>(sqlite3_column_blob(q,col));const int n=sqlite3_column_bytes(q,col);
    if(!p||n<=0||std::memchr(p,0,n))Fail("Coverage text invalid");return std::string(p,n);
}
std::optional<std::string> CoverageMetadata(sqlite3* db,const char* key,int type) {
    Statement q(db,"SELECT value FROM utxo_metadata WHERE key=?");q.Text(1,key);const int rc=sqlite3_step(q.value);
    if(rc==SQLITE_DONE)return {};
    if(rc!=SQLITE_ROW||sqlite3_column_type(q.value,0)!=type)Fail("Coverage index metadata type invalid");
    const auto* bytes=static_cast<const char*>(sqlite3_column_blob(q.value,0));const int size=sqlite3_column_bytes(q.value,0);
    if(!bytes||size<1||size>2048)Fail("Coverage index metadata malformed");
    std::string value(bytes,size);if(sqlite3_step(q.value)!=SQLITE_DONE)Fail("Coverage index metadata incomplete");return value;
}
int64_t CoverageInt(sqlite3_stmt* q,int col) {
    if(sqlite3_column_type(q,col)!=SQLITE_INTEGER)Fail("Coverage integer type invalid");return sqlite3_column_int64(q,col);
}
TxOutPoint CoveragePoint(sqlite3_stmt* q,int id_column,int index_column) {
    const auto id=CoverageText(q,id_column);const auto n=CoverageInt(q,index_column);
    if(id.size()!=64||n<0||n>UINT32_MAX)Fail("Coverage outpoint invalid");
    const auto point=TxOutPoint{TxId(uint256::FromHexUnsafe(id)),uint32_t(n)};
    if(point.txid.AsUint256().GetHex()!=id)Fail("Coverage outpoint encoding invalid");return point;
}
uint256 RecordedScripts(const std::string& bytes,std::string_view magic,const std::string& identity) {
    if(bytes.size()<32)Fail("Coverage receipt malformed");
    Reader r{std::string_view(bytes).substr(0,bytes.size()-32)};
    if(r.Take(6)!=magic||r.Take(r.Number(2))!=identity)Fail("Coverage receipt owner mismatch");
    return r.Hash(); // Decode below checks complete framing and checksum.
}
void CheckCoverageReceipt(const std::string& bytes,std::string_view magic,const std::string& identity,
        const RuntimeWalletCoverageProjection& source) {
    const auto stored=Decode(bytes,identity,RecordedScripts(bytes,magic,identity),magic);
    const auto& progress=stored.progress;
    if(stored.profile!=source.ProfileDigest()||progress.origin_hash!=source.Origin().OriginHash()||
       progress.origin_height!=source.Origin().OriginHeight()||progress.cursor.sequence>source.Head().sequence)
        Fail("Coverage receipt source mismatch");
    const auto point=source.PrefixTip(progress.cursor);
    if(!point||point->first!=progress.tip_hash||point->second!=progress.tip_height)
        Fail("Coverage receipt prefix mismatch");
}
void CoverageDurability(sqlite3* db) {
    if(!db||!sqlite3_get_autocommit(db))Fail("Coverage requires released transaction");
    Exec(db,"PRAGMA synchronous=FULL");Statement q(db,"PRAGMA synchronous");
    if(sqlite3_step(q.value)!=SQLITE_ROW||CoverageInt(q.value,0)!=2||sqlite3_step(q.value)!=SQLITE_DONE)
        Fail("Coverage durability unavailable");
}
std::set<TxOutPoint> CoverageIndexRows(sqlite3* db,const ScriptOwners& scripts,const ScriptOwners& historical,
        const RuntimeWalletCoverageProjection& source) {
    Statement q(db,"SELECT txid,vout,value,spk,path,height,spend_height,is_coinbase,is_confidential,owner_kind,owner_reference FROM wallet_utxos");
    std::set<TxOutPoint> existing;int rc;
    while((rc=sqlite3_step(q.value))==SQLITE_ROW) {
        const auto point=CoveragePoint(q.value,0,1);const auto found=source.ObservedCoins().find(point);
        if(found==source.ObservedCoins().end()||!existing.insert(point).second)Fail("Coverage index row outside proved history");
        const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(q.value,3));const int n=sqlite3_column_bytes(q.value,3);
        if(sqlite3_column_type(q.value,3)!=SQLITE_BLOB||!bytes||n<=0||n>10000)Fail("Coverage index script malformed");
        const std::vector<uint8_t> script(bytes,bytes+n);
        if(!IndexRowOwner(q.value,script,scripts,historical)||CoverageInt(q.value,8)!=0)
            Fail("Coverage index owner mismatch");
        const auto amount=CoverageInt(q.value,2),height=CoverageInt(q.value,5),coinbase=CoverageInt(q.value,7);
        const auto spent=sqlite3_column_type(q.value,6)==SQLITE_NULL?std::optional<int64_t>{}:CoverageInt(q.value,6);
        const bool proved=std::any_of(found->second.begin(),found->second.end(),[&](const auto& coin) {
            return script==coin.output.scriptPubKey&&amount==int64_t(coin.output.value.GetUna())&&
                height==coin.height&&coinbase==coin.coinbase&&
                (spent?(coin.spent&&*spent==coin.spent->height):!coin.spent);
        });
        if(!proved)Fail("Coverage index row lacks a proved version");
    }
    if(rc!=SQLITE_DONE)Fail("Coverage index inventory incomplete");return existing;
}
struct CoverageOrdinaryPlan {
    std::set<TxOutPoint> existing_coins;
    std::set<std::string> existing_history;
    std::set<std::string> unconfirm_history;
};
CoverageOrdinaryPlan CoverageOrdinaryRows(sqlite3* db,int wallet_id,const OrdinaryOwnership& scripts,
        const RuntimeWalletCoverageProjection& source) {
    CoverageOrdinaryPlan plan;int rc;
    Statement coins(db,"SELECT wallet_id,txid,vout,amount,script_pubkey,height,is_coinbase,is_spent,spent_txid,spent_height FROM utxos");
    while((rc=sqlite3_step(coins.value))==SQLITE_ROW) {
        auto* q=coins.value;const auto point=CoveragePoint(q,1,2);const auto found=source.ObservedCoins().find(point);
        if(CoverageInt(q,0)!=wallet_id||found==source.ObservedCoins().end()||!plan.existing_coins.insert(point).second)
            Fail("Coverage ordinary row outside proved history");
        const auto script=Unhex(CoverageText(q,4));const auto spent=CoverageInt(q,7);
        const auto amount=CoverageInt(q,3),height=CoverageInt(q,5),coinbase=CoverageInt(q,6);
        if(!scripts.addresses.contains(script)||(spent!=0&&spent!=1))Fail("Coverage ordinary owner or spend flag invalid");
        std::string spender;int64_t spend_height=0;
        if(spent){spender=CoverageText(q,8);spend_height=CoverageInt(q,9);}
        else if(sqlite3_column_type(q,8)!=SQLITE_NULL||sqlite3_column_type(q,9)!=SQLITE_NULL)
            Fail("Coverage ordinary unspent row has spender metadata");
        const bool proved=std::any_of(found->second.begin(),found->second.end(),[&](const auto& coin) {
            return script==coin.output.scriptPubKey&&amount==int64_t(coin.output.value.GetUna())&&
                height==coin.height&&coinbase==coin.coinbase&&
                (spent?(coin.spent&&spender==coin.spent->txid.AsUint256().GetHex()&&spend_height==coin.spent->height):!coin.spent);
        });
        if(!proved)Fail("Coverage ordinary row lacks a proved version");
    }
    if(rc!=SQLITE_DONE)Fail("Coverage ordinary inventory incomplete");
    Statement history(db,"SELECT wallet_id,txid,height,is_coinbase,confirmations FROM transactions");
    while((rc=sqlite3_step(history.value))==SQLITE_ROW) {
        auto* q=history.value;const auto id=CoverageText(q,1);const auto height=CoverageInt(q,2),coinbase=CoverageInt(q,3);
        if(CoverageInt(q,0)!=wallet_id||id.size()!=64||height<0||height>UINT32_MAX||
           (coinbase!=0&&coinbase!=1)||CoverageInt(q,4)<0)
            Fail("Coverage history row malformed");
        const auto txid=TxId(uint256::FromHexUnsafe(id));if(txid.AsUint256().GetHex()!=id)Fail("Coverage history identity malformed");
        const auto found=source.Transactions().find(txid);
        if(height>0) {
            const auto observed=source.ObservedTransactions().find(txid);
            if(observed==source.ObservedTransactions().end()||
               !std::any_of(observed->second.begin(),observed->second.end(),[&](const auto& h){return h.height==height&&h.coinbase==bool(coinbase);}))
                Fail("Coverage confirmed history lacks a proved version");
            if(found==source.Transactions().end())plan.unconfirm_history.insert(id);
        }
        if(found!=source.Transactions().end()&&found->second.coinbase!=bool(coinbase))Fail("Coverage history kind mismatch");
        plan.existing_history.insert(id);
    }
    if(rc!=SQLITE_DONE)Fail("Coverage history inventory incomplete");
    for(const auto& [id,versions]:source.ObservedTransactions())for(const auto& activity:versions) {
        bool spends=false;
        for(const auto& input:activity.inputs) {
            const auto coin=source.ObservedCoins().find(input);
            if(coin!=source.ObservedCoins().end())for(const auto& version:coin->second)
                spends|=scripts.addresses.contains(version.output.scriptPubKey);
        }
        if(spends&&!plan.existing_history.contains(id.AsUint256().GetHex()))
            Fail("Coverage originated history unavailable");
    }
    return plan;
}
void WriteCoverageOrdinary(sqlite3* db,int wallet_id,const OrdinaryOwnership& scripts,
        const RuntimeWalletCoverageProjection& source,const CoverageOrdinaryPlan& plan) {
    for(const auto& point:plan.existing_coins)if(!source.Coins().contains(point)) {
        // Only a matched, proved former creation can be removed.
        Statement remove(db,"DELETE FROM utxos WHERE wallet_id=? AND txid=? AND vout=?");
        remove.Int(1,wallet_id);remove.Text(2,point.txid.AsUint256().GetHex());remove.Int(3,point.vout);remove.Done();
        if(sqlite3_changes(db)!=1)Fail("Coverage ordinary disconnected creation removal failed");
    }
    for(const auto& id:plan.unconfirm_history) {
        Statement undo(db,"UPDATE transactions SET height=0,confirmations=0 WHERE wallet_id=? AND txid=?");
        undo.Int(1,wallet_id);undo.Text(2,id);undo.Done();
        if(sqlite3_changes(db)<1)Fail("Coverage ordinary history unconfirmation failed");
    }
    for(const auto& [point,coin]:source.Coins()) {
        const auto owned=scripts.addresses.find(coin.output.scriptPubKey);if(owned==scripts.addresses.end())continue;
        if(!plan.existing_coins.contains(point)) {
            Statement add(db,"INSERT INTO utxos(wallet_id,txid,vout,address,amount,script_pubkey,height,is_coinbase,is_spent,created_at,spent_txid,spent_height) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
            add.Int(1,wallet_id);add.Text(2,point.txid.AsUint256().GetHex());add.Int(3,point.vout);add.Text(4,owned->second);
            add.Int(5,coin.output.value.GetUna());add.Text(6,Hex(coin.output.scriptPubKey));add.Int(7,coin.height);add.Int(8,coin.coinbase);
            add.Int(9,coin.spent.has_value());add.Int(10,coin.time);
            if(coin.spent){add.Text(11,coin.spent->txid.AsUint256().GetHex());add.Int(12,coin.spent->height);}
            add.Done();if(sqlite3_changes(db)!=1)Fail("Coverage ordinary insertion failed");
        } else {
            // Preserve local address/creation metadata and auxiliary columns;
            // update only chain fields to the current proved branch.
            Statement update(db,"UPDATE utxos SET height=?,is_spent=?,spent_txid=?,spent_height=? WHERE wallet_id=? AND txid=? AND vout=?");
            update.Int(1,coin.height);update.Int(2,coin.spent.has_value());
            if(coin.spent){update.Text(3,coin.spent->txid.AsUint256().GetHex());update.Int(4,coin.spent->height);}
            update.Int(5,wallet_id);update.Text(6,point.txid.AsUint256().GetHex());update.Int(7,point.vout);update.Done();
            if(sqlite3_changes(db)!=1)Fail("Coverage ordinary chain update failed");
        }
    }
    for(const auto& [txid,activity]:source.Transactions()) {
        const auto id=txid.AsUint256().GetHex();
        if(plan.existing_history.contains(id)) {
            Statement confirm(db,"UPDATE transactions SET height=? WHERE wallet_id=? AND txid=?");
            confirm.Int(1,activity.height);confirm.Int(2,wallet_id);confirm.Text(3,id);confirm.Done();
            if(sqlite3_changes(db)<1)Fail("Coverage history confirmation failed");
        } else {
            uint64_t credit=0;bool receives=false;std::string address;
            for(const auto& output:activity.outputs) {
                const auto owned=scripts.addresses.find(output.scriptPubKey);if(owned==scripts.addresses.end())continue;
                const auto value=output.value.GetUna();if(value>uint64_t(INT64_MAX)-credit)Fail("Coverage history credit overflow");
                credit+=value;receives=true;if(address.empty())address=owned->second;
            }
            if(!receives)continue;
            Statement add(db,"INSERT INTO transactions(wallet_id,txid,address,amount,confirmations,category,label,time,is_coinbase,height) VALUES(?,?,?,?,0,?,'',?,?,?)");
            add.Int(1,wallet_id);add.Text(2,id);add.Text(3,address);
            if(sqlite3_bind_double(add.value,4,double(credit)/100000000.0)!=SQLITE_OK)Fail("Coverage history bind failed");
            add.Text(5,activity.coinbase?"generate":"receive");add.Int(6,activity.time);add.Int(7,activity.coinbase);add.Int(8,activity.height);add.Done();
            if(sqlite3_changes(db)!=1)Fail("Coverage history insertion failed");
        }
    }
    OrdinaryInt(db,"UPDATE utxos SET confirmations=CASE WHEN height>0 THEN MAX(0,?-height+1) ELSE 0 END",source.TipHeight());
    Exec(db,"UPDATE utxos SET is_mature=CASE WHEN is_coinbase=0 OR confirmations>=100 THEN 1 ELSE 0 END");
    OrdinaryInt(db,"UPDATE transactions SET confirmations=CASE WHEN height>0 THEN MAX(0,?-height+1) ELSE 0 END",source.TipHeight());
    OrdinaryInt(db,"INSERT INTO tip(rowid,height) VALUES(1,?) ON CONFLICT(rowid) DO UPDATE SET height=excluded.height",source.TipHeight());
}
} // namespace

bool RuntimeIndexDelivery::CoverageRequired(UTXOIndex& index,const std::string& identity,const HistoricalScripts* historical) {
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_||index.atomic_write_active_||!sqlite3_get_autocommit(index.db_))Fail("Coverage index read owner unavailable");
    const auto invalid=CoverageMetadata(index.db_,invalid_key,SQLITE_TEXT),receipt=CoverageMetadata(index.db_,receipt_key,SQLITE_BLOB);
    if(invalid&&*invalid!="1")Fail("Coverage index invalidation malformed");
    if(receipt||invalid)CheckTriggers(index.db_);
    if(receipt) {
        const auto recorded=RecordedScripts(*receipt,"DNUI01",identity);
        (void)Decode(*receipt,identity,recorded);
        if(recorded!=IndexScripts(index.watched_scripts_,index.historical_scripts_))return true;
    }
    return bool(invalid)||!receipt||(historical && *historical!=index.historical_scripts_)||(!historical && !index.historical_scripts_.empty());
}
bool RuntimeOrdinaryDelivery::CoverageRequiredForWallet(WalletManager& wallet,UTXOIndex& index,uint64_t session) {
    auto lease=wallet.AcquireDatabaseLease();
    if(wallet.database_leases_!=1||lease->Session()!=session||!lease->Database())Fail("Coverage wallet read owner unavailable");
    auto* db=lease->Database();OrdinaryTransaction transaction(db);const auto identity=ExistingIdentity(db);
    const auto scripts=OrdinaryScripts(*lease);bool required=!OrdinaryColumns(db);
    if(!required) {
        Statement q(db,"SELECT runtime_ordinary_receipt,runtime_ordinary_invalid FROM wallet_meta WHERE id=1");
        if(sqlite3_step(q.value)!=SQLITE_ROW)Fail("Coverage ordinary metadata missing");
        const auto invalid=CoverageInt(q.value,1);if(invalid!=0&&invalid!=1)Fail("Coverage ordinary invalidation malformed");
        required=invalid!=0||sqlite3_column_type(q.value,0)==SQLITE_NULL;
        if(sqlite3_column_type(q.value,0)!=SQLITE_NULL) {
            const auto* bytes=static_cast<const char*>(sqlite3_column_blob(q.value,0));const int n=sqlite3_column_bytes(q.value,0);
            if(sqlite3_column_type(q.value,0)!=SQLITE_BLOB||!bytes||n<=0||n>2048)Fail("Coverage ordinary receipt malformed");
            const std::string receipt(bytes,n);const auto recorded=RecordedScripts(receipt,"DNOW01",identity);
            (void)Decode(receipt,identity,recorded,"DNOW01");required|=recorded!=scripts.digest;
        }
        if(sqlite3_step(q.value)!=SQLITE_DONE)Fail("Coverage ordinary metadata incomplete");
        OrdinaryGuards(db,false);
    }
    // Always read both owners, even when the first already needs coverage.
    const bool index_required=RuntimeIndexDelivery::CoverageRequired(index,identity,&scripts.historical);
    transaction.Commit();return required||index_required;
}

void RuntimeIndexDelivery::ReconcileCoverage(UTXOIndex& index,const RuntimeWalletCoverageProjection& source,
        const std::function<void()>& finish) {
    const auto& origin=source.Origin();
    std::lock_guard<std::recursive_mutex> lock(index.db_mutex_);std::lock_guard<std::mutex> scripts(index.scripts_mutex_);
    if(!index.db_||index.atomic_write_active_||!sqlite3_get_autocommit(index.db_)||
       !origin.index_scripts_||*origin.index_scripts_!=index.watched_scripts_||
       !origin.index_historical_scripts_||(*origin.index_historical_scripts_!=index.historical_scripts_ && origin.historical_scripts_!=index.historical_scripts_)||origin.index_path_!=index.db_path_)
        Fail("Coverage index owner changed");
    auto published=origin.historical_scripts_;
    const auto ownership=IndexScripts(index.watched_scripts_,published);
    CoverageDurability(index.db_);
    index.ApplyAtomically([&] {
        const auto receipt=CoverageMetadata(index.db_,receipt_key,SQLITE_BLOB),invalid=CoverageMetadata(index.db_,invalid_key,SQLITE_TEXT);
        if(invalid&&*invalid!="1")Fail("Coverage index invalidation malformed");
        if(receipt)CheckCoverageReceipt(*receipt,"DNUI01",origin.identity_,source);
        if(receipt||invalid)CheckTriggers(index.db_);
        const auto existing=CoverageIndexRows(index.db_,index.watched_scripts_,published,source);
        for(const auto* action:{"INSERT","UPDATE","DELETE"}){auto sql=Trigger(action);sql.insert(15,"IF NOT EXISTS ");Exec(index.db_,sql.c_str());}
        CheckTriggers(index.db_);
        for(const auto& point:existing)if(!source.Coins().contains(point)) {
            Statement remove(index.db_,"DELETE FROM wallet_utxos WHERE txid=? AND vout=?");
            remove.Text(1,point.txid.AsUint256().GetHex());remove.Int(2,point.vout);remove.Done();
            if(sqlite3_changes(index.db_)!=1)Fail("Coverage index disconnected creation removal failed");
        }
        for(const auto& [point,coin]:source.Coins()) {
            WalletUTXO row(point.txid,point.vout,coin.output.value,coin.output.scriptPubKey,"",coin.height,coin.coinbase);
            if(!SetIndexOwner(row,index.watched_scripts_,published))continue;
            if(!existing.contains(point)) {
                if(!index.AddUTXOForDelivery(row,published))Fail("Coverage index insertion failed");
            }
            Statement update(index.db_,"UPDATE wallet_utxos SET height=?,spend_height=? WHERE txid=? AND vout=?");
            update.Int(1,coin.height);if(coin.spent)update.Int(2,coin.spent->height);
            update.Text(3,point.txid.AsUint256().GetHex());update.Int(4,point.vout);update.Done();
            if(sqlite3_changes(index.db_)!=1)Fail("Coverage index chain update failed");
        }
        Receipt next{ {source.Head(),origin.OriginHash(),source.TipHash(),origin.OriginHeight(),source.TipHeight()},Profile(origin.first_.context)};
        Statement write(index.db_,"INSERT OR REPLACE INTO utxo_metadata(key,value) VALUES(?,?)");
        write.Text(1,receipt_key);write.Blob(2,Encode(next,origin.identity_,ownership));write.Done();
        Exec(index.db_,"DELETE FROM utxo_metadata WHERE key='runtime_delivery:v1:invalidated'");
    });
    // Index committed first. A refusal below leaves its genuine source prefix
    // for a fresh-session retry; its locks remain held through ordinary commit.
    finish();
    index.historical_scripts_.swap(published);
}
std::unique_ptr<wallet::OrchardCatalogCapture> RuntimeOrdinaryDelivery::PrepareCoverage(
        WalletManager& wallet,const RuntimeWalletCoverageProjection& source) {
    {
        const auto lease=wallet.AcquireDatabaseLease();
        if(wallet.database_leases_!=1||!lease->Database()||lease->Session()!=source.Origin().session_||
           !sqlite3_get_autocommit(lease->Database()))Fail("Coverage preparation requires released wallet owners");
    }
    return ::dinero::wallet::OrchardAccountDelivery::PrepareCatalogForReplay(
        wallet,source.Origin().session_,source.replay_);
}
void RuntimeOrdinaryDelivery::ReconcileCoverage(WalletManager& wallet,UTXOIndex& index,
        const RuntimeWalletCoverageProjection& source,const wallet::OrchardCatalogCapture& prepared) {
    const auto& origin=source.Origin();auto lease=wallet.AcquireDatabaseLease();
    if(wallet.database_leases_!=1||lease->Session()!=origin.session_||!origin.existing_identity_only_||
       !origin.index_scripts_||!source.Head().sequence||wallet.current_wallet_id_<0)
        Fail("Coverage wallet owner changed");
    auto* db=lease->Database();CoverageDurability(db);OrdinaryTransaction transaction(db);
    if(ExistingIdentity(db)!=origin.identity_)Fail("Coverage persistent identity changed");
    const auto scripts=OrdinaryScripts(*lease);if(scripts.digest!=origin.scripts_digest_)Fail("Coverage script domain changed");
    // Full scan restoration finished before selected ownership. Reauthenticate
    // every captured account/retained/archive byte and shield history in this
    // exact SQL snapshot before index or ordinary effects; never restore here.
    ::dinero::wallet::OrchardAccountDelivery::RecheckCatalogCaptureInTransaction(
        wallet,origin.session_,source.replay_,prepared);
    auto pin=lease->CopyRecoverySeed(origin.session_);lease->ValidateRecoveryInventoryInTransaction(*pin);
    // The live index must claim exactly the durable watched domain. Extra
    // registrations are not silently adopted as another signing owner.
    std::map<std::vector<uint8_t>,std::string> watched;
    Statement paths(db,"SELECT script_pubkey,path FROM watch_scripts");int path_rc;
    while((path_rc=sqlite3_step(paths.value))==SQLITE_ROW) {
        const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(paths.value,0));const int size=sqlite3_column_bytes(paths.value,0);
        if(sqlite3_column_type(paths.value,0)!=SQLITE_BLOB||!bytes||size<=0||size>10000||
           !watched.emplace(std::vector<uint8_t>(bytes,bytes+size),CoverageText(paths.value,1)).second)
            Fail("Coverage watched domain malformed");
    }
    if(path_rc!=SQLITE_DONE||watched!=*origin.index_scripts_)Fail("Coverage index and durable script domains differ");
    const auto ownership=::dinero::wallet::OrchardOwnershipInventory::Read(db,pin->Bytes());
    const auto pending=lease->ReadPendingPaymentsInTransaction(*pin);
    std::set<std::pair<std::string,uint32_t>> reserved;
    for(const auto& entry:ownership.current_inputs) {
        uint256 hash;std::copy(entry.input.txid_wire.begin(),entry.input.txid_wire.end(),hash.begin());
        if(!reserved.emplace(hash.GetHex(),entry.input.output_index).second)Fail("Coverage Orchard reservations overlap");
        const auto point=TxOutPoint{TxId(hash),entry.input.output_index};const auto coin=source.ObservedCoins().find(point);
        if(coin==source.ObservedCoins().end()||
           !std::any_of(coin->second.begin(),coin->second.end(),[&](const auto& version){
               return version.output.value.GetUna()==entry.input.amount_una&&version.output.scriptPubKey==entry.input.script_pub_key;}))
            Fail("Coverage retained Orchard input is outside proved history");
    }
    for(const auto& payment:pending)for(const auto& input:payment.inputs) {
        if(!reserved.emplace(input.txid,input.vout).second)Fail("Coverage ordinary and Orchard reservations overlap");
        const auto point=TxOutPoint{TxId(uint256::FromHexUnsafe(input.txid)),input.vout};const auto coin=source.ObservedCoins().find(point);
        if(coin==source.ObservedCoins().end()||point.txid.AsUint256().GetHex()!=input.txid||
           !std::any_of(coin->second.begin(),coin->second.end(),[&](const auto& version){
               return version.output.value.GetUna()==input.amount_una&&version.output.scriptPubKey==input.script;}))
            Fail("Coverage retained payment input is outside proved history");
        // A disconnected input remains reserved in its authenticated pending
        // owner. Source reconciliation never discards or resubmits its body.
    }
    const auto plan=CoverageOrdinaryRows(db,wallet.current_wallet_id_,scripts,source);
    if(OrdinaryColumns(db)) {
        Statement q(db,"SELECT runtime_ordinary_receipt,runtime_ordinary_invalid FROM wallet_meta WHERE id=1");
        if(sqlite3_step(q.value)!=SQLITE_ROW)Fail("Coverage ordinary metadata missing");
        const auto invalid=CoverageInt(q.value,1);if(invalid!=0&&invalid!=1)Fail("Coverage ordinary invalidation malformed");
        if(sqlite3_column_type(q.value,0)!=SQLITE_NULL) {
            const auto* bytes=static_cast<const char*>(sqlite3_column_blob(q.value,0));const int n=sqlite3_column_bytes(q.value,0);
            if(sqlite3_column_type(q.value,0)!=SQLITE_BLOB||!bytes||n<=0||n>2048)Fail("Coverage ordinary receipt malformed");
            CheckCoverageReceipt(std::string(bytes,n),"DNOW01",origin.identity_,source);
        }
        if(sqlite3_step(q.value)!=SQLITE_DONE)Fail("Coverage ordinary metadata incomplete");
        OrdinaryGuards(db,false);
    } else {
        Exec(db,"ALTER TABLE wallet_meta ADD COLUMN runtime_ordinary_receipt BLOB");
        Exec(db,"ALTER TABLE wallet_meta ADD COLUMN runtime_ordinary_invalid INTEGER NOT NULL DEFAULT 0");
        OrdinaryGuards(db,true);
    }
    RuntimeIndexDelivery::ReconcileCoverage(index,source,[&] {
        WriteCoverageOrdinary(db,wallet.current_wallet_id_,scripts,source,plan);
        Receipt next{{source.Head(),origin.OriginHash(),source.TipHash(),origin.OriginHeight(),source.TipHeight()},Profile(origin.first_.context)};
        Statement receipt(db,"UPDATE wallet_meta SET runtime_ordinary_receipt=?,runtime_ordinary_invalid=0 WHERE id=1");
        receipt.Blob(1,Encode(next,origin.identity_,scripts.digest,"DNOW01"));receipt.Done();
        if(sqlite3_changes(db)!=1)Fail("Coverage ordinary receipt write failed");
        transaction.Commit();
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
    const auto scripts=OrdinaryScripts(*lease);const auto old=OrdinaryRead(db,identity,scripts.digest);
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

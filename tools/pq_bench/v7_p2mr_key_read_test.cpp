#include "rpc/v7_pq_handlers.h"
#include "wallet/v7_p2mr_store.h"
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>
namespace rpc = dinero::rpc::v7;
namespace w = dinero::wallet;
namespace pq = dinero::consensus::pq::ml_dsa_65;
namespace {
void Check(bool b, const char* why) { if (!b) throw std::runtime_error(why); }
struct Temp {
    std::string dir, path;
    Temp() { char n[]="/tmp/dinero_p2mr_key_read_XXXXXX"; auto p=mkdtemp(n); Check(p,"temp"); dir=p; path=dir+"/wallet.sqlite"; }
    ~Temp() { std::filesystem::remove_all(dir); }
};
struct Raw {
    sqlite3* db=nullptr;
    explicit Raw(const std::string& path) { Check(sqlite3_open(path.c_str(),&db)==SQLITE_OK,"raw open"); }
    ~Raw() { sqlite3_close(db); }
    void Exec(const std::string& q) { Check(sqlite3_exec(db,q.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK,"fixture SQL"); }
};
rpc::ImportP2MRSeedResult Add(w::V7P2MRStore& store, uint8_t n) {
    rpc::ImportP2MRSeedParams p{}; p.wallet_id=7; p.pq_seed.fill(n); p.master_key.fill(11);
    p.hrp="rdin"; p.derivation_path="imported-record-"+std::to_string(n); p.label="keep exact label"; p.now_unix=123+n;
    auto r=rpc::ImportP2MRSeed(store,p); Check(r.status==rpc::HandlerStatus::Ok,"actual import"); return r;
}
rpc::SignP2MRResult Sign(const w::V7P2MRStore& s,const std::string& a,int owner=7,uint8_t key=11) {
    rpc::SignP2MRParams p{};p.wallet_id=owner;p.address=a;p.master_key.fill(key);p.sighash.fill(42);return rpc::SignP2MR(s,p);
}
rpc::ExportP2MRSeedResult Export(const w::V7P2MRStore& s,const std::string& a,int owner=7) {
    rpc::ExportP2MRSeedParams p{};p.wallet_id=owner;p.address=a;p.master_key.fill(11);return rpc::ExportP2MRSeed(s,p);
}
void ExportStatus(const w::V7P2MRStore& s,const std::string& a,rpc::HandlerStatus wanted) {
    auto r=Export(s,a);const bool zero=r.pq_seed==pq::Seed{};OPENSSL_cleanse(r.pq_seed.data(),r.pq_seed.size());
    Check(r.status==wanted,"export must authenticate complete address/key binding");Check(zero,"refused export returns no seed");
}
void SignStatus(const w::V7P2MRStore& s,const std::string& a,rpc::HandlerStatus wanted) {
    const auto r=Sign(s,a);Check(r.status==wanted,"sign must authenticate complete address/key binding");
    Check(r.signature==pq::Signature{},"refused signature empty");
}
void BindingAndReopen() {
    Temp t;w::V7P2MRStore s;Check(s.Open(t.path)==w::V7P2MRStore::OpenResult::Ok,"store");auto a=Add(s,1);Add(s,2);s.Close();
    Check(s.OpenExistingReadOnly(t.path)==w::V7P2MRStore::OpenResult::Ok,"reopen");
    auto good=Sign(s,a.address);std::array<uint8_t,32> msg{};msg.fill(42);
    Check(good.status==rpc::HandlerStatus::Ok && pq::Verify(msg.data(),msg.size(),good.signature.data(),good.signature.size(),good.pubkey.data(),good.pubkey.size()),"real reopened signature");
    auto exported=Export(s,a.address);pq::Seed expected{};expected.fill(1);const bool correct=exported.status==rpc::HandlerStatus::Ok && exported.pq_seed==expected;
    OPENSSL_cleanse(exported.pq_seed.data(),exported.pq_seed.size());Check(correct,"real imported seed preserved");
    Check(Sign(s,a.address,19).status==rpc::HandlerStatus::AddressNotFound,"foreign owner");
    Check(Sign(s,a.address,7,12).status==rpc::HandlerStatus::DecryptFailed,"wrong key");
    Raw r(t.path);
    r.Exec("CREATE TEMP TABLE original AS SELECT * FROM v7_p2mr_addresses WHERE id=1");
    r.Exec("UPDATE v7_p2mr_addresses SET pubkey=(SELECT pubkey FROM v7_p2mr_addresses WHERE id=2) WHERE id=1");
    SignStatus(s,a.address,rpc::HandlerStatus::DerivationMismatch);ExportStatus(s,a.address,rpc::HandlerStatus::DerivationMismatch);
    r.Exec("UPDATE v7_p2mr_addresses SET pubkey=(SELECT pubkey FROM original),seed_ciphertext=zeroblob(32) WHERE id=1");
    SignStatus(s,a.address,rpc::HandlerStatus::DecryptFailed);ExportStatus(s,a.address,rpc::HandlerStatus::DecryptFailed);
    r.Exec("UPDATE v7_p2mr_addresses SET seed_ciphertext=(SELECT seed_ciphertext FROM original),merkle_root=zeroblob(32) WHERE id=1");
    SignStatus(s,a.address,rpc::HandlerStatus::DerivationMismatch);ExportStatus(s,a.address,rpc::HandlerStatus::DerivationMismatch);
    r.Exec("UPDATE v7_p2mr_addresses SET merkle_root=(SELECT merkle_root FROM v7_p2mr_addresses WHERE id=2),pubkey=(SELECT pubkey FROM v7_p2mr_addresses WHERE id=2),seed_ciphertext=(SELECT seed_ciphertext FROM v7_p2mr_addresses WHERE id=2),seed_nonce=(SELECT seed_nonce FROM v7_p2mr_addresses WHERE id=2),seed_tag=(SELECT seed_tag FROM v7_p2mr_addresses WHERE id=2) WHERE id=1");
    // Even a mutually consistent public key/root/ciphertext must match the exact recorded address.
    SignStatus(s,a.address,rpc::HandlerStatus::DerivationMismatch);ExportStatus(s,a.address,rpc::HandlerStatus::DerivationMismatch);
}
sqlite3* reader=nullptr;sqlite3* writer=nullptr;bool interrupt_read=false,replace_row=false,deny_read=false;int mutation_rc=SQLITE_OK;
int Authorize(void*,int op,const char* table,const char*,const char*,const char*) {
    return deny_read && op==SQLITE_READ && table && std::string(table)=="v7_p2mr_addresses" ? SQLITE_DENY : SQLITE_OK;
}
int Trace(unsigned event,void* db,void* stmt,void*) {
    const char* q=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));
    if(event==SQLITE_TRACE_ROW && q && std::string(q).rfind("SELECT id, wallet_id",0)==0) {
        if(interrupt_read) sqlite3_interrupt(static_cast<sqlite3*>(db));
        if(replace_row) {replace_row=false;mutation_rc=sqlite3_exec(writer,"UPDATE v7_p2mr_addresses SET pubkey=(SELECT pubkey FROM v7_p2mr_addresses WHERE id=2),seed_ciphertext=(SELECT seed_ciphertext FROM v7_p2mr_addresses WHERE id=2),seed_nonce=(SELECT seed_nonce FROM v7_p2mr_addresses WHERE id=2),seed_tag=(SELECT seed_tag FROM v7_p2mr_addresses WHERE id=2) WHERE id=1",nullptr,nullptr,nullptr);}
    }
    return 0;
}
int Extension(sqlite3* db,char**,const sqlite3_api_routines*) {reader=db;sqlite3_set_authorizer(db,Authorize,nullptr);return sqlite3_trace_v2(db,SQLITE_TRACE_ROW,Trace,db);}
void OpenTraced(w::V7P2MRStore& s,const std::string& path) {
    Check(sqlite3_auto_extension(reinterpret_cast<void(*)()>(Extension))==SQLITE_OK,"extension");
    auto rc=s.OpenExistingReadOnly(path);sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(Extension));Check(rc==w::V7P2MRStore::OpenResult::Ok,"traced open");
}
void ReadErrorsAndCallerTransaction() {
    Temp t;w::V7P2MRStore write;Check(write.Open(t.path)==w::V7P2MRStore::OpenResult::Ok,"store");auto a=Add(write,1);write.Close();
    w::V7P2MRStore s;OpenTraced(s,t.path);
    deny_read=true;SignStatus(s,a.address,rpc::HandlerStatus::StoreError);ExportStatus(s,a.address,rpc::HandlerStatus::StoreError);deny_read=false;
    interrupt_read=true;SignStatus(s,a.address,rpc::HandlerStatus::StoreError);ExportStatus(s,a.address,rpc::HandlerStatus::StoreError);interrupt_read=false;
    Check(sqlite3_exec(reader,"BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK,"caller begin");
    SignStatus(s,a.address,rpc::HandlerStatus::StoreError);ExportStatus(s,a.address,rpc::HandlerStatus::StoreError);
    Check(sqlite3_get_autocommit(reader)==0,"caller transaction untouched");Check(sqlite3_exec(reader,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK,"caller rollback");
    Raw raw(t.path);raw.Exec("UPDATE v7_p2mr_addresses SET seed_ciphertext=CAST(seed_ciphertext AS TEXT) WHERE id=1");
    SignStatus(s,a.address,rpc::HandlerStatus::StoreError);ExportStatus(s,a.address,rpc::HandlerStatus::StoreError);
}
void OneSnapshotDuringReplacement() {
    Temp t;w::V7P2MRStore write;Check(write.Open(t.path)==w::V7P2MRStore::OpenResult::Ok,"store");auto a=Add(write,1);Add(write,2);write.Close();
    Raw raw(t.path);raw.Exec("PRAGMA journal_mode=WAL");writer=raw.db;
    w::V7P2MRStore s;OpenTraced(s,t.path);replace_row=true;
    const auto signed_old=Sign(s,a.address);Check(mutation_rc==SQLITE_OK && !replace_row,"actual separate WAL writer changed row during read");
    Check(signed_old.status==rpc::HandlerStatus::Ok && signed_old.pubkey==a.pubkey,"one statement must capture matching original metadata and ciphertext");
    std::array<uint8_t,32> message{};message.fill(42);
    Check(pq::Verify(message.data(),message.size(),signed_old.signature.data(),signed_old.signature.size(),a.pubkey.data(),a.pubkey.size()),"captured original key produces a valid signature");
    SignStatus(s,a.address,rpc::HandlerStatus::DerivationMismatch);ExportStatus(s,a.address,rpc::HandlerStatus::DerivationMismatch);
    raw.Exec("DELETE FROM v7_p2mr_addresses WHERE id=1");
    // Restore using the real import owner, preserving the original requested address.
    s.Close();Check(write.Open(t.path)==w::V7P2MRStore::OpenResult::Ok,"restore writer");Add(write,1);write.Close();
    raw.Exec("UPDATE v7_p2mr_addresses SET id=1 WHERE id>2");
    OpenTraced(s,t.path);replace_row=true;
    auto captured=Export(s,a.address);pq::Seed original{};original.fill(1);
    const bool valid=captured.status==rpc::HandlerStatus::Ok && captured.pq_seed==original;
    OPENSSL_cleanse(captured.pq_seed.data(),captured.pq_seed.size());
    Check(mutation_rc==SQLITE_OK && !replace_row && valid,"export uses one original key snapshot");
    ExportStatus(s,a.address,rpc::HandlerStatus::DerivationMismatch);
    writer=nullptr;
}
}
int main() {
    int failures=0;
    for(auto c : {std::pair{"BindingAndReopen",BindingAndReopen},std::pair{"ReadErrorsAndCallerTransaction",ReadErrorsAndCallerTransaction},std::pair{"OneSnapshotDuringReplacement",OneSnapshotDuringReplacement}}) {
        try {c.second();std::printf("[PASS] P2MRKeyRead.%s\n",c.first);}
        catch(const std::exception& e) {std::fprintf(stderr,"[FAIL] P2MRKeyRead.%s: %s\n",c.first,e.what());++failures;}
        deny_read=false;interrupt_read=false;replace_row=false;reader=nullptr;writer=nullptr;
    }
    return failures?1:0;
}

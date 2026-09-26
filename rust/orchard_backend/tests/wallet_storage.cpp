#include "orchard_wallet_storage.h"
#include <sqlite3.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace dinero::orchard;
extern char** environ;
static void Check(bool ok){if(!ok)throw std::runtime_error("Orchard wallet snapshot test failed");}
template<class F>static void Reject(F fn){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}Check(rejected);}
static void Exec(sqlite3* db,const char* sql){Check(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)==SQLITE_OK);}
struct DB {sqlite3* p=nullptr;explicit DB(const char* path){Check(sqlite3_open(path,&p)==SQLITE_OK);Exec(p,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");}~DB(){Check(sqlite3_close(p)==SQLITE_OK);}};
static const std::array<uint8_t,64> seed{7};
static WalletStorageIdentity Identity(){return {WalletNetwork::Regtest,Hash{42},Hash{21},0};}
static WalletStateBytes State(uint8_t n){std::vector<uint8_t> bytes(2048,n);return WalletStateBytes(bytes);}
static std::vector<uint8_t> Stored(sqlite3* db){sqlite3_stmt* s=nullptr;Check(sqlite3_prepare_v2(db,"SELECT sealed FROM orchard_wallet_snapshots",-1,&s,nullptr)==SQLITE_OK);Check(sqlite3_step(s)==SQLITE_ROW);const auto* p=static_cast<const uint8_t*>(sqlite3_column_blob(s,0));std::vector<uint8_t> result(p,p+sqlite3_column_bytes(s,0));sqlite3_finalize(s);return result;}
static void Replace(sqlite3* db,const std::vector<uint8_t>& bytes){sqlite3_stmt* s=nullptr;Check(sqlite3_prepare_v2(db,"UPDATE orchard_wallet_snapshots SET sealed=?",-1,&s,nullptr)==SQLITE_OK);Check(sqlite3_bind_blob(s,1,bytes.data(),int(bytes.size()),SQLITE_TRANSIENT)==SQLITE_OK);Check(sqlite3_step(s)==SQLITE_DONE);sqlite3_finalize(s);}
static int Companion(sqlite3* db){sqlite3_stmt* s=nullptr;Check(sqlite3_prepare_v2(db,"SELECT value FROM companion",-1,&s,nullptr)==SQLITE_OK);Check(sqlite3_step(s)==SQLITE_ROW);int n=sqlite3_column_int(s,0);sqlite3_finalize(s);return n;}
static void Initial(const char* path){DB db(path);Exec(db.p,"BEGIN IMMEDIATE;CREATE TABLE companion(value INTEGER);INSERT INTO companion VALUES(1);");WalletSnapshotStore::InitializeSchemaUnderTransaction(db.p);WalletSnapshotStore store(db.p,Identity(),seed);Check(store.StageReplace(0,State(1))==1);Exec(db.p,"COMMIT;");}
static void Verify(const char* path,uint64_t revision,uint8_t n){DB db(path);WalletSnapshotStore store(db.p,Identity(),seed);auto read=store.Read();Check(read&&read->revision==revision);Check(read->state.Bytes().size()==2048);Check(std::all_of(read->state.Bytes().begin(),read->state.Bytes().end(),[&](auto b){return b==n;}));Check(Companion(db.p)==n);}
static void Crash(const std::string& exe,const std::string& path,bool commit){
    pid_t pid;std::string mode=commit?"post":"pre";std::vector<char*> args{const_cast<char*>(exe.c_str()),const_cast<char*>("--crash"),const_cast<char*>(path.c_str()),mode.data(),nullptr};
    Check(posix_spawn(&pid,exe.c_str(),nullptr,nullptr,args.data(),environ)==0);int status=0;Check(waitpid(pid,&status,0)==pid);Check(WIFEXITED(status)&&WEXITSTATUS(status)==73);
}
int main(int argc,char** argv){try{
    if(argc==4&&std::string_view(argv[1])=="--crash"){
        DB db(argv[2]);WalletSnapshotStore store(db.p,Identity(),seed);Exec(db.p,"BEGIN IMMEDIATE;");Check(store.StageReplaceRetaining(1,State(2))==2);Exec(db.p,"UPDATE companion SET value=2;");
        if(std::string_view(argv[3])=="post")Exec(db.p,"COMMIT;");std::_Exit(73);
    }
    auto pattern=(std::filesystem::temp_directory_path()/"dinero-wallet-snapshot-XXXXXX").string();std::vector<char> dir(pattern.begin(),pattern.end());dir.push_back(0);Check(mkdtemp(dir.data()));
    struct Cleanup {std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{dir.data()};
    const auto path=(cleanup.p/"wallet.sqlite").string();Initial(path.c_str());Verify(path.c_str(),1,1);
    {
        DB db(path.c_str());WalletSnapshotStore store(db.p,Identity(),seed);
        Reject([&]{(void)store.StageReplace(1,State(2));}); // outer transaction required
        const auto sealed=Stored(db.p);Check(sealed.size()==2077);Check(std::search(sealed.begin(),sealed.end(),seed.begin(),seed.end())==sealed.end());
        WalletSnapshotStore wrong(db.p,Identity(),std::array<uint8_t,64>{8});Reject([&]{(void)wrong.Read();});
        auto identity=Identity();identity.network=WalletNetwork::Testnet;WalletSnapshotStore network(db.p,identity,seed);Reject([&]{(void)network.Read();});
        identity=Identity();identity.account=1;WalletSnapshotStore account(db.p,identity,seed);Check(!account.Read());
        Exec(db.p,"INSERT INTO orchard_wallet_snapshots SELECT wallet_id,1,revision,sealed FROM orchard_wallet_snapshots WHERE account=0;");
        Reject([&]{(void)account.Read();});Exec(db.p,"DELETE FROM orchard_wallet_snapshots WHERE account=1;");
        Exec(db.p,"BEGIN IMMEDIATE;");Reject([&]{(void)wrong.StageReplace(1,State(2));});Check(Stored(db.p)==sealed);Exec(db.p,"ROLLBACK;");
        auto bad=sealed;bad[17]^=1;Replace(db.p,bad);Reject([&]{(void)store.Read();});Replace(db.p,sealed);
        Exec(db.p,"UPDATE orchard_wallet_snapshots SET revision=2;");Reject([&]{(void)store.Read();});Exec(db.p,"UPDATE orchard_wallet_snapshots SET revision=1;");
        Exec(db.p,"BEGIN IMMEDIATE;");Reject([&]{(void)store.StageReplace(0,State(2));});Check(store.StageReplace(1,State(2))==2);Exec(db.p,"UPDATE companion SET value=2;ROLLBACK;");
        Check(Stored(db.p)==sealed);Check(Companion(db.p)==1);
        Exec(db.p,"BEGIN IMMEDIATE;");Check(store.StageReplace(1,State(1))==2);Check(Stored(db.p)!=sealed);Exec(db.p,"ROLLBACK;");
        Exec(db.p,"BEGIN IMMEDIATE;");
        Check(account.StageReplace(0,State(3))==1);Check(account.Read()->state.Bytes()[0]==3);Check(store.Read()->state.Bytes()[0]==1);
        WalletStateBytes empty(std::span<const uint8_t>{});Check(account.StageReplace(1,empty)==2);Check(account.Read()->state.Bytes().empty());
        Exec(db.p,"ROLLBACK;");Check(!account.Read());
        std::vector<uint8_t> excessive(WalletSnapshotStore::kMaxStateBytes+1);Reject([&]{WalletStateBytes tooLarge(excessive);});
        Exec(db.p,"UPDATE orchard_wallet_schema SET version=2;");Reject([&]{(void)store.Read();});Exec(db.p,"UPDATE orchard_wallet_schema SET version=1;");
        Exec(db.p,"PRAGMA synchronous=OFF;BEGIN IMMEDIATE;");Reject([&]{(void)store.StageReplace(1,State(2));});Exec(db.p,"ROLLBACK;PRAGMA synchronous=FULL;");
    }
    Verify(path.c_str(),1,1);const auto exe=std::filesystem::absolute(argv[0]).string();
    Crash(exe,path,false);Verify(path.c_str(),1,1);
    {DB db(path.c_str());WalletSnapshotStore store(db.p,Identity(),seed);Reject([&]{(void)store.ReadRetained(1);});}
    Crash(exe,path,true);Verify(path.c_str(),2,2);
    {DB db(path.c_str());WalletSnapshotStore store(db.p,Identity(),seed);
     Check(store.ReadRetained(1).state.Bytes()[0]==1);
     Reject([&]{(void)store.ReadRetained(0);});Reject([&]{(void)store.ReadRetained(2);});
     Exec(db.p,"BEGIN IMMEDIATE;UPDATE orchard_wallet_retained SET revision=9;");Reject([&]{(void)store.ReadRetained(1);});Exec(db.p,"ROLLBACK;");
     Exec(db.p,"BEGIN IMMEDIATE;UPDATE orchard_wallet_retained SET sealed=zeroblob(2077);");Reject([&]{(void)store.ReadRetained(1);});Exec(db.p,"ROLLBACK;");
     Exec(db.p,"CREATE TRIGGER reject_retention BEFORE INSERT ON orchard_wallet_retained BEGIN SELECT RAISE(ABORT,'retention failure');END;BEGIN IMMEDIATE;");
     Reject([&]{(void)store.StageReplaceRetaining(2,State(3));});Exec(db.p,"ROLLBACK;DROP TRIGGER reject_retention;");Check(store.Read()->revision==2);
     Exec(db.p,"CREATE TRIGGER reject_latest BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'latest failure');END;BEGIN IMMEDIATE;");
     Reject([&]{(void)store.StageReplaceRetaining(2,State(3));});Exec(db.p,"COMMIT;DROP TRIGGER reject_latest;");Reject([&]{(void)store.ReadRetained(2);});
     {sqlite3_stmt* stmt=nullptr;Check(sqlite3_prepare_v2(db.p,"SELECT count(*) FROM orchard_wallet_retained WHERE revision=2",-1,&stmt,nullptr)==SQLITE_OK);Check(sqlite3_step(stmt)==SQLITE_ROW);Check(sqlite3_column_int(stmt,0)==0);sqlite3_finalize(stmt);}
     Exec(db.p,"BEGIN IMMEDIATE;");Check(store.StageReplaceRetaining(2,State(3))==3);Exec(db.p,"COMMIT;");
     Check(store.ReadRetained(1).state.Bytes()[0]==1);Check(store.ReadRetained(2).state.Bytes()[0]==2);
     WalletSnapshotStore wrong(db.p,Identity(),std::array<uint8_t,64>{8});Reject([&]{(void)wrong.ReadRetained(1);});
     Exec(db.p,"BEGIN IMMEDIATE;DELETE FROM orchard_wallet_retained WHERE revision=1;");Reject([&]{(void)store.ReadRetained(1);});Exec(db.p,"ROLLBACK;");
     Exec(db.p,"BEGIN IMMEDIATE;");Check(store.StageReplaceRetaining(3,State(2))==4);Exec(db.p,"COMMIT;");
    }
    {DB db(path.c_str());WalletSnapshotStore store(db.p,Identity(),seed);Exec(db.p,"BEGIN IMMEDIATE;");Check(store.StageReplaceRetaining(4,State(1))==5);Exec(db.p,"UPDATE companion SET value=1;COMMIT;");}
    Verify(path.c_str(),5,1); // rollback is a NEW revision, never a clock rewind.
    std::cout<<"Encrypted wallet snapshot: binding, tamper/key/revision checks, shared SQLite rollback, fresh-process pre/post-commit recovery passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

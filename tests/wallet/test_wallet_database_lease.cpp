#include "wallet/wallet_manager.h"
#include "wallet/shielded_wallet_ops.h"
#include "wallet/wallet_worker.h"
#include "wallet/utxo_index.h"
#include "wallet/taproot_keys.h"
#include "wallet/transaction_builder.h"
#include "wallet/transaction_signer.h"
#include "wallet/wallet_key_provider.h"
#include "wallet/v7_p2mr_store.h"
#include "rpc/v7_pq_handlers.h"
#include "consensus/pq/p2mr_consensus.h"
#include "consensus/script_interpreter.h"
#include "util/hex.h"
#include "consensus/chainparams.h"
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>

namespace dinero {
struct WalletWorkerTestAccess {
    // Exercise the real enqueue and dispatch code with a deterministic pause
    // between them, without racing a background thread against wallet setup.
    static void EnableQueue(WalletWorker& worker) { worker.running_.store(true); }
    static WalletJob Take(WalletWorker& worker) { return worker.job_queue_.pop(); }
    static void Dispatch(WalletWorker& worker, const WalletJob& job) { worker.ProcessJob(job); }
    static void Connect(WalletWorker& worker, uint32_t height,
                        const std::vector<Transaction>& transactions) {
        worker.ProcessConnect(height, std::string(64, '1'), transactions);
    }
};
}
namespace {
using namespace std::chrono_literals;

class WalletDatabaseLeaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        path = std::filesystem::temp_directory_path() /
            ("dinero_wallet_lease_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        wallet = std::make_unique<dinero::WalletManager>(path);
        wallet->create("owner");
    }
    void TearDown() override {
        wallet.reset();
        std::filesystem::remove_all(path);
    }
    static void Exec(sqlite3* db, const char* sql) {
        char* error = nullptr;
        const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
        const std::string message = error ? error : "";
        sqlite3_free(error);
        if (rc != SQLITE_OK) throw std::runtime_error(message);
    }
    static int Count(sqlite3* db) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT count(*) FROM lease_probe", -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("prepare probe");
        const int rc = sqlite3_step(stmt);
        const int count = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
        sqlite3_finalize(stmt);
        return count;
    }
    std::filesystem::path path;
    std::unique_ptr<dinero::WalletManager> wallet;
};

class WalletAddressIssuanceTest : public WalletDatabaseLeaseTest {
protected:
    static int64_t Scalar(sqlite3* db,const std::string& sql) {
        sqlite3_stmt* raw=nullptr;
        if(sqlite3_prepare_v2(db,sql.c_str(),-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("issuance query");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> q(raw,sqlite3_finalize);
        if(sqlite3_step(q.get())!=SQLITE_ROW)throw std::runtime_error("issuance row");
        const auto value=sqlite3_column_int64(q.get(),0);
        if(sqlite3_step(q.get())!=SQLITE_DONE)throw std::runtime_error("issuance EOF");
        return value;
    }
    std::vector<int64_t> Inventory() {
        auto* db=wallet->getCurrentDatabase();
        return {Scalar(db,"SELECT COUNT(*) FROM addresses"),Scalar(db,"SELECT COUNT(*) FROM address_derivation_paths"),
                Scalar(db,"SELECT COUNT(*) FROM watch_scripts")};
    }
    std::string Issue(int change) { return change?wallet->getNewChangeAddress("issued change"):wallet->getNewAddress("issued receive"); }
};
TEST_F(WalletAddressIssuanceTest, RequiredRowsRollbackAndReopen) {
    wallet->open("owner");
    for(int change:{0,1})for(const auto* table:{"address_derivation_paths","watch_scripts","addresses"}) {
        SCOPED_TRACE(std::string(table)+" chain="+std::to_string(change));
        const auto before=Inventory();const auto next=wallet->getNextAddressIndex(0,change);
        const std::string fail="CREATE TRIGGER fail_issuance BEFORE INSERT ON "+std::string(table)+" BEGIN SELECT RAISE(ABORT,'issuance failure'); END";
        Exec(wallet->getCurrentDatabase(),fail.c_str());
        ASSERT_TRUE(Issue(change).empty());
        EXPECT_EQ(Inventory(),before);EXPECT_EQ(wallet->getNextAddressIndex(0,change),next);
        EXPECT_EQ(sqlite3_get_autocommit(wallet->getCurrentDatabase()),1);
        Exec(wallet->getCurrentDatabase(),"DROP TRIGGER fail_issuance");
        wallet->open("owner");
        EXPECT_EQ(Inventory(),before);EXPECT_EQ(wallet->getNextAddressIndex(0,change),next);
        ASSERT_FALSE(Issue(change).empty());
        EXPECT_EQ(wallet->getNextAddressIndex(0,change),next+1);
    }
}
TEST_F(WalletAddressIssuanceTest, CommitFailureAndBorrowedTransactionRefuse) {
    wallet->open("owner");
    auto* db=wallet->getCurrentDatabase();
    Exec(db,"PRAGMA foreign_keys=ON; CREATE TABLE issuance_parent(id INTEGER PRIMARY KEY); CREATE TABLE issuance_child(id INTEGER REFERENCES issuance_parent(id) DEFERRABLE INITIALLY DEFERRED)");
    for(int change:{0,1}) {
        const auto before=Inventory();const auto next=wallet->getNextAddressIndex(0,change);
        Exec(db,"CREATE TRIGGER deferred_issuance AFTER INSERT ON watch_scripts BEGIN INSERT INTO issuance_child VALUES(99); END");
        EXPECT_TRUE(Issue(change).empty());EXPECT_EQ(Inventory(),before);EXPECT_EQ(wallet->getNextAddressIndex(0,change),next);
        EXPECT_EQ(Scalar(db,"SELECT COUNT(*) FROM issuance_child"),0);EXPECT_EQ(sqlite3_get_autocommit(db),1);
        Exec(db,"DROP TRIGGER deferred_issuance; BEGIN IMMEDIATE; INSERT INTO issuance_parent VALUES(7)");
        EXPECT_TRUE(Issue(change).empty());EXPECT_EQ(Inventory(),before);EXPECT_EQ(sqlite3_get_autocommit(db),0);
        EXPECT_EQ(Scalar(db,"SELECT COUNT(*) FROM issuance_parent"),1);
        Exec(db,"ROLLBACK");
        ASSERT_FALSE(Issue(change).empty());
    }
}
TEST_F(WalletAddressIssuanceTest, CompleteTupleAndIndexSurviveReopen) {
    wallet->open("owner");
    dinero::UTXOIndex index((path/"issuance-index.sqlite").string());ASSERT_TRUE(index.Initialize());
    wallet->setUTXOIndex(&index);
    struct ResetIndex { dinero::WalletManager& wallet; ~ResetIndex(){wallet.setUTXOIndex(nullptr);} } reset{*wallet};
    for(int change:{0,1}) {
        const auto next=wallet->getNextAddressIndex(0,change);
        const auto address=Issue(change);ASSERT_FALSE(address.empty());
        const auto hex=wallet->getScriptPubKeyForAddress(address);ASSERT_TRUE(hex.has_value());
        std::vector<uint8_t> script;for(size_t n=0;n<hex->size();n+=2)script.push_back(std::stoul(hex->substr(n,2),nullptr,16));
        ASSERT_TRUE(index.IsOurScript(script).has_value());
        const auto sql="SELECT COUNT(*) FROM addresses a JOIN address_derivation_paths p ON p.address=a.address JOIN watch_scripts w ON lower(hex(w.script_pubkey))=a.script_pubkey WHERE a.address='"+address+"' AND p.script_pubkey=a.script_pubkey AND p.derivation_path=w.path AND p.address_index=a.idx AND p.change=a.change AND w.is_change=a.change AND a.change="+std::to_string(change)+" AND a.idx="+std::to_string(next);
        EXPECT_EQ(Scalar(wallet->getCurrentDatabase(),sql),1);
        wallet->open("owner");EXPECT_EQ(Scalar(wallet->getCurrentDatabase(),sql),1);
        index.ClearRegisteredAddresses();wallet->LoadAddressesIntoUTXOIndex();ASSERT_TRUE(index.IsOurScript(script).has_value());
    }
    wallet->setUTXOIndex(nullptr);
}
TEST_F(WalletAddressIssuanceTest, WatchConflictRefusesAndPublicationFollowsCommit) {
    wallet->open("owner");auto* db=wallet->getCurrentDatabase();
    const auto original=Issue(0);ASSERT_FALSE(original.empty());
    const auto hex=wallet->getScriptPubKeyForAddress(original);ASSERT_TRUE(hex.has_value());
    std::vector<uint8_t> script;for(size_t n=0;n<hex->size();n+=2)script.push_back(std::stoul(hex->substr(n,2),nullptr,16));
    dinero::UTXOIndex index((path/"watch-index.sqlite").string());ASSERT_TRUE(index.Initialize());
    wallet->setUTXOIndex(&index);
    struct ResetIndex { dinero::WalletManager& wallet; ~ResetIndex(){wallet.setUTXOIndex(nullptr);} } reset{*wallet};
    // Explicit partial-record fixture. This is not automatic recovery policy.
    Exec(db,"CREATE TEMP TABLE original_watch AS SELECT path FROM watch_scripts; DELETE FROM addresses; DELETE FROM address_derivation_paths; UPDATE watch_scripts SET path='conflicting imported path',last_seen_height=91");
    EXPECT_TRUE(Issue(0).empty());EXPECT_FALSE(index.IsOurScript(script));
    EXPECT_EQ(Scalar(db,"SELECT COUNT(*) FROM addresses"),0);EXPECT_EQ(Scalar(db,"SELECT COUNT(*) FROM address_derivation_paths"),0);
    EXPECT_EQ(Scalar(db,"SELECT COUNT(*) FROM watch_scripts WHERE path='conflicting imported path' AND last_seen_height=91"),1);
    Exec(db,"UPDATE watch_scripts SET path=(SELECT path FROM original_watch)");
    struct Commit { dinero::UTXOIndex& index; const std::vector<uint8_t>& script; bool called=false,unpublished=true; } commit{index,script};
    sqlite3_commit_hook(db,[](void* p){auto& c=*static_cast<Commit*>(p);c.called=true;c.unpublished&=!c.index.IsOurScript(c.script).has_value();return 0;},&commit);
    const auto address=Issue(0);sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(address,original);EXPECT_TRUE(commit.called && commit.unpublished);EXPECT_TRUE(index.IsOurScript(script));
    EXPECT_EQ(Scalar(db,"SELECT last_seen_height FROM watch_scripts"),91);
    EXPECT_EQ(Scalar(db,"PRAGMA synchronous"),2);
}
TEST_F(WalletAddressIssuanceTest, IndexReadFailureAndExhaustionRefuse) {
    wallet->open("owner");auto* db=wallet->getCurrentDatabase();const auto before=Inventory();
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char* col,const char*,const char*) {
        return action==SQLITE_READ && table && col && std::string_view(table)=="addresses" && std::string_view(col)=="idx"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_TRUE(Issue(0).empty());EXPECT_TRUE(Issue(1).empty());
    sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_EQ(Inventory(),before);
    ASSERT_FALSE(Issue(0).empty());
    Exec(db,"UPDATE addresses SET idx=2147483647 WHERE account=0 AND change=0 AND idx=(SELECT MAX(idx) FROM addresses WHERE account=0 AND change=0)");
    EXPECT_THROW(wallet->getNextAddressIndex(0,0),std::runtime_error);
    EXPECT_TRUE(Issue(0).empty());
}

class WalletTaprootImportTest : public WalletAddressIssuanceTest {
protected:
    struct Import {
        std::array<uint8_t,32> secret{},internal{},output{};
        std::vector<uint8_t> script;std::string address;
        explicit Import(uint8_t n) {
            secret.back()=n;int parity=0;
            if(!dinero::TaprootKeys::DeriveXOnlyPubkey(secret,internal,parity) ||
               !dinero::TaprootKeys::ComputeTweakedPubkey(internal,output))throw std::runtime_error("test key");
            const auto& network=dinero::Params().name;
            address=dinero::TaprootKeys::CreateTaprootAddress(output,network=="regtest"?"rdin":network=="testnet"?"tdin":"din");
            script={0x51,0x20};script.insert(script.end(),output.begin(),output.end());
        }
    };
    bool Store(const Import& key) {return wallet->storeTaprootKey(key.address,key.secret,key.internal,key.output,"import label");}
    std::vector<int64_t> Rows() {
        auto r=Inventory();auto* db=wallet->getCurrentDatabase();
        r.push_back(Scalar(db,"SELECT count(*) FROM taproot_keys"));
        r.push_back(Scalar(db,"SELECT count(*) FROM taproot_key_mapping"));return r;
    }
};
TEST_F(WalletTaprootImportTest, CompleteImportReopensAndReimports) {
    wallet->open("owner");dinero::UTXOIndex index((path/"import-index.sqlite").string());ASSERT_TRUE(index.Initialize());wallet->setUTXOIndex(&index);
    struct ResetIndex { dinero::WalletManager& wallet; ~ResetIndex(){wallet.setUTXOIndex(nullptr);} } reset{*wallet};
    Import first(11),second(12);ASSERT_TRUE(Store(first));ASSERT_TRUE(Store(second));
    auto* db=wallet->getCurrentDatabase();
    EXPECT_EQ(Scalar(db,"SELECT count(*) FROM addresses WHERE account=-1 AND type='p2tr' AND length(script_pubkey)=68 AND label='import label'"),2);
    EXPECT_EQ(Scalar(db,"SELECT count(*) FROM taproot_keys WHERE length(internal_privkey)=32 AND is_privkey_encrypted=0"),2);
    ASSERT_TRUE(index.IsOurScript(first.script));ASSERT_TRUE(index.IsOurScript(second.script));
    const auto before=Rows();ASSERT_TRUE(Store(first));EXPECT_EQ(Rows(),before);
    wallet->open("owner");index.ClearRegisteredAddresses();wallet->LoadAddressesIntoUTXOIndex();
    EXPECT_TRUE(index.IsOurScript(first.script));EXPECT_TRUE(index.IsOurScript(second.script));
    EXPECT_EQ(Rows(),before);ASSERT_TRUE(Store(second));EXPECT_EQ(Rows(),before);
}
TEST_F(WalletTaprootImportTest, RequiredWritesAndCommitRollback) {
    wallet->open("owner");dinero::UTXOIndex index((path/"import-index.sqlite").string());ASSERT_TRUE(index.Initialize());wallet->setUTXOIndex(&index);
    struct ResetIndex { dinero::WalletManager& wallet; ~ResetIndex(){wallet.setUTXOIndex(nullptr);} } reset{*wallet};
    auto* db=wallet->getCurrentDatabase();const auto initial=Inventory();Import schema_key(13);
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_INSERT && table && std::string_view(table)=="taproot_keys"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    const bool refused=!Store(schema_key);sqlite3_set_authorizer(db,nullptr,nullptr);
    ASSERT_TRUE(refused);EXPECT_EQ(Inventory(),initial);EXPECT_FALSE(index.IsOurScript(schema_key.script));
    EXPECT_EQ(Scalar(db,"SELECT count(*) FROM sqlite_master WHERE name IN ('taproot_keys','taproot_key_mapping')"),0);
    ASSERT_TRUE(Store(schema_key));int n=14;
    for(const auto* table:{"watch_scripts","taproot_key_mapping","taproot_keys","addresses"}) {
        SCOPED_TRACE(table);Import key(n++);const auto before=Rows();
        Exec(db,("CREATE TRIGGER fail_import BEFORE INSERT ON "+std::string(table)+" BEGIN SELECT RAISE(ABORT,'import write failure'); END").c_str());
        ASSERT_FALSE(Store(key));EXPECT_EQ(Rows(),before);EXPECT_FALSE(index.IsOurScript(key.script));EXPECT_EQ(sqlite3_get_autocommit(db),1);
        Exec(db,"DROP TRIGGER fail_import");wallet->open("owner");db=wallet->getCurrentDatabase();
        EXPECT_EQ(Rows(),before);ASSERT_TRUE(Store(key));
    }
    Exec(db,"PRAGMA foreign_keys=ON; CREATE TABLE import_parent(id INTEGER PRIMARY KEY); CREATE TABLE import_child(id INTEGER REFERENCES import_parent(id) DEFERRABLE INITIALLY DEFERRED); CREATE TRIGGER fail_commit AFTER INSERT ON taproot_keys BEGIN INSERT INTO import_child VALUES(999); END");
    Import key(20);const auto before=Rows();ASSERT_FALSE(Store(key));EXPECT_EQ(Rows(),before);EXPECT_FALSE(index.IsOurScript(key.script));EXPECT_EQ(sqlite3_get_autocommit(db),1);
    Exec(db,"DROP TRIGGER fail_commit");ASSERT_TRUE(Store(key));
}
TEST_F(WalletTaprootImportTest, CallerTransactionAndConflictingOwnersRefuse) {
    wallet->open("owner");dinero::UTXOIndex index((path/"import-index.sqlite").string());ASSERT_TRUE(index.Initialize());wallet->setUTXOIndex(&index);
    struct ResetIndex { dinero::WalletManager& wallet; ~ResetIndex(){wallet.setUTXOIndex(nullptr);} } reset{*wallet};Import key(21);ASSERT_TRUE(Store(key));
    auto* db=wallet->getCurrentDatabase();const auto before=Rows();
    Exec(db,"CREATE TABLE import_probe(n INTEGER); BEGIN; INSERT INTO import_probe VALUES(1)");
    ASSERT_FALSE(Store(Import(22)));EXPECT_EQ(sqlite3_get_autocommit(db),0);EXPECT_EQ(Scalar(db,"SELECT count(*) FROM import_probe"),1);EXPECT_EQ(Rows(),before);Exec(db,"ROLLBACK");
    index.ClearRegisteredAddresses();Exec(db,"UPDATE watch_scripts SET path='watch-only' WHERE path LIKE 'tr(%'");
    ASSERT_FALSE(Store(key));EXPECT_EQ(Rows(),before);EXPECT_FALSE(index.IsOurScript(key.script));
    Import other(23);other.internal[0]^=1;ASSERT_FALSE(Store(other));EXPECT_EQ(Rows(),before);
    uint64_t old=0;{auto lease=wallet->AcquireDatabaseLease();old=lease->Session();}
    wallet->open("owner");Import next(26);
    ASSERT_FALSE(wallet->storeTaprootKey(next.address,next.secret,next.internal,next.output,"stale",old));EXPECT_EQ(Rows(),before);

}
TEST_F(WalletTaprootImportTest, EncryptedOwnerAndCommitPublication) {
    wallet->open("owner");wallet->encryptWallet("import-test-passphrase");
    Import key(24);ASSERT_FALSE(Store(key));wallet->unlockWallet("import-test-passphrase");
    dinero::UTXOIndex index((path/"import-index.sqlite").string());ASSERT_TRUE(index.Initialize());wallet->setUTXOIndex(&index);
    struct ResetIndex { dinero::WalletManager& wallet; ~ResetIndex(){wallet.setUTXOIndex(nullptr);} } reset{*wallet};
    struct Hook {dinero::UTXOIndex* index;const Import* key;bool called=false;bool published=false;} hook{&index,&key};
    auto* db=wallet->getCurrentDatabase();
    sqlite3_commit_hook(db,[](void* opaque){auto& h=*static_cast<Hook*>(opaque);h.called=true;h.published=bool(h.index->IsOurScript(h.key->script));return 0;},&hook);
    ASSERT_TRUE(Store(key));sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_TRUE(hook.called);EXPECT_FALSE(hook.published);EXPECT_TRUE(index.IsOurScript(key.script));
    EXPECT_EQ(Scalar(db,"SELECT count(*) FROM taproot_keys WHERE is_privkey_encrypted=1 AND length(internal_privkey)>32"),1);
    wallet->lockWallet();const auto before=Rows();ASSERT_FALSE(Store(Import(25)));EXPECT_EQ(Rows(),before);
    wallet->open("owner");ASSERT_FALSE(Store(Import(25)));wallet->unlockWallet("import-test-passphrase");ASSERT_TRUE(Store(Import(25)));
}


class WalletTaprootLookupTest : public WalletTaprootImportTest {
protected:
    void CheckSignature(const Import& key) {
        const auto resolved=wallet->deriveKeyForScriptPubKey(util::hex(key.script));
        ASSERT_TRUE(resolved.has_value()); ASSERT_EQ(resolved->size(),32u);
        EXPECT_EQ(*resolved,std::vector<uint8_t>(key.secret.begin(),key.secret.end()));
        std::array<uint8_t,32> secret{},message{}; message.back()=77;
        std::copy(resolved->begin(),resolved->end(),secret.begin());
        std::array<uint8_t,64> signature{};
        ASSERT_TRUE(dinero::TaprootKeys::SignSchnorrWithInternalKey(signature,message,secret,key.internal));
        EXPECT_TRUE(dinero::TaprootKeys::VerifySchnorr(signature,message,key.output));
    }
};
TEST_F(WalletTaprootLookupTest, PlaintextAndEncryptedReopenSigning) {
    wallet->open("owner");
    const auto hd=wallet->getNewAddress("HD still resolves"); const auto hd_script=wallet->getScriptPubKeyForAddress(hd);
    ASSERT_TRUE(hd_script); ASSERT_TRUE(wallet->deriveKeyForScriptPubKey(*hd_script));
    Import plain(31); ASSERT_TRUE(Store(plain));
    CheckSignature(plain); wallet->open("owner"); CheckSignature(plain);
    // Encryption of prior imports is a separate migration obligation. This case
    // exercises keys imported after the existing encrypted owner is established.
    wallet->create("encrypted"); wallet->open("encrypted");
    wallet->encryptWallet("lookup-passphrase"); wallet->unlockWallet("lookup-passphrase");
    Import encrypted(32); ASSERT_TRUE(Store(encrypted)); CheckSignature(encrypted);
    wallet->lockWallet(); EXPECT_FALSE(wallet->deriveKeyForScriptPubKey(util::hex(encrypted.script)));
    wallet->open("encrypted"); EXPECT_FALSE(wallet->deriveKeyForScriptPubKey(util::hex(encrypted.script)));
    wallet->unlockWallet("lookup-passphrase"); CheckSignature(encrypted);
    auto* db=wallet->getCurrentDatabase(); Exec(db,"CREATE TEMP TABLE saved_cipher AS SELECT internal_privkey FROM taproot_keys; UPDATE taproot_keys SET internal_privkey=zeroblob(60)");
    EXPECT_FALSE(wallet->deriveKeyForScriptPubKey(util::hex(encrypted.script)));
    Exec(db,"UPDATE taproot_keys SET internal_privkey=(SELECT internal_privkey FROM saved_cipher)"); CheckSignature(encrypted);
    EXPECT_FALSE(wallet->deriveKeyForScriptPubKey(util::hex(plain.script)));
}
TEST_F(WalletTaprootLookupTest, DurableBindingsAndReadFailuresRefuse) {
    wallet->open("owner"); Import key(33); ASSERT_TRUE(Store(key)); CheckSignature(key);
    const std::string script=util::hex(key.script); auto* db=wallet->getCurrentDatabase();
    Exec(db,"CREATE TEMP TABLE saved_keys AS SELECT * FROM taproot_keys; CREATE TEMP TABLE saved_mapping AS SELECT * FROM taproot_key_mapping; CREATE TEMP TABLE saved_watch AS SELECT * FROM watch_scripts; CREATE TEMP TABLE saved_addresses AS SELECT * FROM addresses");
    for(const auto* sql:{"UPDATE taproot_keys SET internal_privkey=zeroblob(32)",
                         "UPDATE taproot_keys SET internal_pubkey=zeroblob(32)",
                         "UPDATE taproot_keys SET internal_privkey=CAST(internal_privkey AS TEXT)",
                         "UPDATE taproot_keys SET internal_pubkey=CAST(internal_pubkey AS TEXT)",
                         "UPDATE taproot_keys SET is_privkey_encrypted=1",
                         "UPDATE taproot_key_mapping SET internal_pubkey=zeroblob(32)",
                         "UPDATE watch_scripts SET path='m/86\''/1448\''/0\''/0/0' WHERE path LIKE 'tr(%'",
                         "UPDATE addresses SET account=0 WHERE account=-1",
                         "DELETE FROM taproot_keys", "DELETE FROM taproot_key_mapping",
                         "DELETE FROM watch_scripts WHERE path LIKE 'tr(%'", "DELETE FROM addresses WHERE account=-1"}) {
        SCOPED_TRACE(sql); Exec(db,sql);
        EXPECT_FALSE(wallet->deriveKeyForScriptPubKey(script));
        EXPECT_EQ(sqlite3_get_autocommit(db),1);
        Exec(db,"BEGIN; DELETE FROM taproot_keys; INSERT INTO taproot_keys SELECT * FROM saved_keys; DELETE FROM taproot_key_mapping; INSERT INTO taproot_key_mapping SELECT * FROM saved_mapping; DELETE FROM watch_scripts; INSERT INTO watch_scripts SELECT * FROM saved_watch; DELETE FROM addresses; INSERT INTO addresses SELECT * FROM saved_addresses; COMMIT");
        CheckSignature(key);
    }
    Exec(db,"BEGIN IMMEDIATE; UPDATE taproot_keys SET label='uncommitted caller label'");
    EXPECT_FALSE(wallet->deriveKeyForScriptPubKey(script)); EXPECT_EQ(sqlite3_get_autocommit(db),0);
    EXPECT_EQ(Scalar(db,"SELECT count(*) FROM taproot_keys WHERE label='uncommitted caller label'"),1);
    Exec(db,"ROLLBACK"); CheckSignature(key);
    // Commit conflicting public ownership: no cached key may bypass it.
    Exec(db,"UPDATE taproot_key_mapping SET internal_pubkey=zeroblob(32)");
    EXPECT_FALSE(wallet->deriveKeyForScriptPubKey(script));
    const auto restore="UPDATE taproot_key_mapping SET internal_pubkey=x'"+util::hex(std::vector<uint8_t>(key.internal.begin(),key.internal.end()))+"'";
    Exec(db,restore.c_str()); CheckSignature(key);
    sqlite3_set_authorizer(db,[](void*,int op,const char* table,const char*,const char*,const char*) {
        return op==SQLITE_READ && table && std::string(table)=="taproot_keys"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_FALSE(wallet->deriveKeyForScriptPubKey(script)); sqlite3_set_authorizer(db,nullptr,nullptr);
    CheckSignature(key);
}


class WalletImportedTransactionTest : public WalletTaprootImportTest {
protected:
    dinero::CanonicalWalletUTXO Coin(const Import& key,uint32_t n) {
        dinero::CanonicalWalletUTXO coin;coin.txid=dinero::uint256::FromHexUnsafe(std::string(63,'0')+"1");
        coin.vout=n;coin.value=dinero::AmountUna::Una(100000);coin.spk=key.script;coin.height=1;
        const auto label=wallet->getWatchScriptPath(key.script);if(!label)throw std::runtime_error("watch owner absent");coin.path=*label;return coin;
    }
    dinero::Transaction TransactionFor(const dinero::CanonicalWalletUTXO& coin) {
        dinero::Transaction tx;tx.version=2;dinero::TxInput in;in.prevout=dinero::TxOutPoint(dinero::TxId(coin.txid),coin.vout);tx.vin.push_back(in);
        dinero::TxOutput out;out.value=dinero::AmountUna::Una(90000);out.scriptPubKey=coin.spk;tx.vout.push_back(out);return tx;
    }
    void Verify(const dinero::Transaction& tx,size_t i,const std::vector<dinero::CanonicalWalletUTXO>& coins,bool v1) {
        ASSERT_EQ(tx.vin[i].witness.size(),1u);ASSERT_EQ(tx.vin[i].witness[0].size(),64u);
        const auto hash=v1?dinero::TaprootTxSigner::ComputeTaprootSighashV1(tx,i,coins,dinero::DEFAULT_EXT_COMMITMENT):dinero::TaprootTxSigner::ComputeTaprootSighash(tx,i,coins);
        ASSERT_EQ(hash.size(),32u);std::array<uint8_t,32> message{},output{};std::array<uint8_t,64> sig{};
        std::copy(hash.begin(),hash.end(),message.begin());std::copy(coins[i].spk.begin()+2,coins[i].spk.end(),output.begin());std::copy(tx.vin[i].witness[0].begin(),tx.vin[i].witness[0].end(),sig.begin());
        EXPECT_TRUE(dinero::TaprootKeys::VerifySchnorr(sig,message,output));
        auto changed=coins;changed[i].value=dinero::AmountUna::Una(99999);
        const auto altered=v1?dinero::TaprootTxSigner::ComputeTaprootSighashV1(tx,i,changed,dinero::DEFAULT_EXT_COMMITMENT):dinero::TaprootTxSigner::ComputeTaprootSighash(tx,i,changed);
        std::copy(altered.begin(),altered.end(),message.begin());EXPECT_FALSE(dinero::TaprootKeys::VerifySchnorr(sig,message,output));
    }
};
TEST_F(WalletImportedTransactionTest, ReopenedEncryptedImportSignsBothEpochs) {
    wallet->open("owner");wallet->encryptWallet("transaction-test");wallet->unlockWallet("transaction-test");Import key(41);ASSERT_TRUE(Store(key));
    wallet->open("owner");wallet->unlockWallet("transaction-test");const auto coin=Coin(key,0);
    const auto secret=wallet->deriveKeyForScriptPubKey(util::hex(key.script));ASSERT_TRUE(secret);
    for(bool v1:{false,true}){auto tx=TransactionFor(coin);const bool ok=v1?dinero::TaprootTxSigner::SignInputV1(tx,0,{coin},*secret,dinero::DEFAULT_EXT_COMMITMENT):dinero::TaprootTxSigner::SignInput(tx,0,{coin},*secret);ASSERT_TRUE(ok);Verify(tx,0,{coin},v1);}
}
TEST_F(WalletImportedTransactionTest, WrongKeyAndOriginRefuseBeforeWitness) {
    wallet->open("owner");Import key(42),other(43);ASSERT_TRUE(Store(key));const auto coin=Coin(key,0);
    const auto secret=wallet->deriveKeyForScriptPubKey(util::hex(key.script));ASSERT_TRUE(secret);
    for(bool v1:{false,true})for(int variant:{0,1,2,3}) {
        auto changed=coin;auto bytes=*secret;if(variant==0)changed.path="";if(variant==1)changed.path="tr(00000000...)";if(variant>=2)bytes.assign(other.secret.begin(),other.secret.end());if(variant==3)changed.path="tr("+util::hex(std::vector<uint8_t>(other.internal.begin(),other.internal.end())).substr(0,8)+"...)";
        auto tx=TransactionFor(coin);const bool ok=v1?dinero::TaprootTxSigner::SignInputV1(tx,0,{changed},bytes,dinero::DEFAULT_EXT_COMMITMENT):dinero::TaprootTxSigner::SignInput(tx,0,{changed},bytes);
        EXPECT_FALSE(ok);EXPECT_TRUE(tx.vin[0].witness.empty());
    }
}
TEST_F(WalletImportedTransactionTest, BuilderRequiresExactOutpointKeys) {
    wallet->open("owner");Import first(44),second(45);ASSERT_TRUE(Store(first));ASSERT_TRUE(Store(second));wallet->open("owner");
    const auto a=Coin(first,0),b=Coin(second,1);auto ak=wallet->deriveKeyForScriptPubKey(util::hex(first.script)),bk=wallet->deriveKeyForScriptPubKey(util::hex(second.script));ASSERT_TRUE(ak && bk);
    dinero::UTXOIndex index((path/"builder.sqlite").string());ASSERT_TRUE(index.Initialize());dinero::TransactionBuilder builder(&index);
    dinero::TransactionBuilder::BuildOptions options;options.candidate_utxos={a,b};options.change_address=first.address;
    const std::vector<dinero::TransactionBuilder::Recipient> recipients{{second.address,150000}};
    std::map<std::string,std::string> keys{{a.GetOutpointString(),util::hex(*ak)},{b.GetOutpointString(),util::hex(*bk)}, {a.path,util::hex(*bk)},{b.path,util::hex(*ak)}};
    auto built=builder.BuildTransaction(recipients,keys,options);ASSERT_TRUE(built.success)<<built.error;ASSERT_EQ(built.selected_utxos.size(),2u);
    for(size_t i=0;i<2;++i)Verify(built.transaction,i,built.selected_utxos,false);
    keys.erase(a.GetOutpointString());keys.erase(b.GetOutpointString());EXPECT_FALSE(builder.BuildTransaction(recipients,keys,options).success);
    options.candidate_utxos={a};EXPECT_FALSE(builder.BuildTransaction({{second.address,50000}},{{"unrelated",util::hex(*ak)}},options).success);
}


class WalletInputProviderTest : public WalletImportedTransactionTest {
protected:
    dinero::UnsignedTransaction Unsigned(const std::vector<dinero::CanonicalWalletUTXO>& coins) {
        dinero::UnsignedTransaction out;out.tx=TransactionFor(coins[0]);out.tx.vin.clear();out.selected_utxos=coins;
        for(const auto& coin:coins){dinero::TxInput in;in.prevout=dinero::TxOutPoint(dinero::TxId(coin.txid),coin.vout);out.tx.vin.push_back(in);}
        out.fee=10000;out.tx.vout[0].value=dinero::AmountUna::Una(coins.size()*100000-out.fee);return out;
    }
};
TEST_F(WalletInputProviderTest, MapUsesExactInputsAndPreservesHdFallback) {
    wallet->open("owner");wallet->encryptWallet("input-provider");wallet->unlockWallet("input-provider");Import a(51),b(52);ASSERT_TRUE(Store(a));ASSERT_TRUE(Store(b));
    wallet->open("owner");wallet->unlockWallet("input-provider");const auto ca=Coin(a,0),cb=Coin(b,1);
    auto ak=wallet->deriveKeyForScriptPubKey(util::hex(a.script)),bk=wallet->deriveKeyForScriptPubKey(util::hex(b.script));ASSERT_TRUE(ak && bk);
    const auto input=Unsigned({ca,cb});const dinero::MapKeyProvider provider({{ca.GetOutpointString(),util::hex(*ak)},{cb.GetOutpointString(),util::hex(*bk)},{ca.path,util::hex(*bk)},{cb.path,util::hex(*ak)}});
    const auto signed_tx=dinero::TransactionSigner::Sign(input,provider);ASSERT_TRUE(signed_tx.success)<<signed_tx.error;
    EXPECT_EQ(signed_tx.signed_tx.tx.GetTxid(),input.tx.GetTxid());EXPECT_EQ(signed_tx.signed_tx.fee,input.fee);
    for(size_t i=0;i<2;++i){Verify(signed_tx.signed_tx.tx,i,input.selected_utxos,false);EXPECT_TRUE(input.tx.vin[i].witness.empty());}
    auto mismatched=input;mismatched.tx.vin[0].prevout.vout=99;
    EXPECT_FALSE(dinero::TransactionSigner::Sign(mismatched,provider).success);
    const auto address=wallet->getNewAddress();ASSERT_FALSE(address.empty());const auto spk=wallet->getScriptPubKeyForAddress(address);ASSERT_TRUE(spk);auto secret=wallet->deriveKeyForScriptPubKey(*spk);ASSERT_TRUE(secret);
    auto hd=ca;ASSERT_TRUE(util::unhex(*spk,hd.spk));const auto path=wallet->getWatchScriptPath(hd.spk);ASSERT_TRUE(path);hd.path=*path;
    const dinero::MapKeyProvider legacy({{hd.path,util::hex(*secret)}});const auto hs=dinero::TransactionSigner::Sign(Unsigned({hd}),legacy);ASSERT_TRUE(hs.success)<<hs.error;Verify(hs.signed_tx.tx,0,{hd},false);
}
TEST_F(WalletInputProviderTest, ImportedLabelsCannotReplaceInputBinding) {
    wallet->open("owner");Import a(53),b(54);ASSERT_TRUE(Store(a));const auto coin=Coin(a,0);auto secret=wallet->deriveKeyForScriptPubKey(util::hex(a.script));ASSERT_TRUE(secret);const auto input=Unsigned({coin});
    for(bool hybrid:{false,true})for(int variant:{0,1,2}) {
        std::map<std::string,std::string> keys{{coin.path,util::hex(*secret)}};
        if(variant==1)keys[coin.GetOutpointString()]=util::hex(std::vector<uint8_t>(b.secret.begin(),b.secret.end()));
        if(variant==2)keys[coin.GetTxIdHex()+":99"]=util::hex(*secret);
        std::unique_ptr<dinero::KeyProvider> provider;
        if(hybrid){dinero::wallet::WalletKeyProvider::Config cfg;cfg.legacy_keys_by_path=keys;provider=std::make_unique<dinero::wallet::WalletKeyProvider>(std::move(cfg));}
        else provider=std::make_unique<dinero::MapKeyProvider>(keys);
        EXPECT_FALSE(dinero::TransactionSigner::Sign(input,*provider).success)<<hybrid<<":"<<variant;
        EXPECT_TRUE(input.tx.vin[0].witness.empty());
    }
}
TEST_F(WalletInputProviderTest, HybridPreservesRealP2mrSigning) {
    wallet->open("owner");Import a(55);ASSERT_TRUE(Store(a));const auto coin=Coin(a,0);auto secret=wallet->deriveKeyForScriptPubKey(util::hex(a.script));ASSERT_TRUE(secret);
    dinero::wallet::V7P2MRStore store;ASSERT_EQ(store.Open((path/"provider-pq.sqlite").string()),dinero::wallet::V7P2MRStore::OpenResult::Ok);
    dinero::rpc::v7::GetNewP2MRAddressParams params;params.wallet_id=1;params.hrp="rdin";params.bip32_priv.fill(7);params.bip32_chain.fill(8);params.master_key.fill(9);
    const auto created=dinero::rpc::v7::GetNewP2MRAddress(store,params);ASSERT_EQ(created.status,dinero::rpc::v7::HandlerStatus::Ok)<<created.error_message;
    auto pq=coin;pq.vout=1;pq.path=created.derivation_path;pq.spk={0x53,0x20};pq.spk.insert(pq.spk.end(),created.merkle_root.begin(),created.merkle_root.end());const auto input=Unsigned({coin,pq});
    dinero::wallet::WalletKeyProvider::Config cfg;cfg.legacy_keys_by_path={{coin.GetOutpointString(),util::hex(*secret)}};cfg.p2mr_store=&store;cfg.wallet_id=1;cfg.master_key=params.master_key;
    dinero::wallet::WalletKeyProvider provider(cfg);const auto result=dinero::TransactionSigner::Sign(input,provider);ASSERT_TRUE(result.success)<<result.error;Verify(result.signed_tx.tx,0,input.selected_utxos,false);
    const auto& tx=result.signed_tx.tx;dinero::consensus::ScriptExecutionContext context(&tx,1,pq.value.GetUna(),0);
    for(const auto& c:input.selected_utxos){context.all_amounts.push_back(c.value.GetUna());context.all_scriptpubkeys.push_back(c.spk);context.all_confidential_flags.push_back(0);context.all_input_commitments.push_back(c.commitment);}
    const auto hash=dinero::consensus::SignatureHashTaproot(context,0,{});ASSERT_EQ(hash.size(),32u);std::array<uint8_t,32> message{};std::copy(hash.begin(),hash.end(),message.begin());
    ASSERT_EQ(tx.vin[1].witness.size(),1u);EXPECT_EQ(dinero::consensus::pq::VerifyP2MRSpend(pq.spk,tx.vin[1].witness[0],message,0),dinero::consensus::pq::P2MRVerifyError::Ok);
    message[0]^=1;EXPECT_NE(dinero::consensus::pq::VerifyP2MRSpend(pq.spk,tx.vin[1].witness[0],message,0),dinero::consensus::pq::P2MRVerifyError::Ok);
    cfg.master_key.fill(0);dinero::wallet::WalletKeyProvider wrong_master(cfg);EXPECT_FALSE(dinero::TransactionSigner::Sign(input,wrong_master).success);
    cfg.master_key=params.master_key;cfg.wallet_id=2;dinero::wallet::WalletKeyProvider wrong_wallet(cfg);EXPECT_FALSE(dinero::TransactionSigner::Sign(input,wrong_wallet).success);
    cfg.wallet_id=1;cfg.p2mr_store=nullptr;dinero::wallet::WalletKeyProvider missing_store(cfg);EXPECT_FALSE(dinero::TransactionSigner::Sign(input,missing_store).success);
}


class WalletEncryptionOwnerTest : public WalletTaprootLookupTest {
  protected:
    std::vector<std::string> EncryptionRows() {
        std::vector<std::string> rows;
        for (const char *sql :
             {"SELECT quote(key)||':'||quote(value) FROM settings ORDER BY key",
              "SELECT quote(encrypted_seed)||':'||quote(salt)||':'||quote(encryption_version) FROM "
              "hd_seeds ORDER BY id",
              "SELECT quote(encrypted)||':'||quote(salt)||':'||quote(updated_at) FROM "
              "encryption_metadata ORDER BY id",
              "SELECT "
              "quote(address)||':'||quote(internal_privkey)||':'||quote(is_privkey_encrypted) FROM "
              "taproot_keys ORDER BY address",
              "SELECT quote(address)||':'||hex(CAST(private_key_enc AS BLOB))||':'||quote(label) "
              "FROM imported_keys ORDER BY address"}) {
            sqlite3_stmt *q = nullptr;
            ASSERT_SQLITE(sqlite3_prepare_v2(wallet->getCurrentDatabase(), sql, -1, &q, nullptr),
                          SQLITE_OK);
            int rc;
            while ((rc = sqlite3_step(q)) == SQLITE_ROW)
                rows.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(q, 0)));
            sqlite3_finalize(q);
            ASSERT_SQLITE(rc, SQLITE_DONE);
        }
        return rows;
    }
    static void ASSERT_SQLITE(int value, int want) {
        if (value != want)
            throw std::runtime_error("encryption test SQL failed");
    }
};
TEST_F(WalletEncryptionOwnerTest, PopulatedImportsEncryptAndRotateWithoutIdentityChange) {
    wallet->open("owner");
    Import a(61), b(62);
    ASSERT_TRUE(Store(a));
    ASSERT_TRUE(Store(b));
    const auto seed = wallet->GetMasterSeed();
    ASSERT_TRUE(seed);
    const auto address = wallet->getNewAddress("preserved");
    ASSERT_FALSE(address.empty());
    wallet->encryptWallet("first-owner");
    EXPECT_TRUE(wallet->isWalletLocked());
    wallet->open("owner");
    wallet->unlockWallet("first-owner");
    CheckSignature(a);
    CheckSignature(b);
    EXPECT_EQ(wallet->GetMasterSeed(), seed);
    const auto pq = wallet->GetV7PqMasterKey();
    ASSERT_TRUE(pq);
    wallet->changePassphrase("first-owner", "second-owner");
    CheckSignature(a);
    EXPECT_EQ(wallet->GetMasterSeed(), seed);
    EXPECT_EQ(wallet->GetV7PqMasterKey(), pq);
    wallet->open("owner");
    EXPECT_THROW(wallet->unlockWallet("first-owner"), std::runtime_error);
    wallet->unlockWallet("second-owner");
    CheckSignature(a);
    CheckSignature(b);
    EXPECT_EQ(wallet->GetMasterSeed(), seed);
    EXPECT_EQ(wallet->GetV7PqMasterKey(), pq);
    EXPECT_TRUE(wallet->getScriptPubKeyForAddress(address));
}
TEST_F(WalletEncryptionOwnerTest, RequiredWritesAndBorrowedTransactionRollback) {
    wallet->open("owner");
    Import key(63);
    ASSERT_TRUE(Store(key));
    const auto seed = wallet->GetMasterSeed();
    auto *db = wallet->getCurrentDatabase();
    const auto before = EncryptionRows();
    Exec(db, "CREATE TRIGGER refuse_seed BEFORE INSERT ON hd_seeds BEGIN SELECT "
             "RAISE(ABORT,'required seed write'); END; CREATE TRIGGER refuse_seed_update BEFORE "
             "UPDATE ON hd_seeds BEGIN SELECT RAISE(ABORT,'required seed update'); END;");
    EXPECT_THROW(wallet->encryptWallet("atomic-owner"), std::runtime_error);
    EXPECT_EQ(EncryptionRows(), before);
    EXPECT_FALSE(wallet->isWalletEncrypted());
    EXPECT_EQ(wallet->GetMasterSeed(), seed);
    Exec(db, "DROP TRIGGER refuse_seed; DROP TRIGGER refuse_seed_update");
    Exec(db, "BEGIN IMMEDIATE");
    EXPECT_THROW(wallet->encryptWallet("atomic-owner"), std::runtime_error);
    EXPECT_EQ(sqlite3_get_autocommit(db), 0);
    EXPECT_EQ(EncryptionRows(), before);
    Exec(db, "ROLLBACK");
    CheckSignature(key);
    wallet->encryptWallet("atomic-owner");
    wallet->unlockWallet("atomic-owner");
    const auto encrypted = EncryptionRows();
    const auto pq = wallet->GetV7PqMasterKey();
    Exec(db, "CREATE TRIGGER refuse_import BEFORE UPDATE ON taproot_keys BEGIN SELECT "
             "RAISE(ABORT,'required import write'); END;");
    EXPECT_THROW(wallet->changePassphrase("atomic-owner", "later-owner"), std::runtime_error);
    EXPECT_EQ(EncryptionRows(), encrypted);
    EXPECT_EQ(wallet->GetMasterSeed(), seed);
    EXPECT_EQ(wallet->GetV7PqMasterKey(), pq);
    CheckSignature(key);
    Exec(db, "DROP TRIGGER refuse_import");
    wallet->open("owner");
    wallet->unlockWallet("atomic-owner");
    CheckSignature(key);
}
TEST_F(WalletEncryptionOwnerTest, ExplicitDecryptionPreservesImportsAndRefusesPqDowngrade) {
    wallet->open("owner");
    Import key(64);
    ASSERT_TRUE(Store(key));
    const auto seed = wallet->GetMasterSeed();
    wallet->encryptWallet("decrypt-owner");
    // No first unlock/PQ master exists: explicit ordinary decryption is representable.
    wallet->decryptWallet("decrypt-owner");
    EXPECT_FALSE(wallet->isWalletEncrypted());
    wallet->open("owner");
    EXPECT_EQ(wallet->GetMasterSeed(), seed);
    CheckSignature(key);
    wallet->encryptWallet("pq-owner");
    wallet->unlockWallet("pq-owner");
    ASSERT_TRUE(wallet->GetV7PqMasterKey());
    const auto before = EncryptionRows();
    EXPECT_THROW(wallet->decryptWallet("pq-owner"), std::runtime_error);
    EXPECT_EQ(EncryptionRows(), before);
    EXPECT_TRUE(wallet->isWalletEncrypted());
    CheckSignature(key);
}

TEST_F(WalletEncryptionOwnerTest, CommitAndPolicyFailuresKeepLiveAndDurableOwner) {
    wallet->open("owner");
    Import key(65);
    ASSERT_TRUE(Store(key));
    auto *db = wallet->getCurrentDatabase();
    const auto seed = wallet->GetMasterSeed();
    const auto before = EncryptionRows();
    struct Commit { dinero::WalletManager& wallet; bool called=false, seed_present=false; } commit{*wallet};
    // SQLite prohibits SQL reentry from this hook. Inspect the wallet only
    // after COMMIT has returned its error and the owner has unwound.
    sqlite3_commit_hook(db, [](void* p) { auto& c=*static_cast<Commit*>(p); c.called=true; c.seed_present=c.wallet.HaveMasterSeed(); return 1; }, &commit);
    EXPECT_THROW(wallet->encryptWallet("commit-owner"), std::runtime_error);
    sqlite3_commit_hook(db, nullptr, nullptr);
    EXPECT_TRUE(commit.called && commit.seed_present);
    EXPECT_FALSE(wallet->isWalletEncrypted());
    EXPECT_EQ(sqlite3_get_autocommit(db), 1);
    EXPECT_EQ(EncryptionRows(), before);
    EXPECT_EQ(wallet->GetMasterSeed(), seed);
    CheckSignature(key);
    for (const char *table : {"settings", "encryption_metadata"}) {
        const auto sql = std::string("CREATE TRIGGER refuse_policy BEFORE INSERT ON ") + table +
                         " BEGIN SELECT RAISE(ABORT,'required policy write'); END";
        Exec(db, sql.c_str());
        EXPECT_THROW(wallet->encryptWallet("commit-owner"), std::runtime_error);
        EXPECT_EQ(EncryptionRows(), before);
        EXPECT_FALSE(wallet->isWalletEncrypted());
        CheckSignature(key);
        Exec(db, "DROP TRIGGER refuse_policy");
    }
    wallet->encryptWallet("commit-owner");
    wallet->unlockWallet("commit-owner");
    const auto encrypted = EncryptionRows();
    const auto pq = wallet->GetV7PqMasterKey();
    Exec(db,
         "CREATE TRIGGER refuse_pq BEFORE UPDATE ON settings WHEN "
         "NEW.key='v7_pq_master_key_encrypted' BEGIN SELECT RAISE(ABORT,'required PQ wrap'); END");
    EXPECT_THROW(wallet->changePassphrase("commit-owner", "new-owner"), std::runtime_error);
    EXPECT_EQ(EncryptionRows(), encrypted);
    EXPECT_EQ(wallet->GetV7PqMasterKey(), pq);
    CheckSignature(key);
    Exec(db, "DROP TRIGGER refuse_pq");
    EXPECT_THROW(wallet->changePassphrase("wrong-owner", "new-owner"), std::runtime_error);
    EXPECT_EQ(EncryptionRows(), encrypted);
    Exec(db, "BEGIN IMMEDIATE");
    EXPECT_THROW(wallet->changePassphrase("commit-owner", "new-owner"), std::runtime_error);
    EXPECT_EQ(sqlite3_get_autocommit(db), 0);
    Exec(db, "ROLLBACK");
    EXPECT_EQ(EncryptionRows(), encrypted);
    {
        auto lease = wallet->AcquireDatabaseLease();
        auto pinned = lease->CopyRecoverySeed(lease->Session());
        EXPECT_THROW(wallet->changePassphrase("commit-owner", "new-owner"), std::runtime_error);
    }
    EXPECT_EQ(EncryptionRows(), encrypted);
    wallet->lockWallet();
    wallet->changePassphrase("commit-owner", "new-owner");
    EXPECT_TRUE(wallet->isWalletLocked());
    wallet->open("owner");
    wallet->unlockWallet("new-owner");
    CheckSignature(key);
    EXPECT_EQ(wallet->GetMasterSeed(), seed);
    EXPECT_EQ(wallet->GetV7PqMasterKey(), pq);
}
TEST_F(WalletEncryptionOwnerTest, LegacyImportedPayloadRoundtripAndFailureRollback) {
    wallet->open("owner");
    Import key(66);
    std::vector<uint8_t> legacy(32, 0);
    legacy.back() = 67;
    // Persisted predecessor-format fixture. Forward imports now use the
    // canonical owner and cannot recreate this historical storage domain.
    const std::string address="din1p4rltumyufleww78u3ste2aj55slz9skpesr8rhqdkcpfgrqqpeeskv96c7";
    Exec(wallet->getCurrentDatabase(),("INSERT INTO imported_keys(address,private_key_enc,label) VALUES('"+address+"','"+util::hex(legacy)+"','preserve legacy label')").c_str());
    ASSERT_FALSE(address.empty());
    ASSERT_TRUE(Store(key));
    auto *db = wallet->getCurrentDatabase();
    const auto legacy_rows = [&]() {
        sqlite3_stmt *q = nullptr;
        ASSERT_SQLITE(sqlite3_prepare_v2(db,
                                         "SELECT hex(CAST(private_key_enc AS BLOB))||':'||label "
                                         "FROM imported_keys WHERE address=?",
                                         -1, &q, nullptr),
                      SQLITE_OK);
        ASSERT_SQLITE(sqlite3_bind_text(q, 1, address.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
        ASSERT_SQLITE(sqlite3_step(q), SQLITE_ROW);
        std::string value(reinterpret_cast<const char *>(sqlite3_column_text(q, 0)));
        ASSERT_SQLITE(sqlite3_step(q), SQLITE_DONE);
        sqlite3_finalize(q);
        return value;
    };
    const auto original = legacy_rows();
    const auto before = EncryptionRows();
    Exec(db, "CREATE TRIGGER refuse_legacy BEFORE UPDATE ON imported_keys BEGIN SELECT "
             "RAISE(ABORT,'required legacy key write'); END");
    EXPECT_THROW(wallet->encryptWallet("legacy-first"), std::runtime_error);
    EXPECT_EQ(EncryptionRows(), before);
    EXPECT_FALSE(wallet->isWalletEncrypted());
    Exec(db, "DROP TRIGGER refuse_legacy");
    wallet->encryptWallet("legacy-first");
    EXPECT_NE(legacy_rows(), original);
    // Rotate while locked, before first unlock creates a P2MR master. Both legacy
    // raw/hex ciphertext representations are preserved, not used as HD paths.
    wallet->changePassphrase("legacy-first", "legacy-second");
    EXPECT_TRUE(wallet->isWalletLocked());
    wallet->decryptWallet("legacy-second");
    EXPECT_EQ(legacy_rows(), original);
    CheckSignature(key);
    wallet->encryptWallet("legacy-third");
    Exec(
        db,
        "UPDATE imported_keys SET private_key_enc='truncated' WHERE label='preserve legacy label'");
    const auto corrupt = EncryptionRows();
    EXPECT_THROW(wallet->changePassphrase("legacy-third", "legacy-fourth"), std::runtime_error);
    EXPECT_EQ(EncryptionRows(), corrupt);
    EXPECT_TRUE(wallet->isWalletLocked());
}
class WalletPrivateKeyImportTest : public WalletTaprootLookupTest {
protected:
    std::string Forward(const Import& key,uint64_t session=0,const std::string& expected="") {
        return wallet->importPrivateKey(std::vector<uint8_t>(key.secret.begin(),key.secret.end()),"forward label",session,expected);
    }
};
TEST_F(WalletPrivateKeyImportTest, CanonicalImportsReopenAndPreserveOtherImports) {
    wallet->open("owner");Import existing(71),one(72),two(73);ASSERT_TRUE(Store(existing));
    ASSERT_EQ(Forward(one),one.address);ASSERT_EQ(Forward(two),two.address);
    EXPECT_EQ(Scalar(wallet->getCurrentDatabase(),"SELECT COUNT(*) FROM imported_keys"),0);
    EXPECT_EQ(Scalar(wallet->getCurrentDatabase(),"SELECT COUNT(*) FROM addresses WHERE account=-1"),3);
    CheckSignature(existing);CheckSignature(one);CheckSignature(two);
    ASSERT_EQ(Forward(one),one.address);wallet->open("owner");CheckSignature(existing);CheckSignature(one);CheckSignature(two);
    wallet->encryptWallet("forward-pass");wallet->unlockWallet("forward-pass");Import encrypted(74);ASSERT_EQ(Forward(encrypted),encrypted.address);
    wallet->open("owner");wallet->unlockWallet("forward-pass");CheckSignature(existing);CheckSignature(one);CheckSignature(encrypted);
}
TEST_F(WalletPrivateKeyImportTest, ExpectedAddressSessionAndTransactionRefuse) {
    wallet->open("owner");Import existing(75),key(76);ASSERT_TRUE(Store(existing));const auto before=Rows();auto* db=wallet->getCurrentDatabase();
    EXPECT_TRUE(Forward(key,0,existing.address).empty());EXPECT_EQ(Rows(),before);
    uint64_t session=0;{auto lease=wallet->AcquireDatabaseLease();session=lease->Session();}wallet->open("owner");db=wallet->getCurrentDatabase();
    EXPECT_TRUE(Forward(key,session).empty());EXPECT_EQ(Rows(),before);
    Exec(db,"BEGIN IMMEDIATE");EXPECT_TRUE(Forward(key).empty());EXPECT_EQ(sqlite3_get_autocommit(db),0);EXPECT_EQ(Rows(),before);Exec(db,"ROLLBACK");
    Exec(db,"CREATE TRIGGER refuse_forward BEFORE INSERT ON taproot_keys BEGIN SELECT RAISE(ABORT,'required forward key'); END");
    EXPECT_TRUE(Forward(key).empty());EXPECT_EQ(Rows(),before);Exec(db,"DROP TRIGGER refuse_forward");
    ASSERT_EQ(Forward(key,0,key.address),key.address);CheckSignature(key);
    wallet->encryptWallet("locked-forward");Import locked(77);EXPECT_TRUE(Forward(locked).empty());
}
TEST_F(WalletPrivateKeyImportTest, LegacyInventoryAndReadFailureRefuseWithoutEffects) {
    wallet->open("owner");Import existing(78),key(79);ASSERT_TRUE(Store(existing));auto* db=wallet->getCurrentDatabase();const auto before=Rows();
    Exec(db,"INSERT INTO imported_keys(address,private_key_enc,label) VALUES('historical-inventory','unchanged historical bytes','preserved')");
    EXPECT_TRUE(Forward(key).empty());EXPECT_EQ(Rows(),before);EXPECT_EQ(Scalar(db,"SELECT COUNT(*) FROM imported_keys WHERE private_key_enc='unchanged historical bytes' AND label='preserved'"),1);CheckSignature(existing);
    Exec(db,"DELETE FROM imported_keys");
    sqlite3_set_authorizer(db,[](void*,int op,const char* table,const char*,const char*,const char*) {return op==SQLITE_READ && table && std::string_view(table)=="imported_keys"?SQLITE_DENY:SQLITE_OK;},nullptr);
    const bool refused=Forward(key).empty();sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(refused);EXPECT_EQ(Rows(),before);
    ASSERT_EQ(Forward(key),key.address);wallet->open("owner");CheckSignature(existing);CheckSignature(key);
}

class WalletRecoveryKeyTest : public WalletDatabaseLeaseTest {};
TEST_F(WalletRecoveryKeyTest, PinsActualSeedAcrossLockAndUnlock) {
    wallet->open("owner");
    const auto original=wallet->GetMasterSeed();ASSERT_TRUE(original.has_value());
    wallet->encryptWallet("recovery-test-passphrase");
    { auto lease=wallet->AcquireDatabaseLease();
      EXPECT_THROW(lease->CopyRecoverySeed(lease->Session()),std::runtime_error); }
    wallet->unlockWallet("recovery-test-passphrase");
    auto lease=wallet->AcquireDatabaseLease();const auto session=lease->Session();
    auto seed=lease->CopyRecoverySeed(session);
    EXPECT_TRUE(std::equal(seed->Bytes().begin(),seed->Bytes().end(),original->begin(),original->end()));
    EXPECT_THROW(lease->CopyRecoverySeed(session+1),std::runtime_error);
    EXPECT_THROW(lease->CopyRecoverySeed(session),std::runtime_error);
    EXPECT_THROW(wallet->lockWallet(),std::logic_error);
    EXPECT_THROW(wallet->unlockWallet("recovery-test-passphrase"),std::logic_error);
    EXPECT_THROW(wallet->storeMasterSeed(*original,"recovery-test-passphrase",false),std::logic_error);
    std::promise<void> entered;auto ready=entered.get_future();
    auto lock=std::async(std::launch::async,[&]{entered.set_value();wallet->lockWallet();});
    ready.wait();const bool excluded=lock.wait_for(100ms)==std::future_status::timeout;
    seed.reset();lease.reset();lock.get();EXPECT_TRUE(excluded);
    EXPECT_FALSE(wallet->GetMasterSeed().has_value());
    lease=wallet->AcquireDatabaseLease();
    EXPECT_THROW(lease->CopyRecoverySeed(session),std::runtime_error);
    std::promise<void> unlocking;auto started=unlocking.get_future();
    auto unlock=std::async(std::launch::async,[&]{unlocking.set_value();wallet->unlockWallet("recovery-test-passphrase");});
    started.wait();const bool unlock_excluded=unlock.wait_for(100ms)==std::future_status::timeout;
    lease.reset();unlock.get();EXPECT_TRUE(unlock_excluded);
    lease=wallet->AcquireDatabaseLease();EXPECT_EQ(lease->CopyRecoverySeed(session)->Bytes().size(),64u);
}
TEST_F(WalletRecoveryKeyTest, RefusesExpiredEmptyAndReopenedSelection) {
    dinero::WalletManager empty(path/"empty");
    { auto lease=empty.AcquireDatabaseLease();EXPECT_THROW(lease->CopyRecoverySeed(lease->Session()),std::runtime_error); }
    wallet->open("owner");uint64_t session;
    { auto lease=wallet->AcquireDatabaseLease();session=lease->Session();
      EXPECT_EQ(lease->CopyRecoverySeed(session)->Bytes().size(),64u); }
    wallet->open("owner");
    { auto lease=wallet->AcquireDatabaseLease();EXPECT_THROW(lease->CopyRecoverySeed(session),std::runtime_error); }
    wallet->encryptWallet("recovery-test-passphrase");wallet->unlockWallet("recovery-test-passphrase",1);
    std::this_thread::sleep_for(1100ms);
    auto lease=wallet->AcquireDatabaseLease();
    EXPECT_THROW(lease->CopyRecoverySeed(lease->Session()),std::runtime_error);
    EXPECT_FALSE(wallet->GetMasterSeed().has_value());
}

class WalletDeliveryBindingTest : public WalletDatabaseLeaseTest {};
TEST_F(WalletDeliveryBindingTest, StableDatabaseIdentitySurvivesReopenAndMetadataChanges) {
    wallet->open("owner");
    std::string identity;
    { auto lease=wallet->AcquireDatabaseLease();identity=lease->EnsureDeliveryIdentity();
      EXPECT_EQ(identity.size(),71u);EXPECT_EQ(lease->EnsureDeliveryIdentity(),identity); }
    wallet->open("owner");EXPECT_EQ(wallet->AcquireDatabaseLease()->EnsureDeliveryIdentity(),identity);
    { auto lease=wallet->AcquireDatabaseLease();
      Exec(lease->Database(),"UPDATE wallet_meta SET name='display-name-changed' WHERE id=1");
      EXPECT_EQ(lease->EnsureDeliveryIdentity(),identity); }
    wallet->create("other");wallet->open("other");
    EXPECT_NE(wallet->AcquireDatabaseLease()->EnsureDeliveryIdentity(),identity);
    wallet.reset();wallet=std::make_unique<dinero::WalletManager>(path);wallet->open("owner");
    EXPECT_EQ(wallet->AcquireDatabaseLease()->EnsureDeliveryIdentity(),identity);
    dinero::WalletManager empty(path/"empty");
    EXPECT_THROW(empty.AcquireDatabaseLease()->EnsureDeliveryIdentity(),std::runtime_error);
}
TEST_F(WalletDeliveryBindingTest, FailedWritesAndCommitDoNotPublishIdentity) {
    wallet->open("owner");auto lease=wallet->AcquireDatabaseLease();auto* db=lease->Database();
    const auto fails=[&](const char* expected) {
        try { (void)lease->EnsureDeliveryIdentity();ADD_FAILURE()<<"Expected identity refusal"; }
        catch(const std::runtime_error& error){EXPECT_EQ(std::string(error.what()),expected);}
    };
    Exec(db,"CREATE TRIGGER reject_schema BEFORE UPDATE ON wallet_meta BEGIN SELECT RAISE(ABORT,'identity schema failure'); END");
    fails("Wallet delivery identity write failed");
    { sqlite3_stmt* stmt=nullptr;ASSERT_EQ(sqlite3_prepare_v2(db,"SELECT count(*) FROM pragma_table_info('wallet_meta') WHERE name='runtime_delivery_id'",-1,&stmt,nullptr),SQLITE_OK);
      ASSERT_EQ(sqlite3_step(stmt),SQLITE_ROW);EXPECT_EQ(sqlite3_column_int(stmt,0),0);sqlite3_finalize(stmt); }
    Exec(db,"DROP TRIGGER reject_schema; ALTER TABLE wallet_meta ADD COLUMN runtime_delivery_id BLOB");
    const auto absent=[&] { sqlite3_stmt* s=nullptr;ASSERT_EQ(sqlite3_prepare_v2(db,"SELECT runtime_delivery_id FROM wallet_meta WHERE id=1",-1,&s,nullptr),SQLITE_OK);
        ASSERT_EQ(sqlite3_step(s),SQLITE_ROW);EXPECT_EQ(sqlite3_column_type(s,0),SQLITE_NULL);sqlite3_finalize(s); };
    Exec(db,"CREATE TRIGGER reject_delivery BEFORE UPDATE ON wallet_meta BEGIN SELECT RAISE(ABORT,'identity write failure'); END");
    fails("Wallet delivery identity write failed");absent();
    Exec(db,"DROP TRIGGER reject_delivery; CREATE TABLE delivery_parent(id INTEGER PRIMARY KEY); CREATE TABLE delivery_child(id INTEGER REFERENCES delivery_parent(id) DEFERRABLE INITIALLY DEFERRED); CREATE TRIGGER reject_commit AFTER UPDATE ON wallet_meta BEGIN INSERT INTO delivery_child VALUES(99); END");
    fails("Wallet delivery identity SQL failed");absent();
    Exec(db,"DROP TRIGGER reject_commit; BEGIN IMMEDIATE; UPDATE wallet_meta SET name='pending' WHERE id=1");
    fails("Wallet delivery identity ownership unavailable");EXPECT_EQ(sqlite3_get_autocommit(db),0);
    Exec(db,"ROLLBACK");absent();
    const auto identity=lease->EnsureDeliveryIdentity();EXPECT_FALSE(identity.empty());
    Exec(db,"UPDATE wallet_meta SET runtime_delivery_id=x'00' WHERE id=1");
    fails("Wallet delivery identity malformed");
}

TEST_F(WalletDatabaseLeaseTest, SerializesConnectionAndPinsWalletThroughSwitch) {
    wallet->create("other");
    wallet->open("owner");
    auto lease = wallet->AcquireDatabaseLease();
    ASSERT_EQ(lease->WalletName(), "owner");
    ASSERT_EQ(lease->Database(), wallet->getCurrentDatabase());
    sqlite3* db = lease->Database();
    // A deterministic probe of SQLite's real connection mutex, not a mocked
    // writer or a timing-only observation that another thread hasn't run.
    auto attempt = std::async(std::launch::async, [db] {
        auto* mutex = sqlite3_db_mutex(db);
        const int rc = sqlite3_mutex_try(mutex);
        if (rc == SQLITE_OK) sqlite3_mutex_leave(mutex);
        return rc;
    });
    EXPECT_EQ(attempt.get(), SQLITE_BUSY);
    EXPECT_THROW(wallet->open("other"), std::logic_error);
    EXPECT_THROW(wallet->create("reentrant"), std::logic_error);
    EXPECT_FALSE(wallet->exists("reentrant"));
    std::promise<void> entered;
    auto ready = entered.get_future();
    auto change = std::async(std::launch::async, [&] {
        entered.set_value();
        wallet->open("other");
    });
    ready.wait();
    EXPECT_EQ(change.wait_for(100ms), std::future_status::timeout);
    EXPECT_EQ(lease->WalletName(), "owner");
    lease.reset();
    EXPECT_EQ(change.wait_for(5s), std::future_status::ready);
    EXPECT_NO_THROW(change.get());
    EXPECT_EQ(wallet->AcquireDatabaseLease()->WalletName(), "other");
}

TEST_F(WalletDatabaseLeaseTest, RejectsBorrowedTransactionAndRollsBackOnlyOwnedWork) {
    sqlite3* db = wallet->getCurrentDatabase();
    Exec(db, "CREATE TABLE lease_probe(value INTEGER)");
    Exec(db, "BEGIN IMMEDIATE; INSERT INTO lease_probe VALUES(1)");
    EXPECT_THROW(wallet->AcquireDatabaseLease(), std::runtime_error);
    EXPECT_EQ(sqlite3_get_autocommit(db), 0);
    EXPECT_EQ(Count(db), 1);
    Exec(db, "ROLLBACK");
    {
        auto lease = wallet->AcquireDatabaseLease();
        Exec(lease->Database(), "BEGIN IMMEDIATE; INSERT INTO lease_probe VALUES(2)");
        EXPECT_EQ(Count(lease->Database()), 1);
    }
    EXPECT_EQ(sqlite3_get_autocommit(db), 1);
    EXPECT_EQ(Count(db), 0);
    {
        auto lease = wallet->AcquireDatabaseLease();
        Exec(lease->Database(), "BEGIN IMMEDIATE; INSERT INTO lease_probe VALUES(3); COMMIT");
    }
    wallet.reset();
    wallet = std::make_unique<dinero::WalletManager>(path);
    wallet->open("owner");
    EXPECT_EQ(Count(wallet->getCurrentDatabase()), 1);
}

TEST_F(WalletDatabaseLeaseTest, ConcurrentRawWriteCannotJoinLeasedTransaction) {
    auto lease = wallet->AcquireDatabaseLease();
    sqlite3* db = lease->Database();
    Exec(db, "CREATE TABLE lease_probe(value INTEGER)");
    Exec(db, "BEGIN IMMEDIATE; INSERT INTO lease_probe VALUES(1)");
    std::promise<void> entered;
    auto ready = entered.get_future();
    auto write = std::async(std::launch::async, [&] {
        entered.set_value();
        Exec(db, "INSERT INTO lease_probe VALUES(2)");
    });
    ready.wait();
    EXPECT_EQ(write.wait_for(100ms), std::future_status::timeout);
    lease.reset(); // rolls back 1 before the competing statement can execute
    EXPECT_NO_THROW(write.get());
    EXPECT_EQ(Count(db), 1);
}

TEST_F(WalletDatabaseLeaseTest, EmptySelectionCanBeLeasedWithoutFabricatedDatabase) {
    wallet.reset();
    wallet = std::make_unique<dinero::WalletManager>(path);
    auto lease = wallet->AcquireDatabaseLease();
    EXPECT_EQ(lease->Database(), nullptr);
    EXPECT_TRUE(lease->WalletName().empty());
    EXPECT_THROW(wallet->open("owner"), std::logic_error);
    lease.reset();
    EXPECT_NO_THROW(wallet->open("owner"));
}

TEST_F(WalletDatabaseLeaseTest, NestedOwnerPreservesOuterTransaction) {
    auto outer = wallet->AcquireDatabaseLease();
    sqlite3* db = outer->Database();
    Exec(db, "CREATE TABLE lease_probe(value INTEGER)");
    Exec(db, "BEGIN IMMEDIATE; INSERT INTO lease_probe VALUES(1)");
    {
        auto inner = wallet->AcquireDatabaseLease();
        EXPECT_EQ(inner->Database(), db);
        std::string error;
        EXPECT_TRUE(dinero::wallet::shielded_ops::EnsureWalletRuntime(*wallet, &error)) << error;
    }
    EXPECT_EQ(sqlite3_get_autocommit(db), 0);
    EXPECT_EQ(Count(db), 1);
    outer.reset();
    EXPECT_EQ(sqlite3_get_autocommit(db), 1);
    EXPECT_EQ(Count(db), 0);
}

TEST_F(WalletDatabaseLeaseTest, RuntimePinsWalletBeforeTakingSharedRuntimeLock) {
    dinero::WalletManager other(path / "other-manager");
    other.create("other");
    auto lease = wallet->AcquireDatabaseLease();
    std::promise<void> entered;
    auto ready = entered.get_future();
    auto first = std::async(std::launch::async, [&] {
        entered.set_value();
        return dinero::wallet::shielded_ops::EnsureWalletRuntime(*wallet, nullptr);
    });
    ready.wait();
    // This wallet's runtime must respect the already-held lifetime lease.
    EXPECT_EQ(first.wait_for(200ms), std::future_status::timeout);
    auto second = std::async(std::launch::async, [&] {
        return dinero::wallet::shielded_ops::EnsureWalletRuntime(other, nullptr);
    });
    // Waiting for one wallet must not hold the process-wide runtime mutex and
    // block a different wallet. Always release before joining, even on failure.
    EXPECT_EQ(second.wait_for(2s), std::future_status::ready);
    lease.reset();
    EXPECT_TRUE(first.get());
    EXPECT_TRUE(second.get());
}
TEST_F(WalletDatabaseLeaseTest, RealWorkerCommitsIndexBlockTogether) {
    dinero::SelectParams(dinero::Chain::REGTEST);
    dinero::UTXOIndex index((path / "worker-index.sqlite").string());
    ASSERT_TRUE(index.Initialize());
    const std::vector<uint8_t> script{0x51};
    index.RegisterAddress(script, "m/84'/1448'/0'/0/0");
    dinero::uint256 hash; hash.begin()[0] = 1;
    dinero::TxId seed(hash);
    ASSERT_TRUE(index.AddUTXO(dinero::WalletUTXO(seed, 0, dinero::AmountUna::Una(1000),
        script, "m/84'/1448'/0'/0/0", 10, false)));
    dinero::Transaction first;
    first.vin.resize(1); first.vin[0].prevout = dinero::TxOutPoint(seed, 0);
    first.vout.emplace_back(dinero::AmountUna::Una(800), script);
    dinero::Transaction second;
    second.vin.resize(1); second.vin[0].prevout = dinero::TxOutPoint(first.GetTxid(), 0);
    second.vout.emplace_back(dinero::AmountUna::Una(600), script);
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open((path / "worker-index.sqlite").string().c_str(), &raw), SQLITE_OK);
    const auto close = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(raw, sqlite3_close);
    Exec(raw, "CREATE TRIGGER fail_second BEFORE INSERT ON wallet_utxos WHEN NEW.value=600 BEGIN SELECT RAISE(ABORT, 'test worker insert'); END");
    dinero::WalletWorker worker(&index, nullptr); // Real worker/index, no other store claim.
    std::string error;
    try { dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}); }
    catch (const std::runtime_error& e) { error = e.what(); }
    EXPECT_EQ(error, "Wallet UTXO block insert failed");
    ASSERT_TRUE(index.GetUTXO(seed, 0));
    EXPECT_FALSE(index.GetUTXO(seed, 0)->spend_height);
    EXPECT_FALSE(index.GetUTXO(first.GetTxid(), 0));
    EXPECT_FALSE(index.GetUTXO(second.GetTxid(), 0));
    Exec(raw, "DROP TRIGGER fail_second");
    EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}));
    ASSERT_TRUE(index.GetUTXO(first.GetTxid(), 0));
    EXPECT_EQ(index.GetUTXO(seed, 0)->spend_height, 20);
    EXPECT_EQ(index.GetUTXO(first.GetTxid(), 0)->spend_height, 20);
    EXPECT_FALSE(index.GetUTXO(second.GetTxid(), 0)->spend_height);
    EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}));
    EXPECT_EQ(index.GetUTXO(first.GetTxid(), 0)->spend_height, 20);
}

class WalletQueuedIdentityTest : public WalletDatabaseLeaseTest {};

TEST_F(WalletQueuedIdentityTest, RejectsReplacedAndReopenedWalletBeforeAnyStoreEffect) {
    dinero::SelectParams(dinero::Chain::REGTEST);
    wallet->create("other");
    wallet->open("owner");
    const auto session = wallet->AcquireDatabaseLease()->Session();
    dinero::UTXOIndex index((path / "queue-index.sqlite").string());
    ASSERT_TRUE(index.Initialize());
    const std::vector<uint8_t> script{0x00, 0x14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                    11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    index.RegisterAddress(script, "m/84'/1448'/0'/0/0");
    dinero::uint256 hash; hash.begin()[0] = 7;
    const dinero::TxId seed(hash);
    ASSERT_TRUE(index.AddUTXO(dinero::WalletUTXO(seed, 0, dinero::AmountUna::Una(1000),
        script, "m/84'/1448'/0'/0/0", 20, false)));
    dinero::Transaction tx;
    tx.vin.resize(1); tx.vin[0].prevout = dinero::TxOutPoint(seed, 0);
    tx.vout.emplace_back(dinero::AmountUna::Una(800), script);
    dinero::Block block; block.vtx = {tx};
    dinero::WalletWorker worker(&index, wallet.get());
    dinero::WalletWorkerTestAccess::EnableQueue(worker);
    worker.QueueBlockConnected(21, std::string(64, '2'), {tx});
    worker.QueueBlockDisconnected(20, block);
    dinero::ReorgDiff diff; diff.disconnect.emplace_back(20, std::string(64, '3'));
    worker.QueueReorg(diff);
    std::vector<dinero::WalletJob> jobs;
    for (int i = 0; i < 3; ++i) {
        jobs.push_back(dinero::WalletWorkerTestAccess::Take(worker));
        ASSERT_EQ(jobs.back().wallet_session, session);
    }
    wallet->open("other");
    wallet->setBlockchainHeight(10);
    sqlite3* other = wallet->getCurrentDatabase();
    Exec(other, "CREATE TABLE lease_probe(value INTEGER)");
    // Any ordinary block operation on the replacement wallet must be absent.
    Exec(other, "CREATE TRIGGER capture_insert AFTER INSERT ON utxos BEGIN INSERT INTO lease_probe VALUES(1); END");
    Exec(other, "CREATE TRIGGER capture_delete AFTER DELETE ON utxos BEGIN INSERT INTO lease_probe VALUES(2); END");
    auto refuses = [&](const dinero::WalletJob& job) {
        std::string error;
        try { dinero::WalletWorkerTestAccess::Dispatch(worker, job); }
        catch (const std::runtime_error& e) { error = e.what(); }
        EXPECT_EQ(error, "Wallet job session changed; canonical recovery required");
        ASSERT_TRUE(index.GetUTXO(seed, 0));
        EXPECT_FALSE(index.GetUTXO(seed, 0)->spend_height);
        EXPECT_FALSE(index.GetUTXO(tx.GetTxid(), 0));
    };
    for (const auto& job : jobs) refuses(job);
    EXPECT_EQ(Count(other), 0);
    EXPECT_EQ(wallet->getBlockchainHeight(), 10);
    wallet->open("owner");
    ASSERT_NE(wallet->AcquireDatabaseLease()->Session(), session);
    // Same name and possibly reused SQLite address do not restore the old session.
    for (const auto& job : jobs) refuses(job);
    ASSERT_TRUE(wallet->addUTXO(seed.AsUint256().GetHex(), 0, 1000, "seed", "0014", 20, false));
    worker.QueueBlockConnected(21, std::string(64, '2'), {tx});
    const auto current = dinero::WalletWorkerTestAccess::Take(worker);
    EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Dispatch(worker, current));
    ASSERT_TRUE(index.GetUTXO(seed, 0));
    EXPECT_EQ(index.GetUTXO(seed, 0)->spend_height, 21);
    EXPECT_TRUE(index.GetUTXO(tx.GetTxid(), 0));
    EXPECT_EQ(wallet->getBlockchainHeight(), 21);
}

TEST_F(WalletQueuedIdentityTest, EmptySelectionAndMissingBindingCannotRetargetWork) {
    dinero::SelectParams(dinero::Chain::REGTEST);
    wallet->unload();
    dinero::WalletWorker worker(nullptr, wallet.get());
    dinero::WalletWorkerTestAccess::EnableQueue(worker);
    worker.QueueBlockConnected(1, std::string(64, '1'), {});
    const auto unselected = dinero::WalletWorkerTestAccess::Take(worker);
    ASSERT_TRUE(unselected.wallet_session);
    const auto empty_session = wallet->AcquireDatabaseLease()->Session();
    EXPECT_EQ(unselected.wallet_session, empty_session);
    std::string empty_error;
    try { dinero::WalletWorkerTestAccess::Dispatch(worker, unselected); }
    catch (const std::runtime_error& e) { empty_error = e.what(); }
    EXPECT_EQ(empty_error, "Wallet job has no selected database; canonical recovery required");
    wallet->open("owner");
    auto session = wallet->AcquireDatabaseLease()->Session();
    EXPECT_NE(session, empty_session);
    std::string error;
    try { dinero::WalletWorkerTestAccess::Dispatch(worker, unselected); }
    catch (const std::runtime_error& e) { error = e.what(); }
    EXPECT_EQ(error, "Wallet job session changed; canonical recovery required");
    auto unbound = dinero::WalletJob::MakeConnect(1, std::string(64, '1'), {});
    error.clear();
    try { dinero::WalletWorkerTestAccess::Dispatch(worker, unbound); }
    catch (const std::runtime_error& e) { error = e.what(); }
    EXPECT_EQ(error, "Wallet job has no matching manager binding");
    {
        auto lease = wallet->AcquireDatabaseLease();
        EXPECT_EQ(lease->Session(), session);
        EXPECT_EQ(wallet->AcquireDatabaseLease()->Session(), session);
        EXPECT_THROW(wallet->open("owner"), std::logic_error);
        EXPECT_EQ(lease->Session(), session);
    }
    // A refusal before touching the selection must not discard valid jobs.
    EXPECT_THROW(wallet->open("missing"), std::runtime_error);
    EXPECT_EQ(wallet->AcquireDatabaseLease()->Session(), session);
    EXPECT_THROW(wallet->create("owner"), std::runtime_error);
    EXPECT_EQ(wallet->AcquireDatabaseLease()->Session(), session);
    dinero::WalletWorker index_only;
    dinero::WalletWorkerTestAccess::EnableQueue(index_only);
    index_only.QueueBlockConnected(1, std::string(64, '1'), {});
    const auto job = dinero::WalletWorkerTestAccess::Take(index_only);
    EXPECT_FALSE(job.wallet_session);
    EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Dispatch(index_only, job));
}

class WalletOrdinaryBlockTest : public WalletDatabaseLeaseTest,
                                public ::testing::WithParamInterface<std::string> {};

TEST_P(WalletOrdinaryBlockTest, ChecksOrdinaryWalletWritesAndCommitBeforeHeight) {
    dinero::SelectParams(dinero::Chain::REGTEST);
    const std::vector<uint8_t> script{0x00, 0x14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                    11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    auto scalar = [](sqlite3* db, const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("prepare state query");
        const int rc = sqlite3_step(stmt);
        const int result = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
        sqlite3_finalize(stmt);
        return result;
    };
    const std::string stage = GetParam();
    {
        wallet->create("block-" + stage);
        sqlite3* db = wallet->getCurrentDatabase();
        dinero::UTXOIndex index((path / (stage + "-index.sqlite")).string());
        ASSERT_TRUE(index.Initialize());
        index.RegisterAddress(script, "m/84'/1448'/0'/0/0");
        dinero::uint256 hash; hash.begin()[0] = 2;
        dinero::Transaction first;
        first.vin.resize(1); first.vin[0].prevout = dinero::TxOutPoint(dinero::TxId(hash), 0);
        first.vout.emplace_back(dinero::AmountUna::Una(800), script);
        dinero::Transaction second;
        second.vin.resize(1); second.vin[0].prevout = dinero::TxOutPoint(first.GetTxid(), 0);
        second.vout.emplace_back(dinero::AmountUna::Una(600), script);
        const auto first_id = first.GetTxid().AsUint256().GetHex();
        if (stage == "confirmation") {
            ASSERT_TRUE(wallet->addTransaction(first_id, "pending", 0.000008, "receive", false));
            Exec(db, "CREATE TRIGGER fail_block BEFORE UPDATE ON transactions BEGIN SELECT RAISE(ABORT, 'test confirmation'); END");
        } else if (stage == "insert") {
            Exec(db, "CREATE TRIGGER fail_block BEFORE INSERT ON utxos WHEN NEW.amount=600 BEGIN SELECT RAISE(ABORT, 'test ordinary insert'); END");
        } else if (stage == "spend") {
            Exec(db, "CREATE TRIGGER fail_block BEFORE UPDATE OF is_spent ON utxos BEGIN SELECT RAISE(ABORT, 'test ordinary spend'); END");
        } else if (stage == "history") {
            Exec(db, "CREATE TRIGGER fail_block BEFORE INSERT ON transactions BEGIN SELECT RAISE(ABORT, 'test ordinary history'); END");
        } else {
            Exec(db, "PRAGMA foreign_keys=ON; CREATE TABLE commit_parent(id INTEGER PRIMARY KEY); CREATE TABLE commit_child(id INTEGER REFERENCES commit_parent(id) DEFERRABLE INITIALLY DEFERRED); CREATE TRIGGER fail_block AFTER INSERT ON utxos BEGIN INSERT INTO commit_child VALUES(1); END");
        }
        const int initial_history = scalar(db, "SELECT count(*) FROM transactions");
        const auto initial_height = wallet->getBlockchainHeight();
        dinero::WalletWorker worker(&index, wallet.get());
        std::string error;
        try { dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}); }
        catch (const std::runtime_error& e) { error = e.what(); }
        if (stage == "confirmation") EXPECT_EQ(error, "Wallet block confirmation failed: test confirmation");
        else if (stage == "commit") EXPECT_EQ(error, "Wallet block commit failed: FOREIGN KEY constraint failed");
        else if (stage == "history") EXPECT_EQ(error, "Wallet block history insert failed");
        else EXPECT_EQ(error, "Wallet block ordinary " + stage + " failed");
        EXPECT_EQ(sqlite3_get_autocommit(db), 1);
        EXPECT_EQ(scalar(db, "SELECT count(*) FROM utxos"), 0);
        EXPECT_EQ(scalar(db, "SELECT count(*) FROM transactions"), initial_history);
        if (stage == "confirmation") EXPECT_EQ(scalar(db, "SELECT height FROM transactions LIMIT 1"), 0);
        EXPECT_EQ(wallet->getBlockchainHeight(), initial_height);
        // Index commits before ordinary-wallet COMMIT. This is a recoverable
        // prefix to replay, not a claim of atomicity between the two databases.
        EXPECT_EQ(index.GetUTXO(first.GetTxid(), 0).has_value(), stage == "commit");
        Exec(db, "DROP TRIGGER fail_block");
        EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}));
        EXPECT_EQ(wallet->getBlockchainHeight(), 20);
        EXPECT_EQ(scalar(db, "SELECT count(*) FROM utxos"), 2);
        EXPECT_EQ(scalar(db, "SELECT is_spent FROM utxos WHERE amount=800"), 1);
        EXPECT_EQ(scalar(db, "SELECT is_spent FROM utxos WHERE amount=600"), 0);
        EXPECT_EQ(index.GetUTXO(first.GetTxid(), 0)->spend_height, 20);
        EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Connect(worker, 20, {first, second}));
        // A creation-only replay may not erase a recorded spend in either store.
        EXPECT_NO_THROW(dinero::WalletWorkerTestAccess::Connect(worker, 20, {first}));
        EXPECT_EQ(scalar(db, "SELECT is_spent FROM utxos WHERE amount=800"), 1);
        EXPECT_EQ(index.GetUTXO(first.GetTxid(), 0)->spend_height, 20);
        wallet->open("owner");
        wallet->open("block-" + stage);
        EXPECT_EQ(scalar(wallet->getCurrentDatabase(), "SELECT is_spent FROM utxos WHERE amount=800"), 1);
    }
}
INSTANTIATE_TEST_SUITE_P(DeliveryStages, WalletOrdinaryBlockTest,
    ::testing::Values("insert", "spend", "history", "confirmation", "commit"),
    [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

} // namespace

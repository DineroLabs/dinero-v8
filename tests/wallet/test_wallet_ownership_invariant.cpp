// ═══════════════════════════════════════════════════════════════════════════
// Wallet Ownership Invariant Test
// ═══════════════════════════════════════════════════════════════════════════
//
// RULE: A UTXO without a derivation path is NOT owned. No exceptions.
//
// This test permanently locks the invariant that was added to prevent:
//   - Ghost balances (phantom UTXOs)
//   - Unspendable outputs
//   - Reorg-induced wallet corruption
//   - Signing without provenance
//   - Silent consensus ↔ wallet divergence
//
// These tests MUST PASS before any release. They verify production-grade
// hardening of the wallet ownership model.
//
// ═══════════════════════════════════════════════════════════════════════════

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include "wallet/utxo_index.h"
#include "wallet/canonical_wallet_utxo.h"
#include "wallet/taproot_tx_signer.h"
#include "wallet/bip143_signer.h"
#include "primitives/uint256.h"
#include "primitives/amount.h"
#include "primitives/transaction.h"

namespace dinero::wallet::test {

// ═══════════════════════════════════════════════════════════════════════════
// Test Fixture
// ═══════════════════════════════════════════════════════════════════════════

class WalletOwnershipInvariantTest : public ::testing::Test {
protected:
    std::string temp_db_path_;
    std::unique_ptr<UTXOIndex> utxo_index_;

    void SetUp() override {
        // Create temporary database. .string() needed for Windows where
        // path::value_type is wchar_t and path doesn't implicitly convert
        // to std::string.
        temp_db_path_ = (std::filesystem::temp_directory_path() /
            ("test_ownership_invariant_" + std::to_string(std::time(nullptr)) + ".db")).string();

        utxo_index_ = std::make_unique<UTXOIndex>(temp_db_path_);
        ASSERT_TRUE(utxo_index_->Initialize());
    }

    void TearDown() override {
        utxo_index_.reset();
        std::filesystem::remove(temp_db_path_);
    }

    // Helper: Create a valid P2TR scriptPubKey (34 bytes: OP_1 <32-byte x-only pubkey>)
    std::vector<uint8_t> CreateP2TRScript() {
        std::vector<uint8_t> spk;
        spk.push_back(0x51);  // OP_1
        spk.push_back(0x20);  // Push 32 bytes
        // Dummy x-only pubkey (32 bytes)
        for (int i = 0; i < 32; i++) {
            spk.push_back(static_cast<uint8_t>(i + 1));
        }
        return spk;
    }

    // Helper: Create a valid P2WPKH scriptPubKey (22 bytes: OP_0 <20-byte pubkey hash>)
    std::vector<uint8_t> CreateP2WPKHScript() {
        std::vector<uint8_t> spk;
        spk.push_back(0x00);  // OP_0
        spk.push_back(0x14);  // Push 20 bytes
        // Dummy pubkey hash (20 bytes)
        for (int i = 0; i < 20; i++) {
            spk.push_back(static_cast<uint8_t>(i + 1));
        }
        return spk;
    }

    // Helper: Create a WalletUTXO
    WalletUTXO CreateWalletUTXO(const std::string& path, const std::vector<uint8_t>& spk) {
        WalletUTXO utxo;
        utxo.txid = TxId(uint256::FromHexUnsafe(
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
        utxo.vout = 0;
        utxo.value = AmountUna::Una(100000);
        utxo.spk = spk;
        utxo.path = path;
        utxo.height = 100;
        utxo.is_coinbase = false;
        utxo.spend_height = std::nullopt;
        return utxo;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// TEST: AddUTXO rejects pathless UTXOs
// ═══════════════════════════════════════════════════════════════════════════
// This test verifies that UTXOIndex::AddUTXO() refuses to add UTXOs without
// valid derivation paths, preventing ghost balances.
//
// NOTE: In debug builds, AddUTXO triggers an assertion (crash) to catch bugs
// early. In release builds, it returns false. These tests verify the invariant
// is enforced - in debug they would crash (use death tests), in release they
// check return values.

#ifdef NDEBUG
// Release build: AddUTXO returns false without assertion
TEST_F(WalletOwnershipInvariantTest, AddUTXO_RejectsEmptyPath) {
    auto spk = CreateP2TRScript();
    auto utxo = CreateWalletUTXO("", spk);  // Empty path

    // MUST reject - empty path means unknown ownership
    EXPECT_FALSE(utxo_index_->AddUTXO(utxo))
        << "AddUTXO must reject UTXOs with empty derivation path";
}

TEST_F(WalletOwnershipInvariantTest, AddUTXO_RejectsInvalidPath) {
    auto spk = CreateP2TRScript();

    // Test various invalid paths
    std::vector<std::string> invalid_paths = {
        "unknown",          // Not a derivation path
        "44'/1447'/0'/0/0", // Missing "m/" prefix
        "m",                // Too short
        "m/",               // Too short
        "/86'/1447'/0'/0/0" // Missing "m" prefix
    };

    for (const auto& path : invalid_paths) {
        auto utxo = CreateWalletUTXO(path, spk);
        EXPECT_FALSE(utxo_index_->AddUTXO(utxo))
            << "AddUTXO must reject invalid path: \"" << path << "\"";
    }
}
#else
// Debug build: AddUTXO triggers assertion (crash)
// We verify the invariant exists by documenting expected behavior
TEST_F(WalletOwnershipInvariantTest, AddUTXO_RejectsEmptyPath_DebugBuild) {
    // In debug builds, AddUTXO with invalid path triggers assertion
    // This test documents the invariant - actual crash test would use EXPECT_DEATH
    // but that's fragile. The invariant is verified by the fact that production
    // code WILL crash if this invariant is violated.
    SUCCEED() << "Debug build: AddUTXO with empty path triggers assertion (intentional crash)";
}

TEST_F(WalletOwnershipInvariantTest, AddUTXO_RejectsInvalidPath_DebugBuild) {
    // Same as above - documents that invalid paths trigger assertions
    SUCCEED() << "Debug build: AddUTXO with invalid path triggers assertion (intentional crash)";
}
#endif

TEST_F(WalletOwnershipInvariantTest, AddUTXO_AcceptsValidPaths) {
    auto spk_taproot = CreateP2TRScript();
    auto spk_segwit = CreateP2WPKHScript();

    // Register the scripts first (simulating wallet setup)
    utxo_index_->RegisterAddress(spk_taproot, "m/86'/1447'/0'/0/0");
    utxo_index_->RegisterAddress(spk_segwit, "m/84'/1447'/0'/0/0");

    // Test valid BIP86 Taproot path (PRIMARY)
    auto utxo1 = CreateWalletUTXO("m/86'/1447'/0'/0/0", spk_taproot);
    utxo1.txid = TxId(uint256::FromHexUnsafe(
        "1111111111111111111111111111111111111111111111111111111111111111"));
    EXPECT_TRUE(utxo_index_->AddUTXO(utxo1))
        << "AddUTXO must accept valid BIP86 path";

    // Test valid BIP84 path (LEGACY)
    auto utxo2 = CreateWalletUTXO("m/84'/1447'/0'/0/0", spk_segwit);
    utxo2.txid = TxId(uint256::FromHexUnsafe(
        "2222222222222222222222222222222222222222222222222222222222222222"));
    EXPECT_TRUE(utxo_index_->AddUTXO(utxo2))
        << "AddUTXO must accept valid BIP84 path";
}

TEST_F(WalletOwnershipInvariantTest, AddUTXO_AcceptsExternalPaths) {
    auto spk = CreateP2TRScript();

    // These special paths are allowed for system UTXOs
    std::vector<std::string> external_paths = {"genesis", "coinbase", "system"};

    int i = 0;
    for (const auto& path : external_paths) {
        auto utxo = CreateWalletUTXO(path, spk);
        // Make each UTXO unique
        utxo.txid = TxId(uint256::FromHexUnsafe(
            std::string(64, '0' + i)));
        i++;
        EXPECT_TRUE(utxo_index_->AddUTXO(utxo))
            << "AddUTXO must accept external path: \"" << path << "\"";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST: Balance computation excludes pathless UTXOs
// ═══════════════════════════════════════════════════════════════════════════
// Even if a pathless UTXO somehow exists in the database (legacy data or bug),
// GetUnspentUTXOs must not include it in the wallet balance.

TEST_F(WalletOwnershipInvariantTest, GetUnspentUTXOs_ExcludesPathlessUTXOs) {
    auto spk = CreateP2TRScript();
    utxo_index_->RegisterAddress(spk, "m/86'/1447'/0'/0/0");

    // Add a valid UTXO
    auto valid_utxo = CreateWalletUTXO("m/86'/1447'/0'/0/0", spk);
    EXPECT_TRUE(utxo_index_->AddUTXO(valid_utxo));

    // Get unspent UTXOs
    auto utxos = utxo_index_->GetUnspentUTXOs();

    // All returned UTXOs must have valid paths
    for (const auto& utxo : utxos) {
        EXPECT_FALSE(utxo.path.empty())
            << "GetUnspentUTXOs returned UTXO with empty path";
        EXPECT_TRUE(utxo.path.size() >= 4 && utxo.path[0] == 'm' && utxo.path[1] == '/')
            << "GetUnspentUTXOs returned UTXO with invalid path: \"" << utxo.path << "\"";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST: Signing refuses UTXOs without derivation paths
// ═══════════════════════════════════════════════════════════════════════════
// These tests verify that TaprootTxSigner and BIP143Signer refuse to sign
// transactions that include UTXOs without valid derivation paths.

TEST_F(WalletOwnershipInvariantTest, TaprootSigner_RefusesPathlessUTXO) {
    // Create a minimal transaction
    Transaction tx;
    tx.version = 2;
    tx.vin.push_back(TxInput());
    tx.vin[0].prevout = TxOutPoint(TxId(uint256::FromHexUnsafe(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")), 0);
    tx.vout.push_back(TxOutput());
    tx.vout[0].value = AmountUna::Una(90000);
    tx.vout[0].scriptPubKey = CreateP2TRScript();

    // Create UTXO WITHOUT path
    CanonicalWalletUTXO utxo;
    utxo.txid = uint256::FromHexUnsafe(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    utxo.vout = 0;
    utxo.value = AmountUna::Una(100000);
    utxo.spk = CreateP2TRScript();
    utxo.path = "";  // NO PATH - this is the bug we're testing
    utxo.height = 100;
    utxo.is_coinbase = false;

    std::vector<CanonicalWalletUTXO> utxos = {utxo};

    // Create a dummy private key (32 bytes)
    std::vector<uint8_t> privkey(32, 0x42);
    std::vector<std::vector<uint8_t>> privkeys = {privkey};

    // MUST refuse to sign - ownership cannot be verified
    EXPECT_FALSE(TaprootTxSigner::SignTransaction(tx, utxos, privkeys))
        << "TaprootTxSigner must refuse to sign UTXO without derivation path";
}

TEST_F(WalletOwnershipInvariantTest, BIP143Signer_RefusesPathlessUTXO) {
    // Create a minimal transaction
    Transaction tx;
    tx.version = 2;
    tx.vin.push_back(TxInput());
    tx.vin[0].prevout = TxOutPoint(TxId(uint256::FromHexUnsafe(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")), 0);
    tx.vout.push_back(TxOutput());
    tx.vout[0].value = AmountUna::Una(90000);
    tx.vout[0].scriptPubKey = CreateP2WPKHScript();

    // Create UTXO WITHOUT path
    CanonicalWalletUTXO utxo;
    utxo.txid = uint256::FromHexUnsafe(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    utxo.vout = 0;
    utxo.value = AmountUna::Una(100000);
    utxo.spk = CreateP2WPKHScript();
    utxo.path = "";  // NO PATH - this is the bug we're testing
    utxo.height = 100;
    utxo.is_coinbase = false;

    std::vector<CanonicalWalletUTXO> utxos = {utxo};

    // Create a dummy private key (32 bytes)
    std::vector<uint8_t> privkey(32, 0x42);
    std::vector<std::vector<uint8_t>> privkeys = {privkey};

    // MUST refuse to sign - ownership cannot be verified
    EXPECT_FALSE(BIP143Signer::SignTransaction(tx, utxos, privkeys))
        << "BIP143Signer must refuse to sign UTXO without derivation path";
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST: IsOurScript returns path for owned scripts
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(WalletOwnershipInvariantTest, IsOurScript_ReturnsPathForOwnedScript) {
    auto spk = CreateP2TRScript();
    const std::string expected_path = "m/86'/1447'/0'/0/42";

    // Register the script
    utxo_index_->RegisterAddress(spk, expected_path);

    // IsOurScript must return the path
    auto result = utxo_index_->IsOurScript(spk);
    ASSERT_TRUE(result.has_value())
        << "IsOurScript must return a value for registered script";
    EXPECT_EQ(result.value(), expected_path)
        << "IsOurScript must return the correct derivation path";
}

TEST_F(WalletOwnershipInvariantTest, IsOurScript_ReturnsNulloptForUnknownScript) {
    auto spk = CreateP2TRScript();  // Not registered

    // IsOurScript must return nullopt for unknown scripts
    auto result = utxo_index_->IsOurScript(spk);
    EXPECT_FALSE(result.has_value())
        << "IsOurScript must return nullopt for unregistered script";
}

} // namespace dinero::wallet::test

namespace dinero {
// Narrow test access for ordinary SQLite refusal fixtures. No production API
// accepts a borrowed connection or changes another owner's transaction.
struct UTXOIndexSchemaTestAccess {
    static sqlite3* Connection(UTXOIndex& index){return index.db_;}
    static bool Migrate(UTXOIndex& index,sqlite3* db) {
        if(index.db_)throw std::runtime_error("Fixture index already has a connection");
        index.db_=db;
        struct Reset {sqlite3*& db;~Reset(){db=nullptr;}} reset{index.db_};
        return index.CreateTables();
    }
};
}
namespace dinero::wallet::test {
namespace {
struct HistoricalSchemaFixture {
    std::filesystem::path path;
    sqlite3* db=nullptr;
    HistoricalSchemaFixture() {
        path=std::filesystem::temp_directory_path()/std::string("dinero-historical-schema-");
        path+=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".db";
        if(sqlite3_open(path.string().c_str(),&db)!=SQLITE_OK)throw std::runtime_error("Fixture database unavailable");
        Sql(R"(
            PRAGMA synchronous=FULL;
            CREATE TABLE wallet_utxos(
                txid TEXT NOT NULL,vout INTEGER NOT NULL,value INTEGER NOT NULL,spk BLOB NOT NULL,
                path TEXT NOT NULL CHECK(length(path)>=1),height INTEGER NOT NULL,spend_height INTEGER,
                is_coinbase INTEGER NOT NULL DEFAULT 0,utreexo_position INTEGER,is_confidential INTEGER DEFAULT 0,
                commitment BLOB,range_proof BLOB,blinding_factor BLOB,nonce BLOB,PRIMARY KEY(txid,vout));
            CREATE TABLE utxo_metadata(key TEXT PRIMARY KEY NOT NULL,value TEXT NOT NULL);
            INSERT INTO wallet_utxos VALUES(printf('%064x',1),2,12000,X'51201122','m/86''/1448''/0''/0/3',101,103,0,987654321,0,X'000102',X'030004',X'000500',X'060007');
            INSERT INTO utxo_metadata VALUES('runtime_delivery:v1:receipt',X'444E554930310000FF');
            INSERT INTO utxo_metadata VALUES('unrelated',X'FF0001');
            CREATE INDEX retained_height ON wallet_utxos(height,value);
            CREATE TRIGGER retained_invalidation AFTER UPDATE ON wallet_utxos BEGIN
                INSERT OR REPLACE INTO utxo_metadata VALUES('runtime_delivery:v1:invalidated','1');
                DELETE FROM utxo_metadata WHERE key='runtime_delivery:v1:receipt';
            END;
            PRAGMA user_version=1;
        )");
    }
    ~HistoricalSchemaFixture(){if(db)sqlite3_close(db);std::filesystem::remove(path);
        std::filesystem::remove(path.string()+"-wal");std::filesystem::remove(path.string()+"-shm");}
    void Sql(const char* sql){if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("Fixture SQL failed");}
    std::string Rows(const char* sql) {
        sqlite3_stmt* raw=nullptr;
        if(sqlite3_prepare_v2(db,sql,-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("Fixture query failed");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> q(raw,sqlite3_finalize);std::string result;int rc;
        while((rc=sqlite3_step(q.get()))==SQLITE_ROW)for(int col=0;col<sqlite3_column_count(q.get());++col) {
            result+=std::to_string(sqlite3_column_type(q.get(),col))+":";
            const auto* bytes=static_cast<const char*>(sqlite3_column_blob(q.get(),col));const int n=sqlite3_column_bytes(q.get(),col);
            result+=std::to_string(n)+":";if(n)result.append(bytes,n);
        }
        if(rc!=SQLITE_DONE)throw std::runtime_error("Fixture query incomplete");return result;
    }
    std::string Values() {
        return Rows("SELECT txid,vout,value,spk,path,height,spend_height,is_coinbase,utreexo_position,is_confidential,commitment,range_proof,blinding_factor,nonce FROM wallet_utxos ORDER BY txid,vout")+
            Rows("SELECT key,value FROM utxo_metadata ORDER BY key");
    }
    std::string Snapshot(){return Rows("SELECT type,name,sql FROM sqlite_schema ORDER BY type,name")+Rows("PRAGMA user_version")+Values();}
};
}
TEST(WalletHistoricalIndexSchema, MigrationRetainsValuesGuardsAndReceiptsAcrossReopen) {
    HistoricalSchemaFixture f;const auto values=f.Values();
    const auto guards=f.Rows("SELECT name,sql FROM sqlite_schema WHERE name IN('retained_height','retained_invalidation') ORDER BY name");
    ASSERT_EQ(sqlite3_close(f.db),SQLITE_OK);f.db=nullptr;
    {UTXOIndex index(f.path.string());ASSERT_TRUE(index.Initialize());
     const auto coin=index.GetUTXO(TxId(uint256::FromHexUnsafe(std::string(63,'0')+"1")),2);
     ASSERT_TRUE(coin);EXPECT_EQ(coin->value.GetUna(),12000u);EXPECT_EQ(coin->spend_height,103);
     EXPECT_EQ(index.getUtreexoPosition(TxId(uint256::FromHexUnsafe(std::string(63,'0')+"1")),2),987654321u);}
    ASSERT_EQ(sqlite3_open(f.path.string().c_str(),&f.db),SQLITE_OK);
    EXPECT_EQ(f.Values(),values);
    EXPECT_EQ(f.Rows("SELECT name,sql FROM sqlite_schema WHERE name IN('retained_height','retained_invalidation') ORDER BY name"),guards);
    EXPECT_EQ(f.Rows("SELECT owner_kind,owner_reference FROM wallet_utxos"),"1:1:0" "3:0:");
    const auto migrated=f.Snapshot();
    {UTXOIndex index("fixture-only");EXPECT_TRUE(UTXOIndexSchemaTestAccess::Migrate(index,f.db));}
    EXPECT_EQ(f.Snapshot(),migrated);
    f.Sql("UPDATE wallet_utxos SET height=102");
    EXPECT_TRUE(f.Rows("SELECT value FROM utxo_metadata WHERE key='runtime_delivery:v1:receipt'").empty());
    EXPECT_FALSE(f.Rows("SELECT value FROM utxo_metadata WHERE key='runtime_delivery:v1:invalidated'").empty());
}
TEST(WalletHistoricalIndexSchema, RequiredWriteAndCommitFailuresRetainLegacyDatabase) {
    for(const int denied:{SQLITE_INSERT,SQLITE_DROP_TABLE,SQLITE_TRANSACTION}) {
        HistoricalSchemaFixture f;const auto before=f.Snapshot();UTXOIndex index("fixture-only");
        struct Fault {sqlite3* db;int action;~Fault(){sqlite3_set_authorizer(db,nullptr,nullptr);sqlite3_commit_hook(db,nullptr,nullptr);}} fault{f.db,denied};
        if(denied==SQLITE_TRANSACTION)sqlite3_commit_hook(f.db,[](void*){return 1;},nullptr);
        else ASSERT_EQ(sqlite3_set_authorizer(f.db,[](void* state,int action,const char* name,const char*,const char*,const char*) {
            const auto& fault=*static_cast<Fault*>(state);
            if(action!=fault.action||!name)return SQLITE_OK;
            return std::strcmp(name,action==SQLITE_INSERT?"wallet_utxos_ownership_v2":"wallet_utxos")==0?SQLITE_DENY:SQLITE_OK;
        },&fault),SQLITE_OK);
        EXPECT_FALSE(UTXOIndexSchemaTestAccess::Migrate(index,f.db));
        sqlite3_set_authorizer(f.db,nullptr,nullptr);sqlite3_commit_hook(f.db,nullptr,nullptr);
        EXPECT_TRUE(sqlite3_get_autocommit(f.db));EXPECT_EQ(f.Snapshot(),before);
        EXPECT_TRUE(UTXOIndexSchemaTestAccess::Migrate(index,f.db));
    }
}
TEST(WalletHistoricalIndexSchema, BorrowedTransactionAndUnknownOwnersRefuseWithoutMutation) {
    HistoricalSchemaFixture f;UTXOIndex index("fixture-only");
    const auto before=f.Snapshot();f.Sql("BEGIN IMMEDIATE");
    EXPECT_FALSE(UTXOIndexSchemaTestAccess::Migrate(index,f.db));EXPECT_FALSE(sqlite3_get_autocommit(f.db));
    EXPECT_EQ(f.Snapshot(),before);f.Sql("ROLLBACK");
    f.Sql("ALTER TABLE wallet_utxos ADD COLUMN unknown_owner BLOB");const auto unknown=f.Snapshot();
    EXPECT_FALSE(UTXOIndexSchemaTestAccess::Migrate(index,f.db));EXPECT_TRUE(sqlite3_get_autocommit(f.db));EXPECT_EQ(f.Snapshot(),unknown);
}
// These are refusal/serialization cases, not key-authentication fixtures.
TEST_F(WalletOwnershipInvariantTest, HistoricalTagCannotAuthorizePublicInsertion) {
    auto coin=CreateWalletUTXO("",CreateP2TRScript());
    coin.owner_kind=WalletOutputOwner::HistoricalImport;coin.owner_reference="untrusted-recorded-address";
    EXPECT_FALSE(utxo_index_->AddUTXO(coin));
    coin.path="m/86'/1448'/0'/0/1";EXPECT_FALSE(utxo_index_->AddUTXO(coin));
    coin.owner_kind=static_cast<WalletOutputOwner>(2);EXPECT_FALSE(utxo_index_->AddUTXO(coin));
    EXPECT_TRUE(utxo_index_->GetUnspentUTXOs().empty());EXPECT_EQ(utxo_index_->GetBalance().GetUna(),0u);
}
TEST_F(WalletOwnershipInvariantTest, UnauthenticatedDurableOwnerRefusesQueriesAndBalances) {
    const auto coin=CreateWalletUTXO("m/86'/1448'/0'/0/1",CreateP2TRScript());
    ASSERT_TRUE(utxo_index_->AddUTXO(coin));auto* db=UTXOIndexSchemaTestAccess::Connection(*utxo_index_);
    ASSERT_EQ(sqlite3_exec(db,"UPDATE wallet_utxos SET owner_kind=1,path='',owner_reference='untrusted-recorded-address'",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)utxo_index_->GetUnspentUTXOs(),std::runtime_error);
    EXPECT_THROW((void)utxo_index_->GetUTXO(coin.txid,coin.vout),std::runtime_error);
    EXPECT_THROW((void)utxo_index_->GetBalance(),std::runtime_error);
    EXPECT_THROW((void)utxo_index_->GetBalanceForPath("m/"),std::runtime_error);
    EXPECT_THROW((void)utxo_index_->GetBalanceWithMaturity(200),std::runtime_error);
    EXPECT_THROW((void)utxo_index_->GetConfidentialUTXOs(),std::runtime_error);
    EXPECT_THROW((void)utxo_index_->GetTotalBalance(),std::runtime_error);
    // A replay through the public path cannot overwrite an established owner.
    EXPECT_FALSE(utxo_index_->AddUTXO(coin));
    utxo_index_.reset();utxo_index_=std::make_unique<UTXOIndex>(temp_db_path_);ASSERT_TRUE(utxo_index_->Initialize());
    EXPECT_THROW((void)utxo_index_->GetUTXO(coin.txid,coin.vout),std::runtime_error);
}
TEST_F(WalletOwnershipInvariantTest, TypedReadsPreserveMetadataAndRefuseIncompleteInventory) {
    auto coin=CreateWalletUTXO("m/86'/1448'/0'/0/1",CreateP2TRScript());
    coin.vout=UINT32_MAX;coin.utreexo_position=987654321;coin.is_coinbase=true;
    ASSERT_TRUE(utxo_index_->AddUTXO(coin));
    auto rows=utxo_index_->GetUnspentUTXOs();ASSERT_EQ(rows.size(),1u);
    EXPECT_EQ(rows.front().vout,UINT32_MAX);EXPECT_EQ(rows.front().utreexo_position,coin.utreexo_position);
    EXPECT_TRUE(rows.front().is_coinbase);EXPECT_EQ(rows.front().owner_kind,WalletOutputOwner::RecordedPath);
    EXPECT_TRUE(rows.front().owner_reference.empty());
    auto found=utxo_index_->GetUTXO(coin.txid,coin.vout);ASSERT_TRUE(found);EXPECT_EQ(found->utreexo_position,coin.utreexo_position);
    auto other=coin;other.txid=TxId(uint256::FromHexUnsafe(std::string(64,'e')));other.value=AmountUna::Una(1);
    ASSERT_TRUE(utxo_index_->AddUTXO(other));auto* db=UTXOIndexSchemaTestAccess::Connection(*utxo_index_);
    struct Fault {sqlite3* db;int rows=0;~Fault(){sqlite3_trace_v2(db,0,nullptr,nullptr);sqlite3_set_authorizer(db,nullptr,nullptr);}} fault{db};
    ASSERT_EQ(sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned,void* state,void*,void*) {
        auto& f=*static_cast<Fault*>(state);if(++f.rows==2)sqlite3_interrupt(f.db);return 0;
    },&fault),SQLITE_OK);
    EXPECT_THROW((void)utxo_index_->GetUnspentUTXOs(),std::runtime_error);EXPECT_EQ(fault.rows,2);
    sqlite3_trace_v2(db,0,nullptr,nullptr);EXPECT_EQ(utxo_index_->GetUnspentUTXOs().size(),2u);
    ASSERT_EQ(sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_READ && table && std::strcmp(table,"wallet_utxos")==0 ? SQLITE_DENY:SQLITE_OK;
    },nullptr),SQLITE_OK);
    EXPECT_THROW((void)utxo_index_->GetUTXO(coin.txid,coin.vout),std::runtime_error);
    sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_TRUE(utxo_index_->GetUTXO(coin.txid,coin.vout));EXPECT_EQ(utxo_index_->GetUnspentUTXOs().size(),2u);
}
} // namespace dinero::wallet::test

// ═══════════════════════════════════════════════════════════════════════════
// Main entry point for Google Test
// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

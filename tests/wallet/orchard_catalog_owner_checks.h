#pragma once
#include "wallet/orchard_account_catalog.h"
#include <openssl/hmac.h>
namespace dinero {
struct WalletOrchardCatalogTestAccess {
    static bool Initial(WalletManager& w,const std::vector<uint8_t>& seed,const std::string& owner){
        return w.storeMasterSeedOwned(seed,"",false,&owner,WalletManager::InitialSeedKind::Generated);
    }
    static bool Rebind(WalletManager& w,const std::vector<uint8_t>& seed){return w.storeMasterSeedOwned(seed,"",false,nullptr);}
    static std::string LegacyOwner(WalletManager& w,const std::vector<uint8_t>& seed,const std::string& owner){
        const std::string domain="Dinero WalletManager initial PQ owner v1";
        std::vector<uint8_t> material(domain.begin(),domain.end());material.insert(material.end(),seed.begin(),seed.end());
        std::array<uint8_t,32> key{};::SHA256(material.data(),material.size(),key.data());OPENSSL_cleanse(material.data(),material.size());
        struct Secret{std::string s;~Secret(){if(!s.empty())OPENSSL_cleanse(s.data(),s.size());}} binary{std::string(reinterpret_cast<const char*>(key.data()),key.size())},plain;
        OPENSSL_cleanse(key.data(),key.size());std::vector<uint8_t> bytes;
        if(!util::unhex(owner,bytes))throw std::runtime_error("catalog fixture owner encoding");
        plain.s=w.decryptData(std::string(bytes.begin(),bytes.end()),binary.s);
        if(plain.s.substr(0,5)!="DNI02")throw std::runtime_error("catalog fixture owner version");
        plain.s.replace(0,5,"DNI01");const auto cipher=w.encryptData(plain.s,binary.s);
        return util::hex(std::vector<uint8_t>(cipher.begin(),cipher.end()));
    }
};
}
namespace {
class OrchardCatalogOwner : public ::testing::Test {
protected:
    using Catalog=dinero::wallet::OrchardAccountCatalog;
    std::filesystem::path root;std::unique_ptr<dinero::WalletManager> wallet;
    struct Seed { std::vector<uint8_t> value;~Seed(){if(!value.empty())OPENSSL_cleanse(value.data(),value.size());} } seed;
    static constexpr const char* setting="orchard_account_catalog_v1";
    static void sql(sqlite3* db,const std::string& text){
        char* err=nullptr;const int rc=sqlite3_exec(db,text.c_str(),nullptr,nullptr,&err);std::string error=err?err:"";sqlite3_free(err);
        if(rc!=SQLITE_OK)throw std::runtime_error(error);
    }
    sqlite3* db(){return wallet->getCurrentDatabase();}
    void SetUp() override {
        char tmp[]="/tmp/dinero-orchard-catalog-XXXXXX";ASSERT_NE(mkdtemp(tmp),nullptr);root=tmp;
        wallet=std::make_unique<dinero::WalletManager>(root/"node");wallet->create("owner");
        auto key=wallet->GetMasterSeed();ASSERT_TRUE(key);seed.value=std::move(*key);ASSERT_EQ(seed.value.size(),64u);
    }
    void TearDown() override {wallet.reset();if(!root.empty())std::filesystem::remove_all(root);}
    std::optional<Catalog::Snapshot> read(){
        auto lease=wallet->AcquireDatabaseLease();sql(db(),"BEGIN");
        try{auto result=Catalog::Read(db(),seed.value);sql(db(),"COMMIT");return result;}
        catch(...){sql(db(),"ROLLBACK");throw;}
    }
    std::vector<std::string> rows(){
        std::vector<std::string> out;
        for(const char* query:{"SELECT quote(key)||':'||quote(value) FROM settings ORDER BY key","SELECT quote(encrypted_seed)||':'||quote(encryption_version) FROM hd_seeds ORDER BY id"}){
            sqlite3_stmt* raw=nullptr;if(sqlite3_prepare_v2(db(),query,-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("catalog fixture rows");
            std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> q(raw,sqlite3_finalize);int rc;
            while((rc=sqlite3_step(raw))==SQLITE_ROW){const auto* text=reinterpret_cast<const char*>(sqlite3_column_text(raw,0));if(!text)throw std::runtime_error("catalog fixture text");out.emplace_back(text,sqlite3_column_bytes(raw,0));}
            if(rc!=SQLITE_DONE)throw std::runtime_error("catalog fixture EOF");
        }return out;
    }
    static void retag(std::vector<uint8_t>& bytes,const std::vector<uint8_t>& key){
        if(bytes.size()<32)throw std::runtime_error("catalog fixture packet");
        const std::string domain="Dinero Orchard account catalog authentication v1";
        std::vector<uint8_t> material(domain.begin(),domain.end());material.push_back(0);material.insert(material.end(),bytes.begin(),bytes.end()-32);
        unsigned size=0;if(!HMAC(EVP_sha256(),key.data(),static_cast<int>(key.size()),material.data(),material.size(),bytes.data()+bytes.size()-32,&size)||size!=32)throw std::runtime_error("catalog fixture tag");
    }
};
TEST_F(OrchardCatalogOwner, GeneratedRecoveryAndEncryptedReopenPreserveOwners){
    auto original=read();ASSERT_TRUE(original);EXPECT_TRUE(original->generated);EXPECT_EQ(original->revision,1u);EXPECT_TRUE(original->accounts.empty());
    const auto catalog=wallet->getSetting(setting),owner=wallet->getSetting("wallet_initial_owner_v1");
    wallet->encryptWallet("catalog-password");wallet->unlockWallet("catalog-password",0);const auto pq=wallet->GetV7PqMasterKey();ASSERT_TRUE(pq);
    wallet->open("owner");wallet->unlockWallet("catalog-password",0);EXPECT_EQ(read(),original);EXPECT_EQ(wallet->GetV7PqMasterKey(),pq);EXPECT_EQ(wallet->getSetting(setting),catalog);EXPECT_EQ(wallet->getSetting("wallet_initial_owner_v1"),owner);
    wallet->createFromBip39("recovered","abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about","");
    auto recovered_seed=wallet->GetMasterSeed();ASSERT_TRUE(recovered_seed);OPENSSL_cleanse(seed.value.data(),seed.value.size());seed.value=std::move(*recovered_seed);
    auto recovered=read();ASSERT_TRUE(recovered);EXPECT_FALSE(recovered->generated);EXPECT_EQ(recovered->revision,1u);EXPECT_TRUE(recovered->accounts.empty());
    wallet->encryptWallet("recovery-password");wallet->open("recovered");wallet->unlockWallet("recovery-password",0);EXPECT_EQ(read(),recovered);EXPECT_FALSE(wallet->GetV7PqMasterKey());
}
TEST_F(OrchardCatalogOwner, MissingMalformedAndWrongSeedRefuseBeforeUnlockPublication){
    const auto good=wallet->getSetting(setting);wallet->encryptWallet("catalog-password");
    std::vector<uint8_t> bytes;ASSERT_TRUE(util::unhex(good,bytes));bytes.back()^=1;
    for(const auto& bad:{std::string("00"),util::hex(bytes),good+std::string(1,'\0')}){
        sql(db(),"UPDATE settings SET value=CAST(X'"+util::hex(std::vector<uint8_t>(bad.begin(),bad.end()))+"' AS TEXT) WHERE key='orchard_account_catalog_v1'");const auto before=rows();
    EXPECT_THROW(wallet->unlockWallet("catalog-password",0),std::runtime_error);EXPECT_TRUE(wallet->isWalletLocked());EXPECT_FALSE(wallet->GetV7PqMasterKey());EXPECT_EQ(rows(),before);
    }
    wallet->setSetting(setting,good);sql(db(),"UPDATE settings SET value=CAST(value AS BLOB) WHERE key='orchard_account_catalog_v1'");
    EXPECT_THROW(wallet->unlockWallet("catalog-password",0),std::runtime_error);EXPECT_TRUE(wallet->isWalletLocked());
    sql(db(),"DELETE FROM settings WHERE key='orchard_account_catalog_v1'");
    EXPECT_THROW(wallet->unlockWallet("catalog-password",0),std::runtime_error);EXPECT_TRUE(wallet->isWalletLocked());EXPECT_TRUE(wallet->getSetting(setting).empty());
    wallet->setSetting(setting,good);wallet->unlockWallet("catalog-password",0);ASSERT_FALSE(wallet->isWalletLocked());const auto pq=wallet->GetV7PqMasterKey();
    wallet->setSetting(setting,"00");
    EXPECT_THROW(wallet->unlockWallet("catalog-password",0),std::runtime_error);EXPECT_FALSE(wallet->isWalletLocked());EXPECT_EQ(wallet->GetV7PqMasterKey(),pq);wallet->setSetting(setting,good);
    auto wrong=seed.value;wrong[0]^=1;sql(db(),"BEGIN");
    EXPECT_THROW(Catalog::Read(db(),wrong),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db()));sql(db(),"ROLLBACK");OPENSSL_cleanse(wrong.data(),wrong.size());
}
TEST_F(OrchardCatalogOwner, IdentityAndAuthenticatedModeAreBound){
    const auto good=wallet->getSetting(setting);std::vector<uint8_t> bytes;ASSERT_TRUE(util::unhex(good,bytes));ASSERT_EQ(bytes.size(),84u);
    bytes[7]^=1;retag(bytes,seed.value);wallet->setSetting(setting,util::hex(bytes));
    EXPECT_THROW(read(),std::runtime_error);
    ASSERT_TRUE(util::unhex(good,bytes));bytes[39]=0;retag(bytes,seed.value);wallet->setSetting(setting,util::hex(bytes));
    auto valid_unknown=read();ASSERT_TRUE(valid_unknown);EXPECT_FALSE(valid_unknown->generated);
    wallet->encryptWallet("catalog-password");
    EXPECT_THROW(wallet->unlockWallet("catalog-password",0),std::runtime_error);EXPECT_TRUE(wallet->isWalletLocked());
    wallet->setSetting(setting,good);wallet->unlockWallet("catalog-password",0);wallet->lockWallet();
    sql(db(),"DELETE FROM settings WHERE key IN ('wallet_initial_owner_v1','v7_pq_master_key_encrypted')");wallet->setSetting(setting,"00");
    EXPECT_THROW(wallet->unlockWallet("catalog-password",0),std::runtime_error);EXPECT_TRUE(wallet->isWalletLocked());
    wallet->setSetting(setting,good);wallet->unlockWallet("catalog-password",0);EXPECT_FALSE(wallet->GetV7PqMasterKey());
}
TEST_F(OrchardCatalogOwner, InitialSeedCatalogAndSealedOwnerRollbackTogether){
    const auto owner=wallet->getSetting("wallet_initial_owner_v1"),catalog=wallet->getSetting(setting);const auto before=rows();
    EXPECT_FALSE(dinero::WalletOrchardCatalogTestAccess::Initial(*wallet,seed.value,owner));EXPECT_EQ(rows(),before);
    sql(db(),"BEGIN IMMEDIATE");EXPECT_FALSE(dinero::WalletOrchardCatalogTestAccess::Initial(*wallet,seed.value,owner));EXPECT_FALSE(sqlite3_get_autocommit(db()));sql(db(),"ROLLBACK");EXPECT_EQ(rows(),before);
    sql(db(),"DELETE FROM hd_seeds; DELETE FROM settings WHERE key IN ('wallet_initial_owner_v1','orchard_account_catalog_v1')");const auto empty=rows();
    for(const char* trigger:{"CREATE TRIGGER refuse_catalog BEFORE INSERT ON settings WHEN NEW.key='orchard_account_catalog_v1' BEGIN SELECT RAISE(ABORT,'catalog write'); END","CREATE TRIGGER refuse_catalog BEFORE INSERT ON hd_seeds BEGIN SELECT RAISE(ABORT,'seed write'); END"}){
        sql(db(),trigger);EXPECT_FALSE(dinero::WalletOrchardCatalogTestAccess::Initial(*wallet,seed.value,owner));EXPECT_EQ(rows(),empty);EXPECT_EQ(wallet->GetMasterSeed(),std::optional<std::vector<uint8_t>>(seed.value));sql(db(),"DROP TRIGGER refuse_catalog");
    }
    bool commit_seen=false;sqlite3_commit_hook(db(),[](void* p){*static_cast<bool*>(p)=true;return 1;},&commit_seen);
    EXPECT_FALSE(dinero::WalletOrchardCatalogTestAccess::Initial(*wallet,seed.value,owner));sqlite3_commit_hook(db(),nullptr,nullptr);EXPECT_TRUE(commit_seen);EXPECT_EQ(rows(),empty);EXPECT_TRUE(sqlite3_get_autocommit(db()));
    ASSERT_TRUE(dinero::WalletOrchardCatalogTestAccess::Initial(*wallet,seed.value,owner));EXPECT_EQ(wallet->getSetting(setting),catalog);EXPECT_EQ(wallet->getSetting("wallet_initial_owner_v1"),owner);ASSERT_TRUE(read());
    wallet->open("owner");EXPECT_EQ(wallet->GetMasterSeed(),std::optional<std::vector<uint8_t>>(seed.value));wallet->encryptWallet("catalog-password");
    EXPECT_NO_THROW(wallet->unlockWallet("catalog-password",0));EXPECT_TRUE(wallet->GetV7PqMasterKey());
}
TEST_F(OrchardCatalogOwner, ReadErrorsAndCallerTransactionPreserveState){
    const auto original=read();const auto before=rows();
    EXPECT_THROW(Catalog::Read(db(),seed.value),std::runtime_error);
    sql(db(),"BEGIN IMMEDIATE");const auto changes=sqlite3_total_changes(db());
    sqlite3_set_authorizer(db(),[](void*,int action,const char* table,const char*,const char*,const char*){return action==SQLITE_READ&&table&&std::strcmp(table,"settings")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(Catalog::Read(db(),seed.value),std::runtime_error);sqlite3_set_authorizer(db(),nullptr,nullptr);EXPECT_FALSE(sqlite3_get_autocommit(db()));
    struct Interrupt{sqlite3* db;bool fired=false;};Interrupt state{db()};
    sqlite3_trace_v2(db(),SQLITE_TRACE_ROW,[](unsigned,void* p,void* raw,void*){
        auto& s=*static_cast<Interrupt*>(p);const char* text=sqlite3_sql(static_cast<sqlite3_stmt*>(raw));
        if(!s.fired&&text&&std::strstr(text,"SELECT value FROM settings WHERE key=?")){s.fired=true;sqlite3_interrupt(s.db);}return 0;
    },&state);
    EXPECT_THROW(Catalog::Read(db(),seed.value),std::runtime_error);sqlite3_trace_v2(db(),0,nullptr,nullptr);EXPECT_TRUE(state.fired);EXPECT_FALSE(sqlite3_get_autocommit(db()));
    EXPECT_EQ(Catalog::Read(db(),seed.value),original);EXPECT_EQ(sqlite3_total_changes(db()),changes);sql(db(),"ROLLBACK");EXPECT_EQ(rows(),before);
}
TEST_F(OrchardCatalogOwner, LegacyOwnersAndSameSeedRebindNeverInitializeCatalog){
    const auto catalog=wallet->getSetting(setting);const auto before=rows();auto changed=seed.value;changed[0]^=1;
    EXPECT_FALSE(dinero::WalletOrchardCatalogTestAccess::Rebind(*wallet,changed));OPENSSL_cleanse(changed.data(),changed.size());EXPECT_EQ(rows(),before);
    ASSERT_TRUE(dinero::WalletOrchardCatalogTestAccess::Rebind(*wallet,seed.value));EXPECT_EQ(wallet->getSetting(setting),catalog);ASSERT_TRUE(read());
    const auto legacy=dinero::WalletOrchardCatalogTestAccess::LegacyOwner(*wallet,seed.value,wallet->getSetting("wallet_initial_owner_v1"));wallet->setSetting("wallet_initial_owner_v1",legacy);sql(db(),"DELETE FROM settings WHERE key='orchard_account_catalog_v1'");EXPECT_FALSE(read());
    wallet->encryptWallet("catalog-password");wallet->unlockWallet("catalog-password",0);auto pq=wallet->GetV7PqMasterKey();ASSERT_TRUE(pq);EXPECT_TRUE(wallet->getSetting(setting).empty());
    wallet->open("owner");wallet->unlockWallet("catalog-password",0);EXPECT_EQ(wallet->GetV7PqMasterKey(),pq);EXPECT_TRUE(wallet->getSetting(setting).empty());EXPECT_EQ(wallet->getSetting("wallet_initial_owner_v1"),legacy);
}
TEST_F(OrchardCatalogOwner, AuthenticatedPayloadBoundsAndCanonicalEncoding){
    const auto original=wallet->getSetting(setting);std::vector<uint8_t> valid;ASSERT_TRUE(util::unhex(original,valid));valid.resize(52);valid[48]=2;
    const auto append=[&](uint32_t account){
        for(int i=0;i<4;++i)valid.push_back(static_cast<uint8_t>(account>>(8*i)));
        valid.push_back(2);valid.insert(valid.end(),32,0);valid.back()=1;
        for(uint32_t value:{uint32_t(102),uint32_t(1)})for(int i=0;i<4;++i)valid.push_back(static_cast<uint8_t>(value>>(8*i)));
    };append(3);append(17);valid.insert(valid.end(),32,0);retag(valid,seed.value);wallet->setSetting(setting,util::hex(valid));
    auto decoded=read();ASSERT_TRUE(decoded);ASSERT_EQ(decoded->accounts.size(),2u);EXPECT_EQ(decoded->accounts[0].account,3u);EXPECT_EQ(decoded->accounts[1].account,17u);
    const auto refuse=[&](std::vector<uint8_t> bad){retag(bad,seed.value);wallet->setSetting(setting,util::hex(bad));const auto before=rows();
    EXPECT_THROW(read(),std::runtime_error);EXPECT_EQ(rows(),before);};
    auto bad=valid;bad[39]=2;refuse(bad);bad=valid;bad[39]=0;refuse(bad); // unknown inventory cannot claim enrolled entries
    bad=valid;std::fill(bad.begin()+40,bad.begin()+48,0);refuse(bad);bad=valid;bad[47]=0x80;refuse(bad);
    bad=valid;bad[48]=1;bad[49]=4;refuse(bad); // authenticated count1025 refuses before allocation
    bad=valid;bad.insert(bad.end()-32,0);refuse(bad);
    bad=valid;bad[55]=0x80;refuse(bad);bad=valid;bad[97]=3;refuse(bad); // invalid scalar index and duplicate account
    bad=valid;bad[56]=3;refuse(bad);bad=valid;std::fill(bad.begin()+57,bad.begin()+89,0);refuse(bad);
    bad=valid;std::fill(bad.begin()+89,bad.begin()+93,0);refuse(bad);bad=valid;std::fill(bad.begin()+93,bad.begin()+97,0xff);refuse(bad);
    std::string noncanonical=original;auto letter=noncanonical.find_first_of("abcdef");ASSERT_NE(letter,std::string::npos);noncanonical[letter]-=('a'-'A');wallet->setSetting(setting,noncanonical);
    EXPECT_THROW(read(),std::runtime_error);
    wallet->setSetting(setting,original);auto restored=read();ASSERT_TRUE(restored);EXPECT_TRUE(restored->accounts.empty());
}
} // namespace

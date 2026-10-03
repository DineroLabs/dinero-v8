#include <gtest/gtest.h>
#include "common/test_logger.h"
#include "mining/block_assembler.h"
#include "daemon/daemon_context.h"
#include "daemon/services/config_service.h"
#include "daemon/services/wallet_service.h"
#include "rpc/rpc_registry.h"
#include "wallet/utxo_index.h"
#include "wallet/v7_p2mr_store.h"
#include "consensus/pq/ml_dsa_65.h"
#include "util/hex.h"
#include <sqlite3.h>
#include <filesystem>
#include <unistd.h>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>

void registerV7PqWalletMethods();
namespace {
class PqRpcOwner : public ::testing::Test {
protected:
    dinero::NullLogger logger;
    DaemonContext daemon;
    std::shared_ptr<dinero::WalletService> service;
    std::unique_ptr<dinero::UTXOIndex> index;
    std::filesystem::path root;
    ExecutionContext ctx;
    void SetUp() override {
        char path[]="/tmp/dinero-pq-rpc-owner-XXXXXX";
        ASSERT_NE(mkdtemp(path),nullptr);root=path;
        auto config=std::make_shared<dinero::ConfigService>();
        config->Set("datadir",root.string());daemon.config=config;daemon.logger_interface=&logger;
        service=std::make_shared<dinero::WalletService>();daemon.wallet=service;
        ASSERT_TRUE(service->Init(daemon));
        index=std::make_unique<dinero::UTXOIndex>((root/"index.db").string());
        auto& w=service->get();w.setUTXOIndex(index.get());
        w.create("other");w.create("owner");w.encryptWallet("owner-password");w.unlockWallet("owner-password",0);
        ctx.daemon=&daemon;ctx.walletName="owner";registerV7PqWalletMethods();
    }
    void TearDown() override {
        service->get().setUTXOIndex(nullptr);service->Stop();index.reset();
        std::filesystem::remove_all(root);
    }
    din::Json call(const char* method,const din::Json& params=din::Json(Json::objectValue)) {
        auto* handler=g_rpcRegistry.lookup(method);if(!handler)throw std::runtime_error("Missing actual PQ adapter");
        return (*handler)(ctx,params);
    }
    static bool ok(const din::Json& r) {return !r.isMember("error") || r["error"].asString().empty();}
    din::Json imported() {
        din::Json p;p["seed_hex"]=std::string(64,'7');p["derivation_path"]="external:known-seed";p["hrp"]="rdin";return p;
    }
    din::Json address_params(const std::string& address) {
        din::Json p;p["address"]=address;p["sighash_hex"]=std::string(64,'4');return p;
    }
};
struct OnOpen {
    static thread_local OnOpen* active;
    dinero::WalletManager* wallet=nullptr;
    bool fired=false,lock_refused=false,selection_refused=false;
    static int extension(sqlite3*,char**,const sqlite3_api_routines*) {
        auto* s=active;if(!s || s->fired)return SQLITE_OK;s->fired=true;
        try{s->wallet->lockWallet();}catch(const std::logic_error&){s->lock_refused=true;}
        try{s->wallet->open("owner");}catch(const std::logic_error&){s->selection_refused=true;}
        return SQLITE_OK;
    }
    explicit OnOpen(dinero::WalletManager& w):wallet(&w) {
        active=this;if(sqlite3_auto_extension(reinterpret_cast<void(*)()>(extension))!=SQLITE_OK)throw std::runtime_error("extension");
    }
    ~OnOpen(){sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(extension));active=nullptr;}
};
thread_local OnOpen* OnOpen::active=nullptr;
TEST_F(PqRpcOwner, ActualAdaptersRoundTripAndSelection) {
    auto generated=call("wallet.getnewp2mraddress");ASSERT_TRUE(ok(generated));
    auto imported_result=call("wallet.importp2mrseed",imported());ASSERT_TRUE(ok(imported_result));
    const auto address=imported_result["address"].asString();
    auto list=call("wallet.listp2mraddresses");ASSERT_TRUE(ok(list));EXPECT_EQ(list["addresses"].size(),2u);
    auto exported=call("wallet.exportp2mrseed",address_params(address));ASSERT_TRUE(ok(exported));EXPECT_EQ(exported["seed_hex"].asString(),std::string(64,'7'));
    auto signed_result=call("wallet.signp2mr",address_params(address));ASSERT_TRUE(ok(signed_result));
    std::vector<uint8_t> pub,sig,hash;ASSERT_TRUE(util::unhex(signed_result["pubkey_hex"].asString(),pub));ASSERT_TRUE(util::unhex(signed_result["signature_hex"].asString(),sig));ASSERT_TRUE(util::unhex(std::string(64,'4'),hash));
    EXPECT_TRUE(dinero::consensus::pq::ml_dsa_65::Verify(hash.data(),hash.size(),sig.data(),sig.size(),pub.data(),pub.size()));
    ctx.walletName="other";
    for(auto name:{"wallet.getnewp2mraddress","wallet.listp2mraddresses","wallet.signp2mr","wallet.exportp2mrseed","wallet.importp2mrseed"}) {
        auto p=std::string(name)=="wallet.importp2mrseed" ? imported() : address_params(address);
        p["seed_hex"]=std::string(64,'8');p["derivation_path"]="external:wrong-selection";p["address_index"]=83;
        EXPECT_FALSE(ok(call(name,p))) << name;
    }
    EXPECT_EQ(service->get().getCurrentWalletName(),"owner");ctx.walletName="owner";
    service->get().lockWallet();EXPECT_FALSE(ok(call("wallet.listp2mraddresses")));
    service->get().unlockWallet("owner-password",0);EXPECT_TRUE(ok(call("wallet.listp2mraddresses")));
}
TEST_F(PqRpcOwner, SessionPinnedAcrossEveryAdapter) {
    auto r=call("wallet.importp2mrseed",imported());ASSERT_TRUE(ok(r));auto address=r["address"].asString();
    for(auto name:{"wallet.getnewp2mraddress","wallet.listp2mraddresses","wallet.signp2mr","wallet.exportp2mrseed","wallet.importp2mrseed"}) {
        auto& w=service->get();w.open("owner");w.unlockWallet("owner-password",0);
        din::Json p=std::string(name)=="wallet.importp2mrseed" ? imported() : address_params(address);
        p["seed_hex"]=std::string(64,'8');p["derivation_path"]="external:second";p["address_index"]=9;
        OnOpen hook(w);r=call(name,p);
        EXPECT_TRUE(hook.fired);EXPECT_TRUE(hook.lock_refused)<<name;EXPECT_TRUE(hook.selection_refused)<<name;
        EXPECT_EQ(w.getCurrentWalletName(),"owner");EXPECT_FALSE(w.isWalletLocked());EXPECT_TRUE(ok(r))<<name;
    }
    service->get().lockWallet();EXPECT_TRUE(service->get().isWalletLocked());
    service->get().open("other");EXPECT_EQ(service->get().getCurrentWalletName(),"other");
}
TEST_F(PqRpcOwner, BorrowedTransactionAndMissingStoreRefuse) {
    auto r=call("wallet.importp2mrseed",imported());ASSERT_TRUE(ok(r));auto p=address_params(r["address"].asString());
    auto& w=service->get();auto* db=w.getCurrentDatabase();ASSERT_EQ(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_FALSE(ok(call("wallet.signp2mr",p)));EXPECT_EQ(sqlite3_get_autocommit(db),0);
    ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);EXPECT_TRUE(ok(call("wallet.signp2mr",p)));
    auto path=std::filesystem::path(w.GetV7P2MRStorePath());auto held=path;held+=".held";std::filesystem::rename(path,held);
    for(auto name:{"wallet.signp2mr","wallet.exportp2mrseed","wallet.listp2mraddresses"}) {
        EXPECT_FALSE(ok(call(name,p)));EXPECT_FALSE(std::filesystem::exists(path))<<name;
    }
    if(std::filesystem::exists(path))std::filesystem::remove(path);
    std::filesystem::rename(held,path);EXPECT_TRUE(ok(call("wallet.signp2mr",p)));
}
}

#pragma once
#include "pool_config_owner_checks.h"
#include "rpc/methods_pool.h"
#include "rpc/rpc_registry.h"
#include "primitives/amount.h"
namespace pool_payment_funding_checks {
using namespace dinero::pool;
using pool_config_owner_checks::Fixture;
using pool_config_owner_checks::Settings;
using pool_orphan_accounting_checks::Sql;
using pool_orphan_accounting_checks::Read;
using pool_orphan_accounting_checks::Rows;
PoolPaymentFunding Policy() {return {"explicit-funding-wallet",7,12345};}
TEST(PoolPaymentFunding, ExplicitPolicyRoundTripAndClearWithoutDefaults) {
    Fixture f;auto cfg=f.manager->getConfig();EXPECT_FALSE(cfg.payment_funding);
    const auto ordinary=f.state();cfg.payment_funding=Policy();
    ASSERT_TRUE(f.manager->setConfig(cfg));EXPECT_EQ(f.manager->getConfig().payment_funding,cfg.payment_funding);
    EXPECT_EQ(f.manager->getDatabase().getConfig().payment_funding,cfg.payment_funding);EXPECT_EQ(f.state(),ordinary);
    const auto before=Settings(f.raw());f.manager.reset();f.open();
    EXPECT_EQ(f.manager->getConfig().payment_funding,cfg.payment_funding);EXPECT_EQ(Settings(f.raw()),before);
    cfg.payment_funding->fee_rate_hint=0;cfg.payment_funding->maximum_fee_una=0;
    ASSERT_TRUE(f.manager->setConfig(cfg));EXPECT_EQ(f.manager->getDatabase().getConfig().payment_funding,cfg.payment_funding);
    cfg.payment_funding.reset();ASSERT_TRUE(f.manager->setConfig(cfg));EXPECT_FALSE(f.manager->getDatabase().getConfig().payment_funding);
    EXPECT_EQ(Read(f.raw(),"SELECT value FROM config WHERE key='payment_funding_state'"),(Rows{{"3:unset"}}));
    EXPECT_EQ(Read(f.raw(),"SELECT value FROM config WHERE key='payment_funding_wallet'"),(Rows{{"3:"}}));
    f.manager.reset();f.open();EXPECT_FALSE(f.manager->getConfig().payment_funding);EXPECT_EQ(f.state(),ordinary);
}
TEST(PoolPaymentFunding, PartialMalformedAndIncompleteSettingsRefuse) {
    for(const char* change:{"DELETE FROM config WHERE key='payment_funding_wallet'","UPDATE config SET value='unknown' WHERE key='payment_funding_state'","UPDATE config SET value='' WHERE key='payment_funding_wallet'","UPDATE config SET value=X'61' WHERE key='payment_funding_wallet'","UPDATE config SET value=CAST(X'610062' AS TEXT) WHERE key='payment_funding_wallet'","UPDATE config SET value='-1' WHERE key='payment_fee_rate_hint'","UPDATE config SET value='1.5' WHERE key='payment_fee_rate_hint'","UPDATE config SET value='18446744073709551615' WHERE key='payment_maximum_fee_una'","UPDATE config SET value='unset' WHERE key='payment_funding_state'"}) {
        Fixture f;auto cfg=f.manager->getConfig();cfg.payment_funding=Policy();ASSERT_TRUE(f.manager->setConfig(cfg));
        Sql(f.raw(),change);const auto before=Settings(f.raw());
        EXPECT_THROW(f.manager->getDatabase().getConfig(),std::runtime_error);EXPECT_EQ(Settings(f.raw()),before);
        EXPECT_EQ(f.manager->getConfig().payment_funding,cfg.payment_funding);
    }
    Fixture f;auto cfg=f.manager->getConfig();cfg.payment_funding=Policy();ASSERT_TRUE(f.manager->setConfig(cfg));
    const auto before=Settings(f.raw());
    struct Interrupt {sqlite3* db;bool hit=false;};Interrupt interruption{f.raw()};
    sqlite3_trace_v2(f.raw(),SQLITE_TRACE_ROW,[](unsigned,void* raw,void* statement,void*) {
        auto& i=*static_cast<Interrupt*>(raw);auto* stmt=static_cast<sqlite3_stmt*>(statement);const auto* sql=sqlite3_sql(stmt);
        if(sql && std::strstr(sql,"SELECT key,value FROM config ORDER BY key")) {
            const auto* key=reinterpret_cast<const char*>(sqlite3_column_text(stmt,0));
            if(key && std::strcmp(key,"payment_maximum_fee_una")==0){i.hit=true;sqlite3_interrupt(i.db);}
        }
        return 0;
    },&interruption);
    EXPECT_THROW(f.manager->getDatabase().getConfig(),std::runtime_error);
    sqlite3_trace_v2(f.raw(),0,nullptr,nullptr);EXPECT_TRUE(interruption.hit);EXPECT_EQ(Settings(f.raw()),before);
    EXPECT_EQ(f.manager->getDatabase().getConfig().payment_funding,cfg.payment_funding);
    // A predecessor with no funding settings remains unset, never guessed.
    Sql(f.raw(),"DELETE FROM config WHERE key IN ('payment_funding_state','payment_funding_wallet','payment_fee_rate_hint','payment_maximum_fee_una')");
    EXPECT_FALSE(f.manager->getDatabase().getConfig().payment_funding);
}
TEST(PoolPaymentFunding, RequiredWritesCommitAndInvalidValuesPreservePolicy) {
    for(int failure=0;failure<3;++failure) {
        Fixture f;const auto old=f.manager->getConfig();auto next=old;next.payment_funding=Policy();
        const auto before=Settings(f.raw());const auto ordinary=f.state();
        if(failure==0)Sql(f.raw(),"CREATE TRIGGER refuse_fee_ceiling BEFORE UPDATE ON config WHEN NEW.key='payment_maximum_fee_una' BEGIN SELECT RAISE(ABORT,'fixture funding refusal'); END");
        struct Commit {bool called=false;};Commit commit;
        if(failure==1)sqlite3_commit_hook(f.raw(),[](void* raw){static_cast<Commit*>(raw)->called=true;return 1;},&commit);
        if(failure==2)Sql(f.raw(),"BEGIN IMMEDIATE");
        EXPECT_FALSE(f.manager->setConfig(next));sqlite3_commit_hook(f.raw(),nullptr,nullptr);
        EXPECT_EQ(f.manager->getConfig(),old);EXPECT_EQ(Settings(f.raw()),before);EXPECT_EQ(f.state(),ordinary);
        if(failure==1)EXPECT_TRUE(commit.called);
        if(failure==2){EXPECT_FALSE(sqlite3_get_autocommit(f.raw()));Sql(f.raw(),"ROLLBACK");}
        if(failure==0)Sql(f.raw(),"DROP TRIGGER refuse_fee_ceiling");
        ASSERT_TRUE(f.manager->setConfig(next));const auto stored=Settings(f.raw());
        for(int mode=0;mode<5;++mode) {
            auto invalid=next;
            if(mode==0)invalid.payment_funding->wallet_name="";
            if(mode==1)invalid.payment_funding->wallet_name=std::string("a\0b",3);
            if(mode==2)invalid.payment_funding->wallet_name=std::string(257,'a');
            if(mode==3)invalid.payment_funding->fee_rate_hint=UINT64_MAX;
            if(mode==4)invalid.payment_funding->maximum_fee_una=UINT64_MAX;
            EXPECT_FALSE(f.manager->setConfig(invalid));EXPECT_EQ(Settings(f.raw()),stored);EXPECT_EQ(f.manager->getConfig(),next);
        }
    }
}
struct RpcFixture : Fixture {
    std::shared_ptr<PoolDB> observer;
    std::shared_ptr<PoolManager> runtime;
    RpcFixture() {
        observer=std::make_shared<PoolDB>((root/"pool.sqlite").string());
        runtime=std::make_shared<PoolManager>((root/"pool.sqlite").string());
        if(!observer->initialize() || !runtime->initialize())throw std::runtime_error("fixture RPC pool initialization");
        din::rpc::registerPoolMethods();din::rpc::configurePoolRpc(observer,runtime,true,"");
    }
    ~RpcFixture(){din::rpc::configurePoolRpc(nullptr,nullptr,false,"fixture complete");runtime.reset();observer.reset();}
    din::Json call(const std::string& name,const din::Json& params=din::Json()) {
        const auto* method=g_rpcRegistry.lookup(name);if(!method)throw std::runtime_error("fixture missing real pool RPC");
        const auto handler=*method;ExecutionContext ctx;return handler(ctx,params);
    }
    din::Json request() {
        din::Json p;p["payment_funding"]["wallet"]=Policy().wallet_name;
        p["payment_funding"]["fee_rate_hint"]=din::Json::UInt64(Policy().fee_rate_hint);
        p["payment_funding"]["maximum_fee_una"]=din::Json::UInt64(Policy().maximum_fee_una);return p;
    }
};
TEST(PoolPaymentFunding, ActualRpcSetClearAndStrictPolicyFields) {
    RpcFixture f;const auto p=f.request();const auto ordinary=f.state();
    ASSERT_TRUE(f.call("pool.setconfig",p)["success"].asBool());
    EXPECT_EQ(f.runtime->getConfig().payment_funding,std::optional<PoolPaymentFunding>(Policy()));
    EXPECT_EQ(f.observer->getConfig().payment_funding,f.runtime->getConfig().payment_funding);
    EXPECT_EQ(f.call("pool.getconfig")["payment_funding"],p["payment_funding"]);const auto before=Settings(f.raw());
    for(int mode=0;mode<10;++mode) {
        auto q=p;
        if(mode==0)q["payment_funding"].removeMember("maximum_fee_una");
        if(mode==1)q["payment_funding"]["fee_rate_hint"]="7";
        if(mode==2)q["payment_funding"]["maximum_fee_una"]=12345.0;
        if(mode==3)q["payment_funding"]["fee_rate_hint"]=-1;
        if(mode==4)q["payment_funding"]["maximum_fee_una"]=din::Json::UInt64(UINT64_MAX);
        if(mode==5)q["payment_funding"]["wallet"]="";
        if(mode==6)q["payment_funding"]["wallet"]=std::string("a\0b",3);
        if(mode==7)q["payment_funding"]["wallet"]=7;
        if(mode==8)q["payment_funding"]["unknown"]=1;
        if(mode==9)q["payment_funding"]=true;
        EXPECT_TRUE(f.call("pool.setconfig",q).isMember("error"))<<mode;
        EXPECT_EQ(Settings(f.raw()),before);EXPECT_EQ(f.runtime->getConfig().payment_funding,std::optional<PoolPaymentFunding>(Policy()));
    }
    din::Json clear;clear["payment_funding"]=din::Json();ASSERT_TRUE(f.call("pool.setconfig",clear)["success"].asBool());
    EXPECT_FALSE(f.runtime->getConfig().payment_funding);EXPECT_FALSE(f.observer->getConfig().payment_funding);
    EXPECT_TRUE(f.call("pool.getconfig")["payment_funding"].isNull());EXPECT_EQ(f.state(),ordinary);
}
TEST(PoolPaymentFunding, ActualRpcCommitRefusalAndStaleUpdatePreserveLiveState) {
    RpcFixture f;const auto p=f.request();const auto initial=f.runtime->getConfig();const auto stored=Settings(f.raw());
    auto* actual=PoolOrphanAccountingTestAccess::Database(f.runtime->getDatabase());
    struct Commit {bool hit=false;};Commit commit;
    sqlite3_commit_hook(actual,[](void* raw){static_cast<Commit*>(raw)->hit=true;return 1;},&commit);
    const auto refused=f.call("pool.setconfig",p);sqlite3_commit_hook(actual,nullptr,nullptr);
    EXPECT_FALSE(refused["success"].asBool());EXPECT_TRUE(commit.hit);EXPECT_EQ(f.runtime->getConfig(),initial);EXPECT_EQ(Settings(f.raw()),stored);
    ASSERT_TRUE(f.call("pool.setconfig",p)["success"].asBool());const auto changed=f.runtime->getConfig();const auto current=Settings(f.raw());
    auto stale=initial;stale.min_payout+=1;
    EXPECT_FALSE(f.runtime->compareAndSetConfig(initial,stale));EXPECT_EQ(f.runtime->getConfig(),changed);EXPECT_EQ(Settings(f.raw()),current);
    auto next=changed;next.min_payout+=1;ASSERT_TRUE(f.runtime->compareAndSetConfig(changed,next));
    EXPECT_EQ(f.runtime->getConfig(),next);EXPECT_EQ(f.observer->getConfig(),next);
}
TEST(PoolPaymentFunding, ActualRpcMalformedOtherSettingsHaveNoEffects) {
    RpcFixture f;const auto initial=f.runtime->getConfig();const auto stored=Settings(f.raw());
    for(int mode=0;mode<9;++mode) {
        auto q=f.request();
        if(mode==0)q["payout_mode"]="unknown";
        if(mode==1)q["min_payout"]=-1;
        if(mode==2)q["min_auto_payout"]=1e300;
        if(mode==3)q["pplns_window"]=din::Json::UInt64(UINT64_MAX);
        if(mode==4)q["required_confirmations"]=din::Json::UInt64(uint64_t{UINT32_MAX}+1);
        if(mode==5)q["max_payout_retries"]=-1;
        if(mode==6)q["new_round_on_block"]="true";
        if(mode==7)q["pool_fee_percent"]="1";
        if(mode==8)q["pool_fee_percent"]=101;
        EXPECT_TRUE(f.call("pool.setconfig",q).isMember("error"))<<mode;
        EXPECT_EQ(f.runtime->getConfig(),initial);EXPECT_EQ(Settings(f.raw()),stored);
    }
}
} // namespace pool_payment_funding_checks

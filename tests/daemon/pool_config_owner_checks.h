#pragma once
#include "pool_payout_inventory_checks.h"
namespace pool_config_owner_checks {
using namespace dinero::pool;
struct Fixture : pool_calculation_inputs_checks::Fixture {
    Fixture() {if(!manager->setConfig(manager->getConfig()))throw std::runtime_error("fixture settings persistence");}
};
using pool_orphan_accounting_checks::Sql;
using pool_orphan_accounting_checks::Read;
using pool_orphan_accounting_checks::Rows;
Rows Settings(sqlite3* db) {return Read(db,"SELECT key,value FROM config ORDER BY key");}
void Equal(const PoolConfig& a,const PoolConfig& b) {
    EXPECT_EQ(a.payout_mode,b.payout_mode);EXPECT_EQ(a.pplns_window,b.pplns_window);
    EXPECT_EQ(a.pps_rate,b.pps_rate);EXPECT_EQ(a.pool_fee_percent,b.pool_fee_percent);
    EXPECT_EQ(a.pool_fee_address,b.pool_fee_address);EXPECT_EQ(a.min_payout,b.min_payout);
    EXPECT_EQ(a.min_auto_payout,b.min_auto_payout);EXPECT_EQ(a.max_payout_retries,b.max_payout_retries);
    EXPECT_EQ(a.required_confirmations,b.required_confirmations);EXPECT_EQ(a.new_round_on_block,b.new_round_on_block);
}
TEST(PoolConfigOwner, ExactValuesReopenAndUnknownSettingsPreserved) {
    Fixture f;const auto ordinary=f.state();auto cfg=f.manager->getConfig();
    cfg.payout_mode=PayoutMode::SOLO;cfg.pplns_window=123456;cfg.pps_rate=0.000000123456789012345;
    cfg.pool_fee_percent=1.23456789012345;cfg.pool_fee_address="";cfg.min_payout=INT64_MAX;
    cfg.min_auto_payout=INT64_MAX;cfg.max_payout_retries=UINT32_MAX;cfg.required_confirmations=UINT32_MAX;cfg.new_round_on_block=false;
    Sql(f.raw(),"INSERT INTO config(key,value) VALUES('future_optional','preserve')");
    ASSERT_TRUE(f.manager->setConfig(cfg));Equal(f.manager->getConfig(),cfg);Equal(f.manager->getDatabase().getConfig(),cfg);
    EXPECT_EQ(f.state(),ordinary);EXPECT_EQ(Read(f.raw(),"SELECT value FROM config WHERE key='future_optional'"),(Rows{{"3:preserve"}}));
    const auto stored=Settings(f.raw());f.manager.reset();f.open();Equal(f.manager->getConfig(),cfg);EXPECT_EQ(Settings(f.raw()),stored);
    cfg.pool_fee_percent=100;ASSERT_TRUE(f.manager->setConfig(cfg));EXPECT_EQ(f.manager->getDatabase().getConfig().pool_fee_percent,100);
    cfg.pool_fee_percent=0;ASSERT_TRUE(f.manager->setConfig(cfg));Equal(f.manager->getConfig(),cfg);
}
TEST(PoolConfigOwner, InvalidNewSettingsNeverPublishOrPersist) {
    Fixture f;const auto initial=f.manager->getConfig();const auto stored=Settings(f.raw());const auto ordinary=f.state();
    const auto refuse=[&](const PoolConfig& cfg) {EXPECT_FALSE(f.manager->setConfig(cfg));Equal(f.manager->getConfig(),initial);EXPECT_EQ(Settings(f.raw()),stored);EXPECT_FALSE(f.manager->getDatabase().updateConfig(cfg));EXPECT_EQ(Settings(f.raw()),stored);EXPECT_EQ(f.state(),ordinary);};
    for(double v:{-1.0,101.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {auto c=initial;c.pool_fee_percent=v;refuse(c);}
    for(double v:{-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {auto c=initial;c.pps_rate=v;refuse(c);}
    auto c=initial;c.payout_mode=static_cast<PayoutMode>(999);refuse(c);
    c=initial;c.pplns_window=UINT64_MAX;refuse(c);c=initial;c.min_payout=UINT64_MAX;refuse(c);
    c=initial;c.min_auto_payout=UINT64_MAX;refuse(c);c=initial;c.pool_fee_address=std::string("a\0b",3);refuse(c);
    // The prior live calculator still performs actual allocation after refusal.
    ASSERT_EQ(f.manager->processConfirmedBlocks(),1u);
}
TEST(PoolConfigOwner, RequiredWritesCommitAndBorrowedTransactionPreserveSettings) {
    for(int failure=0;failure<3;++failure) {
        Fixture f;const auto initial=f.manager->getConfig();auto cfg=initial;cfg.payout_mode=PayoutMode::SOLO;cfg.pool_fee_percent=2.25;
        const auto before=Settings(f.raw());const auto ordinary=f.state();const auto sync=Read(f.raw(),"PRAGMA synchronous");
        if(failure==0)Sql(f.raw(),"CREATE TRIGGER refuse_config BEFORE UPDATE ON config WHEN NEW.key='min_auto_payout' BEGIN SELECT RAISE(ABORT,'fixture config refusal'); END");
        struct Commit {bool called=false;};Commit commit;
        if(failure==1)sqlite3_commit_hook(f.raw(),[](void* p){static_cast<Commit*>(p)->called=true;return 1;},&commit);
        if(failure==2)Sql(f.raw(),"BEGIN IMMEDIATE; UPDATE config SET value='5' WHERE key='max_payout_retries'");
        const auto expected=Settings(f.raw());EXPECT_FALSE(f.manager->setConfig(cfg));sqlite3_commit_hook(f.raw(),nullptr,nullptr);
        Equal(f.manager->getConfig(),initial);EXPECT_EQ(Settings(f.raw()),expected);EXPECT_EQ(f.state(),ordinary);
        if(failure==1)EXPECT_TRUE(commit.called);
        if(failure==2) {EXPECT_FALSE(sqlite3_get_autocommit(f.raw()));EXPECT_EQ(f.manager->getDatabase().getConfig().max_payout_retries,5u);Sql(f.raw(),"ROLLBACK");}
        if(failure==0)Sql(f.raw(),"DROP TRIGGER refuse_config");
        EXPECT_EQ(Settings(f.raw()),before);EXPECT_EQ(Read(f.raw(),"PRAGMA synchronous"),sync);
        ASSERT_TRUE(f.manager->setConfig(cfg));Equal(f.manager->getConfig(),cfg);Equal(f.manager->getDatabase().getConfig(),cfg);
    }
}
TEST(PoolConfigOwner, MalformedRowsAndIncompleteReadsRefuseWithoutDefaults) {
    for(const char* sql:{"UPDATE config SET value='1x' WHERE key='pps_rate'","UPDATE config SET value='nan' WHERE key='pps_rate'","UPDATE config SET value='-1' WHERE key='min_payout'","UPDATE config SET value='4294967296' WHERE key='max_payout_retries'","UPDATE config SET value='UNKNOWN' WHERE key='payout_mode'","UPDATE config SET value='maybe' WHERE key='new_round_on_block'","UPDATE config SET value=X'61' WHERE key='pool_fee_address'","UPDATE config SET value=CAST(X'610062' AS TEXT) WHERE key='pool_fee_address'"}) {
        Fixture f;Sql(f.raw(),sql);const auto before=Settings(f.raw());EXPECT_THROW(f.manager->getDatabase().getConfig(),std::runtime_error);EXPECT_EQ(Settings(f.raw()),before);
    }
    Fixture f;const auto before=Settings(f.raw());const auto initial=f.manager->getConfig();
    sqlite3_set_authorizer(f.raw(),[](void*,int action,const char* table,const char*,const char*,const char*){return action==SQLITE_READ&&table&&std::strcmp(table,"config")==0?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(f.manager->getDatabase().getConfig(),std::runtime_error);sqlite3_set_authorizer(f.raw(),nullptr,nullptr);
    struct Interrupt {sqlite3* db;int rows=0;};Interrupt interrupt{f.raw()};
    sqlite3_trace_v2(f.raw(),SQLITE_TRACE_ROW,[](unsigned,void* context,void* statement,void*){auto& i=*static_cast<Interrupt*>(context);const auto* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(statement));if(sql&&std::strstr(sql,"SELECT key,value FROM config ORDER BY key")&&++i.rows==2)sqlite3_interrupt(i.db);return 0;},&interrupt);
    EXPECT_THROW(f.manager->getDatabase().getConfig(),std::runtime_error);sqlite3_trace_v2(f.raw(),0,nullptr,nullptr);EXPECT_EQ(interrupt.rows,2);
    EXPECT_EQ(Settings(f.raw()),before);Equal(f.manager->getDatabase().getConfig(),initial);
}
} // namespace pool_config_owner_checks

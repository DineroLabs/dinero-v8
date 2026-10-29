#pragma once
#include "pool_calculation_inputs_checks.h"
#include <cmath>
#include <limits>
namespace pool_calculation_arithmetic_checks {
using namespace dinero::pool;
using pool_calculation_inputs_checks::Fixture;
using pool_orphan_accounting_checks::Sql;
using pool_orphan_accounting_checks::Read;
using pool_orphan_accounting_checks::Rows;
TEST(PoolCalculationArithmetic, FiniteFormulasAndStorageBoundaryAreExplicit) {
    Fixture f;const auto before=f.state();auto config=f.config;config.pool_fee_percent=1.0;
    PayoutCalculator calc(f.manager->getDatabase(),config);
    EXPECT_EQ(calc.calculatePoolFee(10000),100u);EXPECT_EQ(calc.getDistributable(10000),9900u);
    EXPECT_DOUBLE_EQ(calc.calculateSharePercent(1,4),0.25);EXPECT_DOUBLE_EQ(calc.calculatePPSRate(4,10000),2500);
    EXPECT_DOUBLE_EQ(calc.calculateSharePercent(0,0),0);EXPECT_DOUBLE_EQ(calc.calculatePPSRate(0,10000),0);
    const auto prop=calc.calculatePROP(f.block());ASSERT_EQ(prop.size(),2u);EXPECT_EQ(prop[0].amount,2475u);EXPECT_EQ(prop[1].amount,7425u);
    const auto pplns=calc.calculatePPLNS(f.block());ASSERT_EQ(pplns.size(),2u);EXPECT_EQ(pplns[0].amount,2475u);EXPECT_EQ(pplns[1].amount,7425u);
    const auto pps=calc.calculatePPS(f.block());ASSERT_EQ(pps.size(),3u);EXPECT_EQ(pps[0].amount,99u);EXPECT_EQ(pps[1].amount,297u);EXPECT_EQ(pps[2].amount,198u);
    const auto solo=calc.calculateSOLO(f.block());ASSERT_EQ(solo.size(),1u);EXPECT_EQ(solo[0].amount,9900u);
    config.pool_fee_percent=100;PayoutCalculator all(f.manager->getDatabase(),config);
    EXPECT_EQ(all.calculatePoolFee(INT64_MAX),static_cast<uint64_t>(INT64_MAX));EXPECT_EQ(all.getDistributable(INT64_MAX),0u);
    config.pool_fee_percent=0;PayoutCalculator none(f.manager->getDatabase(),config);
    EXPECT_EQ(none.calculatePoolFee(INT64_MAX),0u);EXPECT_EQ(none.getDistributable(INT64_MAX),static_cast<uint64_t>(INT64_MAX));
    EXPECT_THROW(none.calculatePoolFee(UINT64_MAX),std::runtime_error);EXPECT_EQ(f.state(),before);
    f.manager.reset();f.open();EXPECT_EQ(f.state(),before);ASSERT_EQ(f.run(),1u);
}
TEST(PoolCalculationArithmetic, InvalidConfigurationAndHelperInputsRefuseWithoutWrites) {
    Fixture f;const auto before=f.state();
    for(double value:{-1.0,101.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto config=f.config;config.pool_fee_percent=value;
        EXPECT_THROW((void)PayoutCalculator(f.manager->getDatabase(),config),std::runtime_error);
    }
    for(double value:{-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto config=f.config;config.pps_rate=value;
        EXPECT_THROW((void)PayoutCalculator(f.manager->getDatabase(),config),std::runtime_error);
    }
    PayoutCalculator calc(f.manager->getDatabase(),f.config);
    for(double value:{-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_THROW(calc.calculateSharePercent(value,4),std::runtime_error);
        EXPECT_THROW(calc.calculateSharePercent(1,value),std::runtime_error);
        EXPECT_THROW(calc.calculatePPSRate(value,10000),std::runtime_error);
    }
    EXPECT_THROW(calc.calculateSharePercent(1,0),std::runtime_error);
    EXPECT_THROW(calc.calculateSharePercent(5,4),std::runtime_error);
    EXPECT_THROW(calc.calculatePPSRate(std::numeric_limits<double>::denorm_min(),10000),std::runtime_error);
    EXPECT_THROW(calc.calculatePPSRate(1,UINT64_MAX),std::runtime_error);
    auto config=f.config;config.payout_mode=static_cast<PayoutMode>(999);
    EXPECT_THROW(PayoutCalculator(f.manager->getDatabase(),config).calculatePayouts(f.block()),std::runtime_error);
    EXPECT_EQ(f.state(),before);
}
TEST(PoolCalculationArithmetic, AggregateOverflowAndUnrepresentablePpsRefuseAtomically) {
    {
        Fixture f;Sql(f.raw(),"UPDATE shares SET difficulty_real=1e308 WHERE share_id IN(3,9)");const auto before=f.state();
        EXPECT_THROW(f.run(),std::runtime_error);EXPECT_EQ(f.state(),before);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
    }
    for(double rate:{std::numeric_limits<double>::max(),std::ldexp(1.0,63)}) {
        Fixture f;f.config.payout_mode=PayoutMode::PPS;f.config.pps_rate=rate;const auto before=f.state();
        EXPECT_THROW(f.run(),std::runtime_error);EXPECT_EQ(f.state(),before);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
        f.config.pps_rate=100;ASSERT_EQ(f.run(),1u);
        EXPECT_EQ(Read(f.raw(),"SELECT worker_id,amount FROM payouts ORDER BY worker_id"),(Rows{{"3:a","1:100"},{"3:b","1:300"},{"3:c","1:200"}}));
    }
    Fixture f;f.config.payout_mode=PayoutMode::PPS;f.config.pps_rate=std::nextafter(std::ldexp(1.0,63),0.0);
    Sql(f.raw(),"UPDATE shares SET status=1 WHERE share_id<>3");
    PayoutCalculator calc(f.manager->getDatabase(),f.config);const auto before=f.state();const auto one=calc.calculatePPS(f.block());
    ASSERT_EQ(one.size(),1u);EXPECT_EQ(one[0].amount,static_cast<uint64_t>(INT64_MAX)-1023u);EXPECT_EQ(f.state(),before);
    EXPECT_THROW(f.run(),std::runtime_error);EXPECT_EQ(f.state(),before); // Existing reward cap still applies.
}
TEST(PoolCalculationArithmetic, InconsistentContributionsAndWrappedTotalsRefuse) {
    for(const char* change:{"UPDATE round_shares SET difficulty_sum=5 WHERE round_id=29 AND worker_id='a'","UPDATE round_shares SET difficulty_sum=3 WHERE round_id=29 AND worker_id='a'"}) {
        Fixture f;f.config.payout_mode=PayoutMode::PROP;Sql(f.raw(),change);const auto before=f.state();
        EXPECT_THROW(f.run(),std::runtime_error);EXPECT_EQ(f.state(),before);EXPECT_TRUE(sqlite3_get_autocommit(f.raw()));
    }
    Fixture f;PayoutCalculator calc(f.manager->getDatabase(),f.config);const auto before=f.state();
    Payout a,b;a.wallet_address="a";b.wallet_address="b";a.amount=UINT64_MAX;b.amount=2;
    EXPECT_FALSE(calc.validatePayouts({a,b},UINT64_MAX));EXPECT_FALSE(calc.validatePayouts({a,b},10));
    a.amount=4;b.amount=6;EXPECT_TRUE(calc.validatePayouts({a,b},10));b.amount=7;EXPECT_FALSE(calc.validatePayouts({a,b},10));
    b.amount=0;EXPECT_FALSE(calc.validatePayouts({a,b},10));EXPECT_TRUE(calc.validatePayouts({},0));EXPECT_EQ(f.state(),before);
}
} // namespace pool_calculation_arithmetic_checks

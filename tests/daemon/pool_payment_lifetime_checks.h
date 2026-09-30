#pragma once
#include "pool_calculation_arithmetic_checks.h"
#include <future>
#include <atomic>
namespace pool_payment_lifetime_checks {
using namespace dinero::pool;
using pool_calculation_inputs_checks::Fixture;
using pool_orphan_accounting_checks::Sql;
using pool_orphan_accounting_checks::Read;
using pool_orphan_accounting_checks::Rows;
TEST(PoolPaymentLifetime, ProcessorRetainsDatabaseAndReleasesCallbackOnClose) {
    Fixture f;ASSERT_EQ(f.run(),1u);
    auto db=std::make_shared<PoolDB>((f.root/"pool.sqlite").string());ASSERT_TRUE(db->initialize());
    std::weak_ptr<PoolDB> weak_db=db;auto capture=std::make_shared<int>(7);std::weak_ptr<int> weak_capture=capture;
    unsigned calls=0;auto processor=std::make_unique<PayoutProcessor>(db,[capture,&calls](const std::string&,uint64_t,std::string&){++calls;return false;});
    db.reset();capture.reset();EXPECT_FALSE(weak_db.expired());EXPECT_FALSE(weak_capture.expired());
    EXPECT_EQ(processor->processPendingPayouts(),2u);EXPECT_EQ(calls,2u);
    processor->Close();EXPECT_TRUE(processor->IsClosed());EXPECT_TRUE(weak_capture.expired());EXPECT_FALSE(weak_db.expired());
    const auto after=f.state();EXPECT_THROW(processor->processPendingPayouts(),std::runtime_error);
    Payout payout;payout.amount=1;payout.wallet_address="address-a";payout.status=PayoutStatus::CONFIRMED;
    EXPECT_THROW(processor->processPayout(payout),std::runtime_error);EXPECT_EQ(payout.status,PayoutStatus::CONFIRMED);
    EXPECT_THROW(processor->retryFailedPayouts(),std::runtime_error);EXPECT_EQ(calls,2u);EXPECT_EQ(f.state(),after);
    processor->Close();processor.reset();EXPECT_TRUE(weak_db.expired());
    EXPECT_THROW((void)PayoutProcessor(nullptr,[](const std::string&,uint64_t,std::string&){return false;}),std::invalid_argument);
}
TEST(PoolPaymentLifetime, ManagerInstallationAndPermanentClosePreserveRows) {
    Fixture f;ASSERT_EQ(f.run(),1u);unsigned calls=0;
    auto capture=std::make_shared<int>(9);std::weak_ptr<int> weak=capture;
    f.manager->setPaymentCallback([capture,&calls](const std::string&,uint64_t,std::string&){++calls;return false;});capture.reset();
    EXPECT_THROW(f.manager->setPaymentCallback([](const std::string&,uint64_t,std::string&){return false;}),std::logic_error);
    EXPECT_EQ(f.manager->sendPendingPayouts(),2u);EXPECT_EQ(calls,2u);EXPECT_FALSE(weak.expired());
    f.manager->ClosePayments();EXPECT_TRUE(weak.expired());const auto before=f.state();
    EXPECT_THROW(f.manager->sendPendingPayouts(),std::runtime_error);
    EXPECT_THROW(f.manager->retryFailedPayouts(3),std::runtime_error);
    EXPECT_THROW(f.manager->setPaymentCallback([](const std::string&,uint64_t,std::string&){return false;}),std::logic_error);
    f.manager->ClosePayments();EXPECT_EQ(calls,2u);EXPECT_EQ(f.state(),before);
}
TEST(PoolPaymentLifetime, CallbackExceptionReleasesOperationWithoutAccountingWrites) {
    Fixture f;ASSERT_EQ(f.run(),1u);const auto before=f.state();unsigned calls=0;
    f.manager->setPaymentCallback([&](const std::string&,uint64_t,std::string&)->bool{++calls;throw std::runtime_error("fixture callback failure");});
    EXPECT_THROW(f.manager->sendPendingPayouts(),std::runtime_error);EXPECT_EQ(calls,1u);EXPECT_EQ(f.state(),before);
    EXPECT_NO_THROW(f.manager->ClosePayments());
    EXPECT_THROW(f.manager->sendPendingPayouts(),std::runtime_error);EXPECT_EQ(f.state(),before);
}
TEST(PoolPaymentLifetime, CloseDrainsOneHealthyControlledOperation) {
    Fixture f;ASSERT_EQ(f.run(),1u);Sql(f.raw(),"UPDATE payouts SET status=3 WHERE worker_id='b'");
    auto db=std::make_shared<PoolDB>((f.root/"pool.sqlite").string());ASSERT_TRUE(db->initialize());
    std::promise<void> entered,release;auto entered_wait=entered.get_future();auto released=release.get_future().share();std::atomic<unsigned> calls{0};
    PayoutProcessor processor(db,[&](const std::string&,uint64_t,std::string&) {
        ++calls;entered.set_value();
        if(released.wait_for(std::chrono::seconds(5))!=std::future_status::ready)throw std::runtime_error("fixture release timeout");
        return false;
    });
    auto operation=std::async(std::launch::async,[&]{return processor.processPendingPayouts();});
    if(entered_wait.wait_for(std::chrono::seconds(5))!=std::future_status::ready) {
        release.set_value();try{(void)operation.get();}catch(...){}FAIL()<<"healthy callback did not start";return;
    }
    auto close=std::async(std::launch::async,[&]{processor.Close();});
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!processor.IsClosed()&&std::chrono::steady_clock::now()<end)std::this_thread::yield();
    const bool closed=processor.IsClosed();
    const bool still_draining=close.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout;
    release.set_value(); // Always release before assertions or future destruction.
    EXPECT_TRUE(closed);EXPECT_TRUE(still_draining);EXPECT_EQ(operation.get(),1u);EXPECT_NO_THROW(close.get());
    EXPECT_EQ(calls.load(),1u);EXPECT_TRUE(processor.IsClosed());EXPECT_THROW(processor.processPendingPayouts(),std::runtime_error);
}
} // namespace pool_payment_lifetime_checks

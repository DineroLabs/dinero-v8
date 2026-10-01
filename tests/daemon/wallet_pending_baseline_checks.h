#pragma once
#include "wallet/runtime_index_delivery.h"
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
TEST(WalletPendingCanonicalBaseline, ActualAdoptionSignedPaymentAndReopenKeepExactProgress) {
    CanonicalVaultWithdrawalFixture f(1,false,CanonicalVaultWithdrawalFixture::FundingObservation::CanonicalOrigin);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        const auto origin=f.f.service->getRuntimeWalletOrigin(use->Wallet(),f.Selected().session,&index->Index());ASSERT_TRUE(origin.ok());
        ASSERT_EQ(f.f.service->adoptRuntimeWalletOrigin(use->Wallet(),index->Index(),**origin),Status::Ok);
    }
    const auto progress=[&](){auto use=WalletService::AcquireWalletUse(f.wallet);
        const auto value=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),f.Selected().session);
        OrchardAdmissionFixture::Require(bool(value));return std::make_tuple(value->cursor,value->origin_hash,value->origin_height,value->tip_hash,value->tip_height);};
    const auto before=progress();const auto [id,body]=f.Retain();EXPECT_EQ(progress(),before);
    const auto pending=f.wallet->get().getPendingPayments();ASSERT_EQ(pending.size(),1u);EXPECT_EQ(pending[0].signed_body,body.Serialize());
    f.Reopen();EXPECT_EQ(progress(),before);EXPECT_EQ(f.wallet->get().getPendingPayments()[0].signed_body,body.Serialize());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
    ASSERT_EQ(sqlite3_exec(lease->Database(),"UPDATE transactions SET label='external history change'",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW(progress(),std::runtime_error);
}
} // namespace dinero
#endif

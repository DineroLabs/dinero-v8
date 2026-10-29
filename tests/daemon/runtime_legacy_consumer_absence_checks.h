#pragma once
#include "daemon/services/chainstate_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/runtime_notification_composition.h"
#include "ipc/oracles/chain_oracle_client.h"
#include "ipc/oracles/time_oracle_client.h"
#include "ipc/oracles/transaction_oracle_client.h"

namespace dinero {
struct RuntimeLegacyConsumerAbsenceTestAccess {
    static void Init(ChainstateService& s) { s.logger_=std::make_shared<LoggerService>(""); }
    static auto Provider(ChainstateService& s) {
        auto selected=s.AcquireBlockIngressActivationLock();return s.runtime_block_notifications_;
    }
    static auto Presence(ChainstateService& s) {
        auto selected=s.AcquireBlockIngressActivationLock();
        return std::array<size_t,4>{bool(s.chain_oracle_client_),bool(s.time_oracle_client_),
                                   bool(s.transaction_oracle_client_),s.wallet_notifiers_.size()};
    }
};
namespace {
struct LegacyConsumerFixture {
    ChainstateService source;
    LegacyConsumerFixture(){RuntimeLegacyConsumerAbsenceTestAccess::Init(source);}
    auto presence(){return RuntimeLegacyConsumerAbsenceTestAccess::Presence(source);}
    auto provider(){return RuntimeLegacyConsumerAbsenceTestAccess::Provider(source);}
};
struct LegacyExtraNotifier final:WalletNotifier {
    unsigned callbacks=0;
    void onBlockConnected(const Block&,uint32_t)override{++callbacks;}
    void onBlockDisconnected(const Block&,uint32_t)override{++callbacks;}
    void onMempoolTransaction(const Transaction&)override{++callbacks;}
};
// Constructors never open sockets. The fixture never calls connect/send/start,
// and never addresses a production or installed Lightning process.
void RegisterDisconnectedOracle(ChainstateService& source,size_t which) {
    const std::string path="/unused-v8113-fixture/no-socket";
    if(which==0) {
        auto p=std::make_unique<ipc::ChainOracleClient>(path);ASSERT_FALSE(p->isConnected());source.setChainOracleClient(std::move(p));
    } else if(which==1) {
        auto p=std::make_unique<ipc::TimeOracleClient>(path);ASSERT_FALSE(p->isConnected());source.setTimeOracleClient(std::move(p));
    } else {
        auto p=std::make_shared<ipc::TransactionOracleClient>(path);ASSERT_FALSE(p->isConnected());source.setTransactionOracleClient(p);
    }
}
void RemoveOracle(ChainstateService& source,size_t which) {
    if(which==0)source.setChainOracleClient(nullptr);
    else if(which==1)source.setTimeOracleClient(nullptr);
    else source.setTransactionOracleClient(nullptr);
}
auto LegacyTypedProvider() {
    return std::make_shared<RuntimeNotificationComposition>(CompositionBindings(std::make_shared<CompositionTrace>()));
}
}
TEST(RuntimeLegacyConsumerAbsence, DisconnectedConfiguredOraclesRefuseProviderInstallation) {
    for(size_t family=0;family<3;++family) {
        LegacyConsumerFixture f;RegisterDisconnectedOracle(f.source,family);
        auto expected=std::array<size_t,4>{};expected[family]=1;ASSERT_EQ(f.presence(),expected);
        const auto provider=LegacyTypedProvider();
        EXPECT_THROW(f.source.setRuntimeBlockNotifications(provider),std::runtime_error);
        EXPECT_FALSE(f.provider());EXPECT_EQ(f.presence(),expected);
        RemoveOracle(f.source,family);EXPECT_EQ(f.presence(),(std::array<size_t,4>{}));
        EXPECT_NO_THROW(f.source.setRuntimeBlockNotifications(provider));EXPECT_EQ(f.provider(),provider);
    }
}
TEST(RuntimeLegacyConsumerAbsence, InstalledProviderRefusesLateOracleRegistration) {
    LegacyConsumerFixture f;const auto provider=LegacyTypedProvider();f.source.setRuntimeBlockNotifications(provider);
    for(size_t family=0;family<3;++family) {
        EXPECT_THROW(RegisterDisconnectedOracle(f.source,family),std::runtime_error);
        EXPECT_EQ(f.provider(),provider);EXPECT_EQ(f.presence(),(std::array<size_t,4>{}));
        EXPECT_NO_THROW(RemoveOracle(f.source,family));EXPECT_EQ(f.provider(),provider);
    }
    const auto replacement=LegacyTypedProvider();
    EXPECT_NO_THROW(f.source.setRuntimeBlockNotifications(replacement));
    EXPECT_EQ(f.provider(),replacement);
}
TEST(RuntimeLegacyConsumerAbsence, ExtraWalletRegistryMustActuallyBeEmpty) {
    LegacyExtraNotifier first,second,unknown;LegacyConsumerFixture f;
    f.source.registerWalletNotifier(&first);f.source.registerWalletNotifier(&second);
    f.source.registerWalletNotifier(&first);f.source.registerWalletNotifier(nullptr);
    EXPECT_EQ(f.presence(),(std::array<size_t,4>{0,0,0,2}));
    const auto provider=LegacyTypedProvider();
    EXPECT_THROW(f.source.setRuntimeBlockNotifications(provider),std::runtime_error);
    f.source.unregisterWalletNotifier(&unknown);f.source.unregisterWalletNotifier(nullptr);
    EXPECT_THROW(f.source.setRuntimeBlockNotifications(provider),std::runtime_error);
    f.source.unregisterWalletNotifier(&first);EXPECT_EQ(f.presence()[3],1u);
    EXPECT_THROW(f.source.setRuntimeBlockNotifications(provider),std::runtime_error);
    EXPECT_FALSE(f.provider());f.source.unregisterWalletNotifier(&second);
    EXPECT_NO_THROW(f.source.setRuntimeBlockNotifications(provider));EXPECT_EQ(f.provider(),provider);
    EXPECT_EQ(first.callbacks,0u);EXPECT_EQ(second.callbacks,0u);EXPECT_EQ(unknown.callbacks,0u);
}
TEST(RuntimeLegacyConsumerAbsence, InstalledProviderRefusesExtraWalletRegistration) {
    LegacyExtraNotifier notifier;LegacyConsumerFixture f;const auto provider=LegacyTypedProvider();
    f.source.setRuntimeBlockNotifications(provider);
    EXPECT_THROW(f.source.registerWalletNotifier(&notifier),std::runtime_error);
    EXPECT_EQ(f.provider(),provider);EXPECT_EQ(f.presence(),(std::array<size_t,4>{}));
    EXPECT_NO_THROW(f.source.registerWalletNotifier(nullptr));
    EXPECT_NO_THROW(f.source.unregisterWalletNotifier(&notifier));EXPECT_EQ(notifier.callbacks,0u);
}
TEST(RuntimeLegacyConsumerAbsence, ExplicitDetachmentRestoresLegacyRegistrationWithoutFalseAbsence) {
    LegacyExtraNotifier notifier;LegacyConsumerFixture f;const auto provider=LegacyTypedProvider();
    f.source.setRuntimeBlockNotifications(provider);f.source.setRuntimeBlockNotifications(nullptr);
    EXPECT_NO_THROW(RegisterDisconnectedOracle(f.source,2));f.source.registerWalletNotifier(&notifier);
    EXPECT_EQ(f.presence(),(std::array<size_t,4>{0,0,1,1}));
    f.source.setRuntimeBlockNotifications(nullptr);EXPECT_EQ(f.presence(),(std::array<size_t,4>{0,0,1,1}));
    EXPECT_THROW(f.source.setRuntimeBlockNotifications(provider),std::runtime_error);
    RemoveOracle(f.source,2);
    EXPECT_THROW(f.source.setRuntimeBlockNotifications(provider),std::runtime_error);
    f.source.unregisterWalletNotifier(&notifier);
    EXPECT_NO_THROW(f.source.setRuntimeBlockNotifications(provider));
    EXPECT_EQ(f.provider(),provider);EXPECT_EQ(notifier.callbacks,0u);
}
} // namespace dinero

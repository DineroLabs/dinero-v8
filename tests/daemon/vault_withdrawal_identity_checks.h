#pragma once
#include <set>
#include <limits>
namespace dinero {
namespace {
struct VaultWithdrawalIdentityFixture {
    vault::Ledger ledger;
    vault::InMemorySigningBackend backend{vault::BackendId{"withdrawal-identity-fixture"}};
    vault::WithdrawalQueue queue{&ledger,&backend};
    vault::AccountId account{"retained-account"};
    std::vector<uint8_t> destination=std::vector<uint8_t>(34,42);
    explicit VaultWithdrawalIdentityFixture(uint64_t amount=1000) {
        destination[0]=0x51;destination[1]=0x20;
        vault::OutpointId deposit;deposit.txid_raw.fill(21);
        ledger.append(vault::DepositObserved{1,1,account,deposit,amount});
        ledger.append(vault::CreditOpened{2,2,account,deposit,amount});
        ledger.append(vault::CreditSettled{3,3,account,deposit});
    }
    static vault::WithdrawalId Id(uint8_t tag) {vault::WithdrawalId id{};id.fill(tag);return id;}
    void Refuses(const std::function<void()>& operation,vault::WithdrawalQueueError::Kind kind) {
        try {operation();FAIL()<<"expected checked withdrawal refusal";}
        catch(const vault::WithdrawalQueueError& error) {EXPECT_EQ(error.kind(),kind);}
    }
    void Preserves(const vault::WithdrawalRequest& request,const vault::WithdrawalState& state,
                   const std::vector<vault::LedgerEntry>& entries) {
        ASSERT_EQ(queue.requests().size(),1u);
        const auto& retained=queue.requests().at(request.request_id);
        EXPECT_EQ(retained.request_id,request.request_id);EXPECT_EQ(retained.account,request.account);
        EXPECT_EQ(retained.amount,request.amount);EXPECT_EQ(retained.destination_script_pub_key,request.destination_script_pub_key);
        EXPECT_EQ(retained.created_at,request.created_at);EXPECT_EQ(queue.state(request.request_id),state);
        EXPECT_EQ(ledger.entries(),entries);
    }
};
}
TEST(VaultWithdrawalIdentity, DefaultIdentityAcrossIndependentQueuesAndActualServices) {
    std::set<vault::WithdrawalId> ids;
    for(unsigned owner=0;owner<4;++owner) {
        VaultWithdrawalIdentityFixture f;
        for(unsigned i=0;i<8;++i) {
            const auto id=f.queue.enqueue(f.account,1,f.destination);
            EXPECT_NE(id,vault::WithdrawalId{});EXPECT_TRUE(ids.insert(id).second);
            EXPECT_TRUE(std::holds_alternative<vault::WithdrawalPending>(f.queue.state(id)));
        }
        EXPECT_EQ(f.queue.requests().size(),8u);EXPECT_EQ(f.queue.outstandingDepth(),8);
        EXPECT_EQ(f.queue.currentOutstanding(f.account),8u);EXPECT_EQ(f.ledger.entries().size(),3u);
    }
    for(unsigned owner=0;owner<2;++owner) {
        std::array<uint8_t,32> hash{},txid{};hash.fill(18);txid.fill(static_cast<uint8_t>(owner+60));
        vault::VaultService service(std::make_unique<vault::InMemorySigningBackend>(vault::BackendId{"service-identity"}),
            vault::VaultServiceConfig{},[hash](uint64_t){return hash;},[](const auto&,uint64_t,const auto&){return true;});
        const vault::AccountId account{"service-account"};service.recordDeposit(txid,0,account,1000,100,hash);service.tipChanged(105);
        const auto entries=service.entriesSince(0);
        const auto id=service.enqueueWithdrawal(account,10,std::vector<uint8_t>{0x51});
        EXPECT_NE(id,vault::WithdrawalId{});EXPECT_TRUE(ids.insert(id).second);
        EXPECT_TRUE(std::holds_alternative<vault::WithdrawalPending>(service.withdrawalState(id)));
        EXPECT_EQ(service.entriesSince(0),entries);EXPECT_EQ(service.withdrawalQueueDepth(),1);
    }
}
TEST(VaultWithdrawalIdentity, DuplicateAndZeroPreserveExistingPayloadAndState) {
    VaultWithdrawalIdentityFixture f;const auto id=f.Id(7);f.queue.setRequestIdGenerator([id]{return id;});
    ASSERT_EQ(f.queue.enqueue(f.account,50,f.destination),id);
    const auto request=f.queue.requests().at(id);auto state=f.queue.state(id);auto entries=f.ledger.entries();
    auto destination=f.destination;destination.back()^=1;
    f.Refuses([&]{f.queue.enqueue(f.account,40,destination);},vault::WithdrawalQueueError::Kind::DUPLICATE_REQUEST);
    f.Preserves(request,state,entries);
    f.queue.setRequestIdGenerator([]{return vault::WithdrawalId{};});
    f.Refuses([&]{f.queue.enqueue(f.account,40,destination);},vault::WithdrawalQueueError::Kind::INVALID_REQUEST_ID);
    f.Preserves(request,state,entries);
    ASSERT_EQ(f.queue.processNext(),id);f.queue.markBroadcastIncluded(id,100);ASSERT_EQ(f.queue.tipChanged(101),1);
    state=f.queue.state(id);entries=f.ledger.entries();ASSERT_TRUE(std::holds_alternative<vault::WithdrawalSettledOnChain>(state));
    f.queue.setRequestIdGenerator([id]{return id;});
    f.Refuses([&]{f.queue.enqueue(f.account,40,destination);},vault::WithdrawalQueueError::Kind::DUPLICATE_REQUEST);
    f.Preserves(request,state,entries);
    VaultWithdrawalIdentityFixture failed;failed.queue.setRequestIdGenerator([id]{return id;});
    failed.queue.enqueue(failed.account,50,failed.destination);const auto failed_request=failed.queue.requests().at(id);
    failed.backend.setNextErrorTrap(vault::SigningBackendError::Kind::UNAVAILABLE);
    EXPECT_THROW(failed.queue.processNext(),vault::WithdrawalQueueError);
    const auto failed_state=failed.queue.state(id);ASSERT_TRUE(std::holds_alternative<vault::WithdrawalFailed>(failed_state));
    const auto failed_entries=failed.ledger.entries();
    failed.Refuses([&]{failed.queue.enqueue(failed.account,40,failed.destination);},vault::WithdrawalQueueError::Kind::DUPLICATE_REQUEST);
    failed.Preserves(failed_request,failed_state,failed_entries);
}
TEST(VaultWithdrawalIdentity, GeneratorFailurePublishesNoRequestOrState) {
    VaultWithdrawalIdentityFixture f;const auto entries=f.ledger.entries();
    f.queue.setRequestIdGenerator([]()->vault::WithdrawalId{throw std::runtime_error("injected identity source unavailable");});
    EXPECT_THROW(f.queue.enqueue(f.account,50,f.destination),std::runtime_error);
    EXPECT_TRUE(f.queue.requests().empty());EXPECT_EQ(f.queue.outstandingDepth(),0);EXPECT_EQ(f.queue.currentOutstanding(f.account),0u);
    EXPECT_EQ(f.ledger.entries(),entries);
    const auto id=f.Id(8);f.queue.setRequestIdGenerator([id]{return id;});ASSERT_EQ(f.queue.enqueue(f.account,50,f.destination),id);
    const auto request=f.queue.requests().at(id);const auto state=f.queue.state(id);
    f.queue.setRequestIdGenerator([]()->vault::WithdrawalId{throw std::runtime_error("injected identity source unavailable");});
    EXPECT_THROW(f.queue.enqueue(f.account,40,f.destination),std::runtime_error);f.Preserves(request,state,entries);
}
TEST(VaultWithdrawalIdentity, OutstandingCapArithmeticRefusesWithoutWrapping) {
    constexpr auto maximum=std::numeric_limits<vault::UnaAmount>::max();
    VaultWithdrawalIdentityFixture f(maximum);const auto first=f.Id(9);unsigned generations=0;
    f.queue.setRequestIdGenerator([&]{++generations;return first;});
    ASSERT_EQ(f.queue.enqueue(f.account,maximum-10,f.destination),first);
    const auto request=f.queue.requests().at(first);const auto state=f.queue.state(first);const auto entries=f.ledger.entries();
    f.Refuses([&]{f.queue.enqueue(f.account,20,f.destination);},vault::WithdrawalQueueError::Kind::PER_ACCOUNT_OUTSTANDING_EXCEEDED);
    EXPECT_EQ(generations,1u);f.Preserves(request,state,entries);EXPECT_EQ(f.queue.currentOutstanding(f.account),maximum-10);
    auto caps=vault::WithdrawalCaps::unbounded();caps.per_account_outstanding=100;f.queue.setCaps(caps);
    f.Refuses([&]{f.queue.enqueue(f.account,1,f.destination);},vault::WithdrawalQueueError::Kind::PER_ACCOUNT_OUTSTANDING_EXCEEDED);
    EXPECT_EQ(generations,1u);f.Preserves(request,state,entries);
    caps.per_account_outstanding=maximum;f.queue.setCaps(caps);const auto second=f.Id(10);f.queue.setRequestIdGenerator([&]{++generations;return second;});
    ASSERT_EQ(f.queue.enqueue(f.account,10,f.destination),second);EXPECT_EQ(f.queue.currentOutstanding(f.account),maximum);
    f.Refuses([&]{f.queue.enqueue(f.account,1,f.destination);},vault::WithdrawalQueueError::Kind::PER_ACCOUNT_OUTSTANDING_EXCEEDED);
    EXPECT_EQ(generations,2u);EXPECT_EQ(f.queue.requests().size(),2u);EXPECT_EQ(f.ledger.entries(),entries);
}
} // namespace dinero

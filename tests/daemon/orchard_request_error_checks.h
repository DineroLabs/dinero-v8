#pragma once
namespace dinero {
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
void ExactRequestError(const din::Json& result,const char* code) {
    ASSERT_TRUE(result.isObject());ASSERT_EQ(result.size(),2u);
    EXPECT_EQ(result["error"].asString(),"Orchard account delivery ownership or state mismatch");
    EXPECT_EQ(result["error_code"].asString(),code);
    EXPECT_FALSE(result.isMember("reservation_retained"));
}
}
TEST(OrchardRequestErrors, SpendRevisionConflictAndExactRetry) {
    OrchardSpendRpcFixture f;const auto request=f.RequestJson();const auto before=f.Snapshot();
    auto stale=request;stale["expected_revision"]=Json::UInt64(request["expected_revision"].asUInt64()+1);
    ExactRequestError(f.CallSpend(stale),"stale_account_revision");EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);EXPECT_FALSE(use->OrchardProofs().Query(f.Operation(1)));}
    const auto queued=f.CallSpend(request);ASSERT_FALSE(queued.isMember("error"))<<queued.toStyledString();
    const auto reserved=f.Snapshot();
    auto changed=request;changed["fee_una"]=Json::UInt64(request["fee_una"].asUInt64()+1);
    ExactRequestError(f.CallSpend(changed),"request_id_conflict");EXPECT_EQ(f.Snapshot(),reserved);
    // A known exact request takes precedence over current revision. Never
    // turn a legitimate old-revision retry into a new payment.
    EXPECT_NE(queued["account_revision"],request["expected_revision"]);
    const auto retry=f.CallSpend(request);ASSERT_FALSE(retry.isMember("error"))<<retry.toStyledString();
    EXPECT_TRUE(retry["existing_request"].asBool());EXPECT_FALSE(retry["proof_queued"].asBool());
    EXPECT_EQ(f.Snapshot(),reserved);
}
TEST(OrchardRequestErrors, ShieldRevisionConflictAndExactRetry) {
    ShieldRpcFixture f;const auto request=f.Request();const auto before=f.Snapshot();
    auto stale=request;stale["expected_revision"]=Json::UInt64(request["expected_revision"].asUInt64()+1);
    ExactRequestError(f.QueueRpc(stale),"stale_account_revision");EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.broadcasts,0u);
    {auto use=WalletService::AcquireWalletUse(f.wallet);EXPECT_FALSE(use->OrchardProofs().Query(orchard::Hash{81}));}
    const auto queued=f.QueueRpc(request);ASSERT_FALSE(queued.isMember("error"))<<queued.toStyledString();
    const auto reserved=f.Snapshot();auto changed=request;changed["fee_una"]=Json::UInt64(request["fee_una"].asUInt64()+1);
    ExactRequestError(f.QueueRpc(changed),"request_id_conflict");EXPECT_EQ(f.Snapshot(),reserved);
    EXPECT_NE(queued["account_revision"],request["expected_revision"]);
    const auto retry=f.QueueRpc(request);ASSERT_FALSE(retry.isMember("error"))<<retry.toStyledString();
    EXPECT_TRUE(retry["existing_request"].asBool());EXPECT_FALSE(retry["proof_queued"].asBool());
    EXPECT_EQ(f.Snapshot(),reserved);EXPECT_EQ(f.broadcasts,0u);
}
TEST(OrchardRequestErrors, MissingCurrentRequestAndReadFailureStayDistinct) {
    OrchardFinishRpcFixture f;const auto request=f.RequestJson();din::Json id;
    id["account"]=request["account"];id["request_id"]=request["request_id"];
    const auto before=f.Snapshot();ExactRequestError(f.Finish(id),"request_not_current");
    ExactRequestError(rpc_context_wallet_orchard_finishshield(f.RequestContext(),id),"request_not_current");
    EXPECT_EQ(f.Snapshot(),before);f.ExpectUnsubmitted();
    bool denied=false;auto* db=f.Database();
    sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char*,const char*,const char*) {
        if(action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_snapshots") {
            *static_cast<bool*>(p)=true;return SQLITE_DENY;
        }return SQLITE_OK;
    },&denied);
    const auto unavailable=f.Finish(id);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_TRUE(denied);ASSERT_TRUE(unavailable.isMember("error"));EXPECT_EQ(unavailable.size(),1u);
    EXPECT_FALSE(unavailable.isMember("error_code"));EXPECT_EQ(f.Snapshot(),before);f.ExpectUnsubmitted();
    ExactRequestError(f.Finish(id),"request_not_current");
}
#endif
} // namespace dinero

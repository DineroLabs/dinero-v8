#pragma once
#include "rpc/orchard_account_rpc.h"
namespace dinero {
namespace {
void ExpectActivationUnavailable(const din::Json& response,const char* code) {
    ASSERT_TRUE(response.isMember("error"))<<response.toStyledString();
    EXPECT_EQ(response["error_code"].asString(),code);
    for(const char* name:{"activation_state","activation_height","branch_id","tip_height","tip_hash",
                         "rule_active_at_tip","rule_active_for_next_block","next_block_height","storage_mode"})
        EXPECT_FALSE(response.isMember(name))<<name;
}
din::Json ReadActivation(DaemonContext& daemon) {
    RegisterOrchardAccountRpc();ExecutionContext ctx;ctx.daemon=&daemon;
    const auto* call=g_rpcRegistry.lookup("orchard.getactivationstatus");
    if(!call)throw std::runtime_error("activation RPC registration absent");
    return (*call)(ctx,din::obj());
}
}
TEST(OrchardActivationStatus, ParametersAndMissingOwnerNeverInventState) {
    RegisterOrchardAccountRpc();const auto* call=g_rpcRegistry.lookup("orchard.getactivationstatus");
    ASSERT_NE(call,nullptr);ExecutionContext ctx;
    auto nonempty=din::obj();nonempty["height"]=130000;
    for(const auto& params:std::vector<din::Json>{din::Json(true),din::Json(0),din::Json(""),nonempty})
        ASSERT_NO_FATAL_FAILURE(ExpectActivationUnavailable((*call)(ctx,params),"invalid_parameters"));
    for(const auto& params:std::vector<din::Json>{din::Json{},din::obj(),din::arr()})
        ASSERT_NO_FATAL_FAILURE(ExpectActivationUnavailable((*call)(ctx,params),"activation_status_unavailable"));
    DaemonContext daemon;daemon.chainstate=std::make_shared<ChainstateService>();
    ASSERT_NO_FATAL_FAILURE(ExpectActivationUnavailable(ReadActivation(daemon),"activation_status_unavailable"));
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardActivationStatus, FullBoundaryAndUnscheduledProfileRemainReadOnly) {
    CanonicalRecoveryFixture f;const auto before=f.f.db.getValidatedTip();ASSERT_TRUE(before.ok());
    const auto captured=f.f.service->getUtreexoRpcSnapshot();ASSERT_TRUE(captured.ok());
    const auto response=ReadActivation(f.context);ASSERT_FALSE(response.isMember("error"))<<response.toStyledString();
    EXPECT_EQ(response["activation_state"].asString(),"scheduled");
    EXPECT_EQ(response["tip_height"].asUInt64(),101u);EXPECT_EQ(response["tip_hash"].asString(),captured->block_hash.GetHex());
    EXPECT_EQ(response["activation_height"].asUInt64(),102u);EXPECT_EQ(response["next_block_height"].asUInt64(),102u);
    EXPECT_FALSE(response["rule_active_at_tip"].asBool());EXPECT_TRUE(response["rule_active_for_next_block"].asBool());
    EXPECT_EQ(response["storage_mode"].asString(),"full");EXPECT_TRUE(response["wallet_backend_compiled"].asBool());
    {struct Restore {ChainParams saved=Params();~Restore(){MutableParams()=saved;}} restore;
     MutableParams().orchard_activation_height=UINT32_MAX;MutableParams().orchard_branch_id=0;
     MutableParams().release_v8113_activation_height=UINT32_MAX;
     const auto unset=ReadActivation(f.context);ASSERT_FALSE(unset.isMember("error"))<<unset.toStyledString();
     EXPECT_EQ(unset["activation_state"].asString(),"unscheduled");
     EXPECT_TRUE(unset["activation_height"].isNull());EXPECT_TRUE(unset["branch_id"].isNull());
     EXPECT_FALSE(unset["rule_active_at_tip"].asBool());EXPECT_FALSE(unset["rule_active_for_next_block"].asBool());
     EXPECT_EQ(unset["tip_hash"],response["tip_hash"]);}
    EXPECT_EQ(ReadActivation(f.context),response);
    const auto after=f.f.db.getValidatedTip();ASSERT_TRUE(after.ok());EXPECT_EQ(after->hash,before->hash);EXPECT_EQ(after->height,before->height);
}
TEST(OrchardActivationStatus, DurableMismatchAndInvalidProfileReturnNoPrefix) {
    CanonicalRecoveryFixture f;const auto before=ReadActivation(f.context);ASSERT_FALSE(before.isMember("error"));
    const auto tip=f.f.db.getValidatedTip();ASSERT_TRUE(tip.ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,tip->hash,tip->height-1),Status::Ok);
    ASSERT_NO_FATAL_FAILURE(ExpectActivationUnavailable(ReadActivation(f.context),"activation_status_unavailable"));
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,tip->hash,tip->height),Status::Ok);
    EXPECT_EQ(ReadActivation(f.context),before);
    {struct Restore {ChainParams saved=Params();~Restore(){MutableParams()=saved;}} restore;
     MutableParams().orchard_branch_id=0;
     ASSERT_NO_FATAL_FAILURE(ExpectActivationUnavailable(ReadActivation(f.context),"activation_profile_invalid"));}
    EXPECT_EQ(ReadActivation(f.context),before);
    {auto selected=f.f.service->AcquireBlockIngressActivationLock();
     ASSERT_NO_FATAL_FAILURE(ExpectActivationUnavailable(ReadActivation(f.context),"activation_status_unavailable"));}
    EXPECT_EQ(ReadActivation(f.context),before);
}
TEST(OrchardActivationStatus, CompactHistoricalBoundaryActiveAndStoppedObservations) {
    {HistoricalPreparationFixture f;const auto response=ReadActivation(f.context);
     ASSERT_FALSE(response.isMember("error"))<<response.toStyledString();
     EXPECT_EQ(response["tip_height"].asUInt64(),100u);EXPECT_EQ(response["activation_state"].asString(),"scheduled");
     EXPECT_FALSE(response["rule_active_at_tip"].asBool());EXPECT_FALSE(response["rule_active_for_next_block"].asBool());
     EXPECT_EQ(response["storage_mode"].asString(),"compact");}
    for(bool boundary:{true,false}) {
        CompactServiceStartFixture f(boundary);ASSERT_TRUE(f.service->Start());
        const auto rows=f.storage->Rows();const auto archives=f.storage->ArchiveBytes();
        const auto response=ReadActivation(f.context);ASSERT_FALSE(response.isMember("error"))<<response.toStyledString();
        EXPECT_EQ(response["activation_state"].asString(),boundary?"scheduled":"active");
        EXPECT_EQ(response["rule_active_at_tip"].asBool(),!boundary);EXPECT_TRUE(response["rule_active_for_next_block"].asBool());
        EXPECT_EQ(response["storage_mode"].asString(),"compact");
        EXPECT_EQ(f.storage->Rows(),rows);EXPECT_EQ(f.storage->ArchiveBytes(),archives);
        f.service->Stop();ASSERT_NO_FATAL_FAILURE(ExpectActivationUnavailable(ReadActivation(f.context),"activation_status_unavailable"));
    }
}
#endif
}

#include <gtest/gtest.h>
#include "daemon/http_rpc_server.h"
#include "rpc/rpc_registry.h"
#include "rpc/orchard_account_rpc.h"
#include "../../qt/tests/orchard_error_envelope_adapter.h"
#include <sstream>
struct HttpRpcRequestTestAccess {
    static Json::Value Dispatch(HttpRpcServer& server,const std::string& method) {
        Json::Value request(Json::objectValue);request["jsonrpc"]="2.0";request["id"]=7;
        request["method"]=method;request["params"]=Json::Value(Json::objectValue);
        return server.process_rpc_call(request);
    }
};
namespace {
Json::Value Client(const Json::Value& response) {
    Json::StreamWriterBuilder writer;const auto wire=Json::writeString(writer,response);
    std::istringstream text(OrchardErrorEnvelopeTest::Decode(wire));Json::Value result;text>>result;return result;
}
void Failure(const Json::Value& response,const Json::Value& client) {
    ASSERT_TRUE(response["error"].isObject());EXPECT_FALSE(response.isMember("result"));
    EXPECT_FALSE(client["success"].asBool());EXPECT_EQ(client["details"].asInt(),1);
    EXPECT_EQ(client["legacy"].asInt(),1);EXPECT_EQ(client["results"].asInt(),0);
    EXPECT_EQ(client["tag"].asString(),"actual-request-tag");
}
}
TEST(RpcOrchardErrorEnvelope, TypedFailureCrossesDispatcherAndQtParserForAliases) {
    RpcRegistry registry;HttpRpcServer server("127.0.0.1",0);server.set_rpc_registry(&registry);
    Json::Value body;body["error"]="Proof pending";body["error_code"]="proof_not_ready";
    body["proof_state"]="running";body["reservation_retained"]=true;
    body["private_payment"]="must not escape";
    ASSERT_TRUE(registry.registerHandler("wallet.orchard.finishspend",[&](const ExecutionContext&,const Json::Value&){return body;},"test"));
    registry.registerAlias("explicit-finish","wallet.orchard.finishspend");
    for(const auto* name:{"wallet.orchard.finishspend","orchard.finishspend","explicit-finish"}) {
        SCOPED_TRACE(name);const auto r=HttpRpcRequestTestAccess::Dispatch(server,name);const auto c=Client(r);Failure(r,c);
        EXPECT_EQ(c["data"]["orchard"]["error_code"].asString(),"proof_not_ready");
        EXPECT_EQ(c["data"]["orchard"]["proof_state"].asString(),"running");
        EXPECT_TRUE(c["data"]["orchard"]["reservation_retained"].asBool());
        EXPECT_EQ(c["data"]["orchard"].size(),3u);EXPECT_FALSE(c["data"].isMember("private_payment"));
    }
    ASSERT_TRUE(registry.registerHandler("orchard.getactivationstatus",[&](const ExecutionContext&,const Json::Value&){return body;},"test"));
    registry.registerAlias("explicit-activation","orchard.getactivationstatus");
    for(const auto* name:{"orchard.getactivationstatus","getactivationstatus","explicit-activation"}) {
        SCOPED_TRACE(name);const auto r=HttpRpcRequestTestAccess::Dispatch(server,name);const auto c=Client(r);Failure(r,c);
        EXPECT_EQ(c["data"]["orchard"]["error_code"].asString(),"proof_not_ready");
        EXPECT_EQ(c["data"]["orchard"]["proof_state"].asString(),"running");
        EXPECT_TRUE(c["data"]["orchard"]["reservation_retained"].asBool());
        EXPECT_EQ(c["data"]["orchard"].size(),3u);EXPECT_FALSE(c["data"].isMember("private_payment"));
    }
    EXPECT_FALSE(server.is_running());
}
TEST(RpcOrchardErrorEnvelope, ActualBindingRefusalRetainsMachineCodeWithoutWalletAccess) {
    RegisterOrchardAccountRpc();HttpRpcServer server("127.0.0.1",0);server.set_rpc_registry(&g_rpcRegistry);
    for(const auto* name:{"wallet.orchard.queuespend","orchard.queuespend","wallet.orchard.getbalance","orchard.getbalance"}) {
        SCOPED_TRACE(name);const auto r=HttpRpcRequestTestAccess::Dispatch(server,name);const auto c=Client(r);Failure(r,c);
        EXPECT_EQ(c["data"]["orchard"]["error_code"].asString(),"wallet_binding_required");
        EXPECT_FALSE(c["data"]["orchard"].isMember("reservation_retained"));
    }
    EXPECT_FALSE(server.is_running());
}
TEST(RpcOrchardErrorEnvelope, MalformedDetailsNeverInventProofProgress) {
    RpcRegistry registry;HttpRpcServer server("127.0.0.1",0);server.set_rpc_registry(&registry);
    Json::Value body;body["error"]="refused";body["error_code"]="proof_not_ready";
    body["proof_state"]=17;body["reservation_retained"]="true";body["admitted"]=true;
    ASSERT_TRUE(registry.registerHandler("wallet.orchard.finishshield",[&](const ExecutionContext&,const Json::Value&){return body;},"test"));
    auto r=HttpRpcRequestTestAccess::Dispatch(server,"wallet.orchard.finishshield");auto c=Client(r);Failure(r,c);
    EXPECT_EQ(c["data"]["orchard"].size(),1u);EXPECT_FALSE(c["data"]["orchard"].isMember("admitted"));
    body["error_code"]=std::string(65,'a');r=HttpRpcRequestTestAccess::Dispatch(server,"wallet.orchard.finishshield");c=Client(r);Failure(r,c);
    EXPECT_FALSE(c.isMember("data"));
    body["error_code"]="proof_not_ready";ASSERT_TRUE(registry.registerHandler("wallet.other",[&](const ExecutionContext&,const Json::Value&){return body;},"test"));
    r=HttpRpcRequestTestAccess::Dispatch(server,"wallet.other");c=Client(r);Failure(r,c);EXPECT_FALSE(c.isMember("data"));
}
// v8.2.7: a fee refusal reaches the client with the fee it needs. Only for
// that code, only as integers; everything else in the result stays behind.
TEST(RpcOrchardErrorEnvelope, FeeBelowMinimumCarriesRequiredFeeAndSize) {
    RpcRegistry registry;HttpRpcServer server("127.0.0.1",0);server.set_rpc_registry(&registry);
    Json::Value body;body["error"]="Network fee is below the minimum for this payment's size";
    body["error_code"]="fee_below_minimum";body["retryable"]=false;
    body["required_fee_una"]=Json::UInt64(12410);body["virtual_size"]=Json::UInt64(12410);
    body["private_payment"]="must not escape";
    ASSERT_TRUE(registry.registerHandler("wallet.orchard.queuespend",[&](const ExecutionContext&,const Json::Value&){return body;},"test"));
    auto r=HttpRpcRequestTestAccess::Dispatch(server,"wallet.orchard.queuespend");auto c=Client(r);Failure(r,c);
    EXPECT_EQ(c["data"]["orchard"]["error_code"].asString(),"fee_below_minimum");
    EXPECT_EQ(c["data"]["orchard"]["required_fee_una"].asUInt64(),12410u);
    EXPECT_EQ(c["data"]["orchard"]["virtual_size"].asUInt64(),12410u);
    EXPECT_EQ(c["data"]["orchard"].size(),3u);EXPECT_FALSE(c["data"].isMember("private_payment"));
    body["required_fee_una"]="12410";body["virtual_size"]=-1;
    r=HttpRpcRequestTestAccess::Dispatch(server,"wallet.orchard.queuespend");c=Client(r);Failure(r,c);
    EXPECT_EQ(c["data"]["orchard"].size(),1u) << "malformed sizes must not cross";
    body["error_code"]="proof_not_ready";body["required_fee_una"]=Json::UInt64(1);
    r=HttpRpcRequestTestAccess::Dispatch(server,"wallet.orchard.queuespend");c=Client(r);Failure(r,c);
    EXPECT_FALSE(c["data"]["orchard"].isMember("required_fee_una")) << "only a fee refusal carries a fee";
}
TEST(RpcOrchardErrorEnvelope, WalletCreationRefusalKeepsOnlyTypedRetryState) {
    RpcRegistry registry;HttpRpcServer server("127.0.0.1",0);server.set_rpc_registry(&registry);
    Json::Value body;body["error"]="Orchard history is not ready; wallet was not created.";
    body["error_code"]="orchard_history_pending";body["retryable"]=true;
    body["mnemonic"]="must not escape";body["wallet_name"]="private name";
    ASSERT_TRUE(registry.registerHandler("wallet.createhd",[&](const ExecutionContext&,const Json::Value&){return body;},"test"));
    const auto response=HttpRpcRequestTestAccess::Dispatch(server,"wallet.createhd");
    const auto client=Client(response);Failure(response,client);
    EXPECT_EQ(client["data"]["wallet_creation"]["error_code"].asString(),"orchard_history_pending");
    EXPECT_TRUE(client["data"]["wallet_creation"]["retryable"].asBool());
    EXPECT_EQ(client["data"]["wallet_creation"].size(),2u);
    EXPECT_FALSE(client["data"].isMember("mnemonic"));
    EXPECT_FALSE(client["data"].isMember("wallet_name"));

    body["error_code"]="native_wallet_unservable";body["retryable"]=false;
    const auto permanent=Client(HttpRpcRequestTestAccess::Dispatch(server,"wallet.createhd"));
    EXPECT_EQ(permanent["data"]["wallet_creation"]["error_code"].asString(),"native_wallet_unservable");
    EXPECT_FALSE(permanent["data"]["wallet_creation"]["retryable"].asBool());

    body["retryable"]="true";
    const auto malformed=Client(HttpRpcRequestTestAccess::Dispatch(server,"wallet.createhd"));
    EXPECT_FALSE(malformed.isMember("data")) << "a malformed retry flag must not be promoted";
}
TEST(RpcOrchardErrorEnvelope, ExistingStructuredErrorsAndNullSuccessRemainCompatible) {
    RpcRegistry registry;HttpRpcServer server("127.0.0.1",0);server.set_rpc_registry(&registry);Json::Value body;
    body["error"]["code"]=-123;body["error"]["message"]="existing";body["error"]["data"]["reason"]="old";
    ASSERT_TRUE(registry.registerHandler("wallet.other",[&](const ExecutionContext&,const Json::Value&){return body;},"test"));
    const auto r=HttpRpcRequestTestAccess::Dispatch(server,"wallet.other");const auto c=Client(r);Failure(r,c);
    EXPECT_EQ(c["code"].asInt(),-123);EXPECT_EQ(c["data"]["reason"].asString(),"old");
    ASSERT_TRUE(registry.registerHandler("blockchain.gettxout",[](const ExecutionContext&,const Json::Value&){return Json::Value();},"test"));
    const auto ok=Client(HttpRpcRequestTestAccess::Dispatch(server,"blockchain.gettxout"));
    EXPECT_TRUE(ok["success"].asBool());EXPECT_TRUE(ok["result"].isNull());EXPECT_EQ(ok["results"].asInt(),1);EXPECT_EQ(ok["details"].asInt(),0);
}

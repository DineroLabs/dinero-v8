#include <gtest/gtest.h>
#include "daemon/http_rpc_server.h"
#include "daemon/rpc_utils.h"
#include "rpc/rpc_registry.h"
#include "rpc/orchard_account_rpc.h"
#include <array>
#include <string>

// This target never starts a listener, opens a socket, constructs a wallet,
// or runs a daemon. Exercise the actual production dispatch policy directly.
struct HttpRpcRequestTestAccess {
    static Json::Value Dispatch(HttpRpcServer& server, const std::string& method) {
        Json::Value request(Json::objectValue);
        request["jsonrpc"]="2.0";request["id"]=19;
        request["method"]=method;request["params"]=Json::Value(Json::objectValue);
        return server.process_rpc_call(request);
    }
};
namespace {
constexpr std::array<const char*,6> mutations{{
    "wallet.orchard.createaccount","wallet.orchard.getnewaddress",
    "wallet.orchard.queueshield","wallet.orchard.finishshield",
    "wallet.orchard.queuespend","wallet.orchard.finishspend"}};
constexpr std::array<const char*,6> observations{{
    "wallet.orchard.getwalletbinding","wallet.orchard.getbalance",
    "wallet.orchard.listaccounts","wallet.orchard.listoperations",
    "wallet.orchard.listreceived","orchard.getactivationstatus"}};
std::string Flat(const std::string& name) { return name.substr(name.find('.')+1); }
Json::Value Marker() { Json::Value r(Json::objectValue);r["visited"]=true;return r; }
void RequireForbidden(const Json::Value& r) {
    ASSERT_TRUE(r["error"].isObject());
    EXPECT_EQ(r["error"]["code"].asInt(),dinero::rpc::RPC_FORBIDDEN);
    EXPECT_FALSE(r.isMember("result") && !r["result"].isNull());
}
}
TEST(RpcOrchardReadOnlyPolicy, CanonicalAutoAndExplicitAliasesRefuseBeforeInvocation) {
    RpcRegistry registry;unsigned visits=0;
    HttpRpcServer server("127.0.0.1",0);server.set_rpc_registry(&registry);server.set_readonly_mode(true);
    unsigned index=0;
    for(const auto* name:mutations) {
        ASSERT_TRUE(registry.registerHandler(name,[&](const ExecutionContext&,const Json::Value&){++visits;return Marker();},"readonly-test"));
        const auto alias="test.orchard_write_"+std::to_string(index++);registry.registerAlias(alias,name);
        for(const auto& method:{std::string(name),Flat(name),alias}) {
            SCOPED_TRACE(method);ASSERT_NE(registry.lookup(method),nullptr);
            RequireForbidden(HttpRpcRequestTestAccess::Dispatch(server,method));EXPECT_EQ(visits,0u);
        }
    }
    // Existing ordinary wallet policy must remain enforced as well.
    ASSERT_TRUE(registry.registerHandler("wallet.sendtoaddress",[&](const ExecutionContext&,const Json::Value&){++visits;return Marker();},"readonly-test"));
    RequireForbidden(HttpRpcRequestTestAccess::Dispatch(server,"wallet.sendtoaddress"));
    RequireForbidden(HttpRpcRequestTestAccess::Dispatch(server,"sendtoaddress"));EXPECT_EQ(visits,0u);
    EXPECT_FALSE(server.is_running());
}
TEST(RpcOrchardReadOnlyPolicy, WritableServerStillDispatchesMutations) {
    RpcRegistry registry;unsigned visits=0;HttpRpcServer server("127.0.0.1",0);server.set_rpc_registry(&registry);
    for(const auto* name:mutations) {
        ASSERT_TRUE(registry.registerHandler(name,[&](const ExecutionContext&,const Json::Value&){++visits;return Marker();},"readonly-test"));
        for(const auto& method:{std::string(name),Flat(name)}) {
            const auto before=visits;const auto r=HttpRpcRequestTestAccess::Dispatch(server,method);
            EXPECT_TRUE(r["error"].isNull());EXPECT_TRUE(r["result"]["visited"].asBool());EXPECT_EQ(visits,before+1);
        }
    }
    EXPECT_FALSE(server.is_running());
}
TEST(RpcOrchardReadOnlyPolicy, ObservationMethodsRemainAvailable) {
    RpcRegistry registry;unsigned visits=0;HttpRpcServer server("127.0.0.1",0);server.set_rpc_registry(&registry);server.set_readonly_mode(true);
    for(const auto* name:observations) {
        ASSERT_TRUE(registry.registerHandler(name,[&](const ExecutionContext&,const Json::Value&){++visits;return Marker();},"readonly-test"));
        for(const auto& method:{std::string(name),Flat(name)}) {
            SCOPED_TRACE(method);const auto before=visits;const auto r=HttpRpcRequestTestAccess::Dispatch(server,method);
            EXPECT_TRUE(r["error"].isNull());EXPECT_TRUE(r["result"]["visited"].asBool());EXPECT_EQ(visits,before+1);
        }
    }
    EXPECT_FALSE(server.is_running());
}
TEST(RpcOrchardReadOnlyPolicy, ActualRegisteredMethodsRefuseBeforeBackendOrWalletAccess) {
    RegisterOrchardAccountRpc();HttpRpcServer server("127.0.0.1",0);server.set_rpc_registry(&g_rpcRegistry);server.set_readonly_mode(true);
    for(const auto* name:mutations) {
        for(const auto& method:{std::string(name),Flat(name)}) {
            SCOPED_TRACE(method);ASSERT_NE(g_rpcRegistry.lookup(method),nullptr);
            RequireForbidden(HttpRpcRequestTestAccess::Dispatch(server,method));
        }
    }
    EXPECT_FALSE(server.is_running());
}

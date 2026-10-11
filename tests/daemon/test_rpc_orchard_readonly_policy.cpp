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
constexpr std::array<const char*,5> observations{{
    "wallet.orchard.getwalletbinding","wallet.orchard.getbalance",
    "wallet.orchard.listaccounts","wallet.orchard.listoperations",
    "orchard.getactivationstatus"}};
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

TEST(RpcOrchardReadOnlyPolicy, MemoHistoryAndSecretExportsRefuseBeforeInvocation) {
    RpcRegistry registry; unsigned visits=0;
    HttpRpcServer server("127.0.0.1",0); server.set_rpc_registry(&registry);
    server.set_readonly_mode(true);
    unsigned index=0;
    for(const auto* name:{"wallet.orchard.listreceived", "wallet.exportseed", "wallet.exportp2mrseed"}) {
        ASSERT_TRUE(registry.registerHandler(name,[&](const ExecutionContext&,const Json::Value&){++visits;return Marker();},"private-content-test"));
        const auto alias="test.private_content_"+std::to_string(index++);
        registry.registerAlias(alias,name);
        for(const auto& method:{std::string(name),Flat(name),alias}) {
            SCOPED_TRACE(method);
            RequireForbidden(HttpRpcRequestTestAccess::Dispatch(server,method));
            EXPECT_EQ(visits,0u);
        }
    }
    EXPECT_FALSE(server.is_running());
}
TEST(RpcOrchardReadOnlyPolicy, WritableServerRetainsPrivateContentAccess) {
    RpcRegistry registry; unsigned visits=0;
    HttpRpcServer server("127.0.0.1",0); server.set_rpc_registry(&registry);
    for(const auto* name:{"wallet.orchard.listreceived", "wallet.exportseed", "wallet.exportp2mrseed"}) {
        ASSERT_TRUE(registry.registerHandler(name,[&](const ExecutionContext&,const Json::Value&){++visits;return Marker();},"private-content-test"));
        for(const auto& method:{std::string(name),Flat(name)}) {
            const auto before=visits; const auto r=HttpRpcRequestTestAccess::Dispatch(server,method);
            EXPECT_TRUE(r["error"].isNull()); EXPECT_TRUE(r["result"]["visited"].asBool());
            EXPECT_EQ(visits,before+1);
        }
    }
    EXPECT_FALSE(server.is_running());
}

// Dummy handlers only: this never invokes recovery or reads a real memo.
TEST(RpcOrchardReadOnlyPolicy, RecoveryTriggersAndLegacyMemosRefuseBeforeInvocation) {
    RpcRegistry registry; unsigned visits=0;
    HttpRpcServer server("127.0.0.1",0); server.set_rpc_registry(&registry);
    server.set_readonly_mode(true);
    unsigned index=0;
    for (const auto* name : {"blockchain.getutxoproof", "blockchain.getutxoproofs_batch", "blockchain.getproofupdates", "wallet.listshieldedoutgoing"}) {
        ASSERT_TRUE(registry.registerHandler(name,[&](const ExecutionContext&,const Json::Value&){++visits;return Marker();},"sensitive-observation-test"));
        const auto alias="test.sensitive_observation_"+std::to_string(index++);
        registry.registerAlias(alias,name);
        for (const auto& method : {std::string(name),Flat(name),alias}) {
            SCOPED_TRACE(method);
            RequireForbidden(HttpRpcRequestTestAccess::Dispatch(server,method));
            EXPECT_EQ(visits,0u);
        }
    }
    EXPECT_FALSE(server.is_running());
}
TEST(RpcOrchardReadOnlyPolicy, WritableServerRetainsSensitiveObservationDispatch) {
    RpcRegistry registry; unsigned visits=0;
    HttpRpcServer server("127.0.0.1",0); server.set_rpc_registry(&registry);
    unsigned index=0;
    for (const auto* name : {"blockchain.getutxoproof", "blockchain.getutxoproofs_batch", "blockchain.getproofupdates", "wallet.listshieldedoutgoing"}) {
        ASSERT_TRUE(registry.registerHandler(name,[&](const ExecutionContext&,const Json::Value&){++visits;return Marker();},"sensitive-observation-test"));
        const auto alias="test.sensitive_observation_"+std::to_string(index++);
        registry.registerAlias(alias,name);
        for (const auto& method : {std::string(name),Flat(name),alias}) {
            SCOPED_TRACE(method); const auto before=visits;
            const auto r=HttpRpcRequestTestAccess::Dispatch(server,method);
            EXPECT_TRUE(r["error"].isNull()); EXPECT_TRUE(r["result"]["visited"].asBool());
            EXPECT_EQ(visits,before+1);
        }
    }
    EXPECT_FALSE(server.is_running());
}

// Exercise dispatch, never the production side-effect or key-returning handler.
TEST(RpcOrchardReadOnlyPolicy, SideEffectsAndViewingAuthorityRefuseBeforeInvocation) {
    RpcRegistry registry; unsigned visits=0;
    HttpRpcServer server("127.0.0.1",0); server.set_rpc_registry(&registry);
    server.set_readonly_mode(true);
    unsigned index=0;
    for (const auto* name : {"wallet.utxoproof", "wallet.validatestatelessbalance", "wallet.listdescriptors", "daemon.shieldedstatehash", "blockchain.getblockfilters", "mining.getblocktemplate", "ping", "wallet.listshielded", "wallet.shieldedbalance", "wallet.listaddresses", "wallet.validateaddress"}) {
        ASSERT_TRUE(registry.registerHandler(name,[&](const ExecutionContext&,const Json::Value&){++visits;return Marker();},"side-effect-test"));
        const auto alias="test.side_effect_"+std::to_string(index++);registry.registerAlias(alias,name);
        for (const auto& method : {std::string(name),Flat(name),alias}) {
            SCOPED_TRACE(method);RequireForbidden(HttpRpcRequestTestAccess::Dispatch(server,method));
            EXPECT_EQ(visits,0u);
        }
    }
    EXPECT_FALSE(server.is_running());
}
TEST(RpcOrchardReadOnlyPolicy, WritableServerRetainsSideEffectDispatch) {
    RpcRegistry registry; unsigned visits=0;
    HttpRpcServer server("127.0.0.1",0); server.set_rpc_registry(&registry);
    unsigned index=0;
    for (const auto* name : {"wallet.utxoproof", "wallet.validatestatelessbalance", "wallet.listdescriptors", "daemon.shieldedstatehash", "blockchain.getblockfilters", "mining.getblocktemplate", "ping", "wallet.listshielded", "wallet.shieldedbalance", "wallet.listaddresses", "wallet.validateaddress"}) {
        ASSERT_TRUE(registry.registerHandler(name,[&](const ExecutionContext&,const Json::Value&){++visits;return Marker();},"side-effect-test"));
        const auto alias="test.side_effect_"+std::to_string(index++);registry.registerAlias(alias,name);
        for (const auto& method : {std::string(name),Flat(name),alias}) {
            SCOPED_TRACE(method);const auto before=visits;
            const auto r=HttpRpcRequestTestAccess::Dispatch(server,method);
            EXPECT_TRUE(r["error"].isNull());EXPECT_TRUE(r["result"]["visited"].asBool());
            EXPECT_EQ(visits,before+1);
        }
    }
    EXPECT_FALSE(server.is_running());
}

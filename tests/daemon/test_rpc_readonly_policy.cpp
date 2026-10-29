// --rpc-readonly is default-deny (include/daemon/rpc_readonly_policy.h), and
// permission follows the handler that would actually run: the registry's
// handlerSource(), not the spelling of the request.
#include "daemon/rpc_readonly_policy.h"
#include "rpc/rpc_registry.h"

#include <gtest/gtest.h>

#include <regex>

using dinero::rpc::ReadOnlyAllowedMethods;
using dinero::rpc::ReadOnlyAllows;

namespace {
RpcHandler Marker(int id) {
    return [id](const ExecutionContext&, const din::Json&) { din::Json j; j["marker"] = id; return j; };
}
int RunMarker(RpcRegistry& r, const std::string& m) {
    RpcHandler* h = r.lookup(m);
    return h ? (*h)(ExecutionContext{}, din::Json())["marker"].asInt() : -1;
}
bool AllowsRequest(RpcRegistry& r, const std::string& m) { return ReadOnlyAllows(r.handlerSource(m)); }
}  // namespace

TEST(RpcReadOnlyPolicy, ANamespacedHandlerAndItsGeneratedShortNameAreOneHandler) {
    RpcRegistry r;
    r.registerHandler("wallet.getbalance", Marker(1));
    r.registerHandler("wallet.exportseed", Marker(2));
    EXPECT_EQ(r.handlerSource("getbalance"), "wallet.getbalance");
    EXPECT_TRUE(AllowsRequest(r, "wallet.getbalance"));
    EXPECT_TRUE(AllowsRequest(r, "getbalance"));
    EXPECT_FALSE(AllowsRequest(r, "wallet.exportseed"));
    EXPECT_FALSE(AllowsRequest(r, "exportseed"));
}

// A different handler registered under the short name first: lookup runs it,
// so it must not borrow wallet.getbalance's permission.
TEST(RpcReadOnlyPolicy, AStandaloneShortNameDoesNotInheritTheNamespacedPermission) {
    RpcRegistry r;
    r.registerHandler("getbalance", Marker(7));
    r.registerHandler("wallet.getbalance", Marker(1));
    ASSERT_EQ(RunMarker(r, "getbalance"), 7);
    EXPECT_EQ(r.handlerSource("getbalance"), "getbalance");
    EXPECT_FALSE(AllowsRequest(r, "getbalance"));
    EXPECT_TRUE(AllowsRequest(r, "wallet.getbalance"));
}

TEST(RpcReadOnlyPolicy, OverwritingTheShortNameDropsTheNamespacedPermission) {
    RpcRegistry r;
    r.registerHandler("wallet.getbalance", Marker(1), RegisterMode::Overwrite);
    r.registerHandler("getbalance", Marker(7), RegisterMode::Overwrite);
    ASSERT_EQ(RunMarker(r, "getbalance"), 7);
    EXPECT_FALSE(AllowsRequest(r, "getbalance"));
}

// Overwriting the namespaced handler leaves the short name running the old
// copy. Its origin is no longer the registered handler: refused.
TEST(RpcReadOnlyPolicy, AShortNameLeftOnAnOverwrittenHandlerIsRefused) {
    RpcRegistry r;
    r.registerHandler("wallet.getbalance", Marker(1), RegisterMode::Overwrite);
    r.registerHandler("wallet.getbalance", Marker(2), RegisterMode::Overwrite);
    ASSERT_EQ(RunMarker(r, "getbalance"), 1) << "the registry keeps the stale copy (unchanged behavior)";
    ASSERT_EQ(RunMarker(r, "wallet.getbalance"), 2);
    EXPECT_EQ(r.handlerSource("getbalance"), "");
    EXPECT_FALSE(AllowsRequest(r, "getbalance"));
    EXPECT_TRUE(AllowsRequest(r, "wallet.getbalance"));
}

TEST(RpcReadOnlyPolicy, AnExactHandlerWinsOverAnExplicitAlias) {
    RpcRegistry r;
    r.registerHandler("wallet.getbalance", Marker(1));
    r.registerHandler("wallet.exportseed", Marker(2));
    r.registerAlias("balance", "wallet.getbalance");  // explicit alias, no handler of that name
    EXPECT_EQ(RunMarker(r, "balance"), 1);
    EXPECT_TRUE(AllowsRequest(r, "balance"));
    r.registerHandler("myseed", Marker(3));
    r.registerAlias("myseed", "wallet.getbalance");  // exact handler named like the alias
    ASSERT_EQ(RunMarker(r, "myseed"), 3);
    EXPECT_FALSE(AllowsRequest(r, "myseed")) << "the exact handler runs, not the alias target";
    r.registerAlias("seedalias", "wallet.exportseed");
    EXPECT_FALSE(AllowsRequest(r, "seedalias"));
}

TEST(RpcReadOnlyPolicy, ARuntimeRegistrationCycleRestoresTheOrigins) {
    RpcRegistry r;
    r.registerHandler("wallet.getbalance", Marker(1), RegisterMode::Overwrite);
    r.beginRuntimeRegistrationCycle();  // baseline
    r.registerHandler("getbalance", Marker(7), RegisterMode::Overwrite);
    EXPECT_FALSE(AllowsRequest(r, "getbalance"));
    r.beginRuntimeRegistrationCycle();  // restore baseline
    EXPECT_EQ(RunMarker(r, "getbalance"), 1);
    EXPECT_TRUE(AllowsRequest(r, "getbalance"));
}

TEST(RpcReadOnlyPolicy, UnknownAndEmptyAreRefused) {
    RpcRegistry r;
    EXPECT_EQ(r.handlerSource("wallet.somethingnew"), "");
    EXPECT_FALSE(ReadOnlyAllows(""));
    EXPECT_FALSE(ReadOnlyAllows("wallet.somethingnew"));
    EXPECT_TRUE(ReadOnlyAllows("http:help")) << "the HTTP server's own table is named apart";
    EXPECT_FALSE(ReadOnlyAllows("help")) << "a registry handler named help is not the HTTP built-in";
}

// A guard on the list itself: an action that writes, spends or exports
// (the verb that starts the last name segment) cannot be added without
// being reviewed here too. A name heuristic, not a semantic proof.
TEST(RpcReadOnlyPolicy, TheAllowListNamesNoActionThatWritesSpendsOrExports) {
    const std::regex risky(
        "^(send|spend|sign|fund|pay|withdraw|transfer|shield|unshield|broadcast|submit|export|import|dump|backup|"
        "unlock|lock|encrypt|passphrase|open|unload|delete|remove|rename|restore|create|new|set|start|stop|"
        "clear|generate|invalidate|reconsider|prune|reindex|rebuild|rescan|abandon|authorize|request|refresh|"
        "revoke|watch|unwatch|accept|cancel|refund|offer|claim|configure|exit|ban|addnode|disconnect|connect|"
        "process|consolidate|record|migrate|notarize|acknowledge|enable|allow|observe|dial|follow|subscribe|"
        "mnemonic|load|reset)",
        std::regex::icase);
    const std::unordered_set<std::string> reviewed = {
        "wallet.createrawtransaction",  // unsigned bytes from caller-given inputs: no keys, no wallet change
        "wallet.generateqrcode",        // renders the given text
        "daemon.shieldedroot", "daemon.shieldedstatehash", "wallet.shieldedbalance",  // "shielded": reads
        "pool.payouts",                 // payout history; processing is pool.processpayouts
        "rpc.openrpc",                  // the OpenRPC schema
        "vault.account.spendable",      // reads the spendable amount
    };
    for (const auto& m : ReadOnlyAllowedMethods()) {
        const std::string action = m.substr(m.rfind('.') == std::string::npos ? 0 : m.rfind('.') + 1);
        if (reviewed.count(m)) continue;
        EXPECT_FALSE(std::regex_search(action, risky)) << m << " looks like it changes state; review it";
    }
}

TEST(RpcReadOnlyPolicy, OrchardObservationsKeepTheirExactHandlerOrigins) {
    RpcRegistry r;
    for(const auto* name:{"wallet.orchard.getwalletbinding", "wallet.orchard.getbalance",
                         "wallet.orchard.listaccounts", "wallet.orchard.listoperations",
                         "orchard.getactivationstatus"}) {
        const std::string canonical(name), flat=canonical.substr(canonical.find('.')+1);
        ASSERT_TRUE(r.registerHandler(canonical,Marker(1)));
        r.registerAlias("observation_alias",canonical);
        EXPECT_TRUE(AllowsRequest(r,canonical)); EXPECT_TRUE(AllowsRequest(r,flat));
        EXPECT_TRUE(AllowsRequest(r,"observation_alias"));
    }
}
TEST(RpcReadOnlyPolicy, OrchardMemosAndSecretsRemainDeniedThroughAliases) {
    RpcRegistry r;
    for(const auto* name:{"wallet.orchard.listreceived", "wallet.exportseed", "wallet.exportp2mrseed"}) {
        const std::string canonical(name), flat=canonical.substr(canonical.find('.')+1);
        ASSERT_TRUE(r.registerHandler(canonical,Marker(2)));
        r.registerAlias("private_content_alias",canonical);
        EXPECT_FALSE(AllowsRequest(r,canonical)); EXPECT_FALSE(AllowsRequest(r,flat));
        EXPECT_FALSE(AllowsRequest(r,"private_content_alias"));
    }
}

TEST(RpcReadOnlyPolicy, RecoveryTriggersAndLegacyMemosRemainDeniedThroughAliases) {
    RpcRegistry r;
    for (const auto* name : {"blockchain.getutxoproof", "blockchain.getutxoproofs_batch", "blockchain.getproofupdates", "wallet.listshieldedoutgoing"}) {
        SCOPED_TRACE(name);
        const std::string canonical(name), flat=canonical.substr(canonical.find('.')+1);
        ASSERT_TRUE(r.registerHandler(canonical, Marker(3)));
        r.registerAlias("sensitive_observation_alias", canonical);
        EXPECT_FALSE(AllowsRequest(r, canonical));
        EXPECT_FALSE(AllowsRequest(r, flat));
        EXPECT_FALSE(AllowsRequest(r, "sensitive_observation_alias"));
    }
}

TEST(RpcReadOnlyPolicy, SideEffectsAndViewingAuthorityRemainDeniedThroughAliases) {
    RpcRegistry r;
    for (const auto* name : {"wallet.utxoproof", "wallet.validatestatelessbalance", "wallet.listdescriptors", "daemon.shieldedstatehash", "blockchain.getblockfilters", "mining.getblocktemplate", "ping", "wallet.listshielded", "wallet.shieldedbalance", "wallet.listaddresses", "wallet.validateaddress"}) {
        SCOPED_TRACE(name);
        const std::string canonical(name), flat=canonical.substr(canonical.find('.')+1);
        ASSERT_TRUE(r.registerHandler(canonical, Marker(4)));
        r.registerAlias("side_effect_alias",canonical);
        EXPECT_FALSE(AllowsRequest(r,canonical));
        EXPECT_FALSE(AllowsRequest(r,flat));
        EXPECT_FALSE(AllowsRequest(r,"side_effect_alias"));
    }
}

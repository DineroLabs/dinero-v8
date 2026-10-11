#pragma once
// What a --rpc-readonly server lets a client call: an allow-list.
//
// ADMIN_METHODS (http_rpc_server.cpp) lists methods to refuse; every method
// it missed was callable read-only, including wallet.exportseed (the whole
// recovery phrase), wallet.delete and wallet.open. Here read-only mode is
// default-deny: a method runs only if it is named below. A method added
// later is refused until someone decides it is safe to read; forgetting a
// safe one is a missing feature, not a leak.
//
// Only methods that change nothing and reveal no secret belong here: no
// spend, sign, broadcast, export, import, key or seed material, wallet
// selection, lock/unlock, configuration, start/stop or file writes.
#include <string>
#include <unordered_set>

namespace dinero::rpc {

inline const std::unordered_set<std::string>& ReadOnlyAllowedMethods() {
    static const std::unordered_set<std::string> kAllowed = {
        // The HTTP server's own method table (named apart from the registry).
        "http:help", "http:getinfo", "http:listtransactions", "http:relay_hints.list", "http:seeder.status",
        // Registered under a flat name of their own.
        "getaddednodeinfo", "getbuildinfo", "getconnectioncount", "getconsensusinfo", "getdaemonstatus",
        "getnettotals", "getnetworkinfo", "getpeerinfo", "getpeerscores", "listbanned", 

        "addrman.stats", "addrman.status",
        "auth.stats", "auth.whoami",
        "blockchain.getaddressbalance", "blockchain.getaddressbatch", "blockchain.getaddresshistory",
        "blockchain.getaddressmempool", "blockchain.getarchivalstatus", "blockchain.getbackgroundvalidationprogress",
        "blockchain.getbestblockhash", "blockchain.getblock", "blockchain.getblockchaininfo", "blockchain.getblockcount",
         "blockchain.getblockhash", "blockchain.getblockheader", "blockchain.getchaintips",
        "blockchain.getchainwork", "blockchain.getcheckpoints", "blockchain.getdifficulty", "blockchain.getheaders",
        "blockchain.getibdprogress", "blockchain.getinfo", "blockchain.getmininginfo", 
        "blockchain.getpruninginfo", "blockchain.getreorgstatus", "blockchain.getsnapshotbootstrapstatus",
        "blockchain.getsynchealth", "blockchain.gettransaction", "blockchain.gettxout", "blockchain.gettxoutsetinfo",
        "blockchain.getutreexocachestats", "blockchain.getutreexocommitment", "blockchain.getutreexogossipstats",
        "blockchain.getutreexoroots", "blockchain.getutreexostats", 
         "blockchain.scantxoutset", "blockchain.shielded.outputs",
        "blockchain.verifyutxoproofs_batch",
        "compactblocks.stats", "compactblocks.status",
        "contract.list", "contract.status",
        "ct.estimatefee", "ct.getfeeconfig",
        "daemon.getstatus", "daemon.shieldedroot", 
        "dpi.checkconflicts", "dpi.decodeinvoice", "dpi.verifypackage",
        "economics.getinfo", "economics.getminerstats", "economics.getsupply",
        "hwallet.analyzepsbt",
        "market.getoffer", "market.getreputation", "market.listoffers", "market.myoffers", "market.mytrades",
        "market.search",
        "mempool.estimatefee", "mempool.estimatesmartfee", "mempool.getbyfee", "mempool.getfeehistogram",
        "mempool.getinfo", "mempool.getmempoolancestors", "mempool.getmempooldescendants", "mempool.getmempoolentry",
        "mempool.getorphans", "mempool.getrawmempool", "mempool.gettransaction", "mempool.stats",
        "mempool.testmempoolaccept",
        "mining.getaddress",  "mining.getinfo", "mining.getstatus", "mining.getstratuminfo",
        "mining.getstratumtemplate", "mining.gpuinfo", "mining.gpustatus", "mining.info", "mining.status",
        "network.getrelayservice", "network.info",
        "node.getcpustats", "node.getdiskstats", "node.getresourcepressure", "node.health", "node.info", "node.status",
        "payment.analyze", "payment.status",
        "pool.block", "pool.blocks", "pool.getconfig", "pool.payouts", "pool.stats", "pool.status", "pool.worker",
        "pool.workers",
        "rbf.estimate", "rbf.status",
        // Storage-initializing reads, filter backfill, mining-job mutation,
        // network ping, viewing-key descriptors and unsnapshotted state hashing
        // are not generic read-only capabilities. Operational status metadata
        // (including node paths and advertised addresses) remains available to
        // authenticated monitoring clients; this is not a public-anonymous API.
        // Proof queries that can schedule chainstate recovery and legacy outgoing
        // memo history are deliberately absent. Read permission must not confer
        // recovery control or access to private payment contents.
        // Orchard observation methods reviewed for this integration. Memo-bearing
        // listreceived remains denied to generic read-only credentials.
        "wallet.orchard.getwalletbinding", "wallet.orchard.getbalance",
        "wallet.orchard.listaccounts", "wallet.orchard.listoperations",
        "orchard.getactivationstatus",
        "reorg.status",
        "rpc.apihash", "rpc.discover", "rpc.info", "rpc.listmethods", "rpc.methods", "rpc.namespaces", "rpc.openrpc",
        "rpc.schema", "rpc.version",
        "safemode.status",
        "shielded.witness.by_index",
        "swap.decode", "swap.list", "swap.status",
        "sync.metrics", "sync.status",
        "telemetry.getverificationsummary",
        "vault.account.metrics", "vault.account.spendable", "vault.getoperator", "vault.metrics",
        "vault.withdrawal.status",
        "wallet.combinepsbt", "wallet.covenant.inspect", "wallet.covenant.list", "wallet.createrawtransaction",
        "wallet.decoderawtransaction", "wallet.deriveaddress", "wallet.deriveaddresses", "wallet.estimatefee",
        "wallet.generateqrcode", "wallet.getbalance", "wallet.getdescriptorinfo", "wallet.getinfo", "wallet.getlabel",
        "wallet.getrawtransaction", "wallet.getreceivepolicy", "wallet.getreorginfo", "wallet.getslowreason",
        "wallet.getstatus", "wallet.getsyncstate", "wallet.getsyncstatus", "wallet.gettransaction",
        "wallet.getwalletinfo",  "wallet.listaddresseswithbalances",
          // private descriptors are redacted
        "wallet.listp2mraddresses",   "wallet.listtransactions",
        "wallet.listunspent", "wallet.listwallets", "wallet.proofstatus", 
           "wallet.verifyutxoproof",
    };
    return kAllowed;
}

// source: what would actually run for the request, as
// RpcRegistry::handlerSource() names it ("ns.rest" for a generated short name
// "rest"), or "http:<name>" for the HTTP server's own table. Empty means no
// handler or one of unknown origin: refused.
inline bool ReadOnlyAllows(const std::string& source) {
    return !source.empty() && ReadOnlyAllowedMethods().count(source) > 0;
}

}  // namespace dinero::rpc

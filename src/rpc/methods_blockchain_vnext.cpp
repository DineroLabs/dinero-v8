/**
 * Blockchain RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Core blockchain query and validation methods.
 */

#include "rpc/rpc_method_builder.h"
#include "common/logger.h"
#include <iostream>

namespace din {
namespace rpc {

// Implementation functions from methods_blockchain_legacy.cpp
extern din::Json rpc_legacy_getblockcount(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_legacy_getblockhash(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_legacy_getblock(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_legacy_getblockchaininfo(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_getsynchealth(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_getarchivalstatus(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_legacy_getmininginfo(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_legacy_submitblock(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_legacy_invalidateblock(const ExecutionContext& ctx, const din::Json& params);

// Additional blockchain methods from other handlers
extern din::Json getbestblockhash_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getblockheader_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getchaintips_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getdifficulty_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getchainwork_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getverificationsummary_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getsupply_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getreorgstatus_impl(const ExecutionContext& ctx, const din::Json& params);

void registerBlockchainMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // CORE BLOCKCHAIN QUERY METHODS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("blockchain.getblockcount", "blockchain")
        .description("Returns the number of blocks in the longest blockchain")
        .params({})
        .result("number", "The current block height")
        .handler(rpc_legacy_getblockcount)
        .examples({
            "blockchain.getblockcount"
        });

    RPC_METHOD("blockchain.getbestblockhash", "blockchain")
        .description("Returns the hash of the best (tip) block in the longest blockchain")
        .params({})
        .result("string", "The block hash (hex)")
        .handler(getbestblockhash_impl)
        .examples({
            "blockchain.getbestblockhash"
        });

    RPC_METHOD("blockchain.getblockhash", "blockchain")
        .description("Returns hash of block at given height")
        .param("height", "number", "The block height", true)
        .result("string", "The block hash")
        .handler(rpc_legacy_getblockhash)
        .examples({
            "blockchain.getblockhash 1000",
            "blockchain.getblockhash 0"
        });

    RPC_METHOD("blockchain.getblock", "blockchain")
        .description("Returns information about a block. Utreexo commitment fields use display-order hex for compatibility; use utreexocommitment_raw for semantic equality with header bytes, proof bundles, or blockchain.getutreexocommitment.")
        .param("blockhash", "string", "The block hash", true)
        .param("verbosity", "number", "0=hex, 1=json, 2=json+txs (default: 1)", false)
        .result("string|object", "Block data (format depends on verbosity)")
        .handler(rpc_legacy_getblock)
        .examples({
            "blockchain.getblock \"00000000000000000007..\"",
            "blockchain.getblock \"00000000000000000007..\" 2"
        });

    RPC_METHOD("blockchain.getblockheader", "blockchain")
        .description("Returns block header information. utreexo_root is display-order hex for compatibility; use utreexo_root_raw for semantic equality with header bytes, proof bundles, or blockchain.getutreexocommitment.")
        .param("blockhash", "string", "The block hash", true)
        .param("verbose", "boolean", "true=json, false=hex (default: true)", false)
        .result("object|string", "Block header data")
        .handler(getblockheader_impl)
        .examples({
            "blockchain.getblockheader \"00000000000000000007..\"",
            "blockchain.getblockheader \"00000000000000000007..\" false"
        });

    RPC_METHOD("blockchain.getblockchaininfo", "blockchain")
        .description("Returns comprehensive blockchain state information")
        .params({})
        .result("object", "Blockchain info including height, difficulty, chain work, etc.")
        .handler(rpc_legacy_getblockchaininfo)
        .examples({
            "blockchain.getblockchaininfo"
        });

    RPC_METHOD("blockchain.getsynchealth", "blockchain")
        .description("Returns operator-focused sync diagnostics across active chain, header selector, persisted header store, peers, and block download state")
        .params({})
        .result("object", "Structured sync-health diagnostics")
        .handler(rpc_context_getsynchealth)
        .examples({
            "blockchain.getsynchealth"
        });

    RPC_METHOD("blockchain.getarchivalstatus", "blockchain")
        .description("Returns whether local blk*.dat coverage is sufficient for genesis-to-tip replay and where the first archival gap begins")
        .params({})
        .result("object", "Archival coverage and replayability diagnostics")
        .handler(rpc_context_getarchivalstatus)
        .examples({
            "blockchain.getarchivalstatus"
        });

    // ═══════════════════════════════════════════════════════════════
    // CHAIN ANALYSIS METHODS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("blockchain.getchaintips", "blockchain")
        .description("Returns information about all known chain tips")
        .params({})
        .result("array", "Array of chain tip objects with status and heights")
        .handler(getchaintips_impl)
        .examples({
            "blockchain.getchaintips"
        });

    RPC_METHOD("blockchain.getdifficulty", "blockchain")
        .description("Returns the current proof-of-work difficulty")
        .params({})
        .result("number", "The difficulty value")
        .handler(getdifficulty_impl)
        .examples({
            "blockchain.getdifficulty"
        });

    RPC_METHOD("blockchain.getchainwork", "blockchain")
        .description("Returns the total cumulative chain work")
        .params({})
        .result("string", "Chain work in hexadecimal")
        .handler(getchainwork_impl)
        .examples({
            "blockchain.getchainwork"
        });

    RPC_METHOD("blockchain.getverificationsummary", "blockchain")
        .description("Returns blockchain verification and validation summary")
        .params({})
        .result("object", "Verification summary with block validation stats")
        .handler(getverificationsummary_impl)
        .examples({
            "blockchain.getverificationsummary"
        });

    RPC_METHOD("blockchain.getreorgstatus", "blockchain")
        .description("Returns information about recent blockchain reorganizations")
        .params({})
        .result("object", "Reorg status with detected reorganizations")
        .handler(getreorgstatus_impl)
        .examples({
            "blockchain.getreorgstatus"
        });

    // ═══════════════════════════════════════════════════════════════
    // ECONOMIC & SUPPLY METHODS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("blockchain.getsupply", "blockchain")
        .description("Returns current circulating supply and emission information")
        .params({})
        .result("object", "Supply data including minted, burned, and circulating amounts")
        .handler(getsupply_impl)
        .examples({
            "blockchain.getsupply"
        });

    // ═══════════════════════════════════════════════════════════════
    // MINING & BLOCK SUBMISSION
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("blockchain.getmininginfo", "blockchain")
        .description("Returns mining-related information")
        .params({})
        .result("object", "Mining info including difficulty, network hashrate, pool info")
        .handler(rpc_legacy_getmininginfo)
        .examples({
            "blockchain.getmininginfo"
        });

    RPC_METHOD("blockchain.submitblock", "blockchain")
        .description("Attempts to submit a new block to the network")
        .param("hexdata", "string", "Block data in hex format", true)
        .result("null|string", "null on success, error string on failure")
        .handler(rpc_legacy_submitblock)
        .examples({
            "blockchain.submitblock \"0000002000000000...\""
        });

    RPC_METHOD("blockchain.invalidateblock", "blockchain")
        .description("Marks a block as invalid and reorganizes the chain")
        .param("blockhash", "string", "The block hash to invalidate", true)
        .result("null", "null on success")
        .handler(rpc_legacy_invalidateblock)
        .examples({
            "blockchain.invalidateblock \"00000000000000000007...\""
        });

    std::cout << "[Blockchain RPC vNext] ✅ Registered 16 blockchain methods with full metadata" << std::endl;
}

} // namespace rpc
} // namespace din

// Auto-register at startup
static auto _blockchain_vnext_init = (din::rpc::registerBlockchainMethodsVNext(), 0);

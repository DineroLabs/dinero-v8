// SPDX-License-Identifier: MIT
// Dinero - Blockchain RPC Method Metadata
//
// This file provides comprehensive documentation metadata for all blockchain RPC methods.
// It improves UX by adding descriptions, parameter specs, and return value documentation.

#include "rpc/rpc_registry.h"

// Forward declaration of global RPC registry (in global namespace, not dinero::rpc)
extern RpcRegistry g_rpcRegistry;

namespace dinero {
namespace rpc {

// Forward declarations of RPC handlers
extern din::Json rpc_context_getblockcount(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_getblockhash(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_getblock(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_getblockheader(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_getblockchaininfo(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_getdifficulty(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_context_getbestblockhash(const ExecutionContext& ctx, const din::Json& params);

/**
 * Register all blockchain RPC methods with comprehensive metadata
 *
 * This function provides detailed documentation for each RPC method including:
 * - Method description
 * - Parameter specifications (name, type, description, required/optional)
 * - Return value specification (type, description)
 * - Help text with examples
 */
void registerBlockchainRPCMetadata() {
    // =========================================================================
    // getblockcount - Get current blockchain height
    // =========================================================================
    {
        RpcMethodMeta meta;
        meta.name = "getblockcount";
        meta.ns = "blockchain";
        meta.description = "Returns the height of the most-work fully-validated chain";

        // No parameters
        meta.params = {};

        // Return value
        meta.result.type = "number";
        meta.result.desc = "The current block height (number of blocks in the longest chain)";

        // Help text
        meta.help = R"(getblockcount

Returns the number of blocks in the longest blockchain.

Result:
n    (number) The current block count

Examples:
> dinero-cli getblockcount
> curl --user $(cat ~/.dinero/.cookie) --data-binary '{"jsonrpc":"1.0","id":"1","method":"getblockcount","params":[]}' -H 'content-type:text/plain;' http://127.0.0.1:20998/
)";

        ::g_rpcRegistry.registerHandler("blockchain.getblockcount", rpc_context_getblockcount, meta,
                                     RegisterMode::Overwrite, "blockchain_meta");
        ::g_rpcRegistry.registerAlias("getblockcount", "blockchain.getblockcount");
    }

    // =========================================================================
    // getblockhash - Get block hash by height
    // =========================================================================
    {
        RpcMethodMeta meta;
        meta.name = "getblockhash";
        meta.ns = "blockchain";
        meta.description = "Returns the block hash for the specified block height";

        // Parameters
        meta.params = {
            {"height", "number", "The block height", true}
        };

        // Return value
        meta.result.type = "string";
        meta.result.desc = "The block hash (64-character hex string)";

        // Help text
        meta.help = R"(getblockhash height

Returns hash of block in best-block-chain at height provided.

Arguments:
1. height    (number, required) The block height

Result:
"hash"       (string) The block hash

Examples:
> dinero-cli getblockhash 1000
> curl --user $(cat ~/.dinero/.cookie) --data-binary '{"jsonrpc":"1.0","id":"1","method":"getblockhash","params":[1000]}' -H 'content-type:text/plain;' http://127.0.0.1:20998/
)";

        ::g_rpcRegistry.registerHandler("blockchain.getblockhash", rpc_context_getblockhash, meta,
                                     RegisterMode::Overwrite, "blockchain_meta");
        ::g_rpcRegistry.registerAlias("getblockhash", "blockchain.getblockhash");
    }

    // =========================================================================
    // getblock - Get block data
    // =========================================================================
    {
        RpcMethodMeta meta;
        meta.name = "getblock";
        meta.ns = "blockchain";
        meta.description = "Returns detailed block information by block hash";

        // Parameters
        meta.params = {
            {"blockhash", "string", "The block hash (64-character hex string)", true},
            {"verbosity", "number", "0=hex string, 1=json object, 2=json with tx data (default: 1)", false}
        };

        // Return value
        meta.result.type = "object|string";
        meta.result.desc = "Block data (format depends on verbosity parameter)";

        // Help text
        meta.help = R"(getblock "blockhash" ( verbosity )

Returns block data for the specified block hash.

Arguments:
1. blockhash     (string, required) The block hash
2. verbosity     (number, optional, default=1) 0 for hex, 1 for json, 2 for json with transaction data

Result (for verbosity = 1):
{
  "hash" : "hash",            (string) the block hash (same as provided)
  "confirmations" : n,        (number) number of confirmations
  "height" : n,               (number) the block height
  "version" : n,              (number) the block version
  "merkleroot" : "xxxx",      (string) the merkle root
  "time" : ttt,               (number) the block time in seconds since epoch
  "nonce" : n,                (number) the nonce
  "bits" : "1d00ffff",        (string) the bits
  "difficulty" : x.xxx,       (number) the difficulty
  "chainwork" : "xxxx",       (string) total chainwork (hex)
  "previousblockhash" : "hash",   (string) hash of previous block
  "nextblockhash" : "hash",       (string) hash of next block
  "tx" : [                    (array) transaction ids
    "txid",                   (string) transaction id
    ...
  ]
}

Examples:
> dinero-cli getblock "00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09"
> curl --user $(cat ~/.dinero/.cookie) --data-binary '{"jsonrpc":"1.0","id":"1","method":"getblock","params":["00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09"]}' -H 'content-type:text/plain;' http://127.0.0.1:20998/
)";

        ::g_rpcRegistry.registerHandler("blockchain.getblock", rpc_context_getblock, meta,
                                     RegisterMode::Overwrite, "blockchain_meta");
        ::g_rpcRegistry.registerAlias("getblock", "blockchain.getblock");
    }

    // =========================================================================
    // getblockheader - Get block header
    // =========================================================================
    {
        RpcMethodMeta meta;
        meta.name = "getblockheader";
        meta.ns = "blockchain";
        meta.description = "Returns block header information by block hash";

        // Parameters
        meta.params = {
            {"blockhash", "string", "The block hash (64-character hex string)", true},
            {"verbose", "boolean", "true for json object, false for hex string (default: true)", false}
        };

        // Return value
        meta.result.type = "object|string";
        meta.result.desc = "Block header data (format depends on verbose parameter)";

        // Help text
        meta.help = R"(getblockheader "blockhash" ( verbose )

Returns block header information for the specified block hash.

Arguments:
1. blockhash     (string, required) The block hash
2. verbose       (boolean, optional, default=true) true for json, false for hex

Result (for verbose = true):
{
  "hash" : "hash",            (string) the block hash
  "confirmations" : n,        (number) number of confirmations
  "height" : n,               (number) the block height
  "version" : n,              (number) the block version
  "merkleroot" : "xxxx",      (string) the merkle root
  "time" : ttt,               (number) the block time
  "nonce" : n,                (number) the nonce
  "bits" : "1d00ffff",        (string) the bits
  "difficulty" : x.xxx,       (number) the difficulty
  "chainwork" : "xxxx",       (string) total chainwork
  "previousblockhash" : "hash",   (string) hash of previous block
  "nextblockhash" : "hash"        (string) hash of next block
}

Examples:
> dinero-cli getblockheader "00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09"
> curl --user $(cat ~/.dinero/.cookie) --data-binary '{"jsonrpc":"1.0","id":"1","method":"getblockheader","params":["00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09"]}' -H 'content-type:text/plain;' http://127.0.0.1:20998/
)";

        ::g_rpcRegistry.registerHandler("blockchain.getblockheader", rpc_context_getblockheader, meta,
                                     RegisterMode::Overwrite, "blockchain_meta");
        ::g_rpcRegistry.registerAlias("getblockheader", "blockchain.getblockheader");
    }

    // =========================================================================
    // getblockchaininfo - Get comprehensive blockchain information
    // =========================================================================
    {
        RpcMethodMeta meta;
        meta.name = "getblockchaininfo";
        meta.ns = "blockchain";
        meta.description = "Returns comprehensive information about the current state of the blockchain";

        // No parameters
        meta.params = {};

        // Return value
        meta.result.type = "object";
        meta.result.desc = "Comprehensive blockchain status information";

        // Help text
        meta.help = R"(getblockchaininfo

Returns an object containing various state info regarding blockchain processing.

Result:
{
  "chain": "xxxx",                (string) current network name (main, test, regtest)
  "blocks": xxxxxx,               (number) the height of the most-work fully-validated chain
  "headers": xxxxxx,              (number) the current number of headers we have validated
  "bestblockhash": "...",         (string) the hash of the currently best block
  "difficulty": xxxxxx,           (number) the current difficulty
  "mediantime": xxxxxx,           (number) median time for the current best block
  "verificationprogress": xxxx,   (number) estimate of verification progress [0..1]
  "initialblockdownload": xxxx,   (boolean) whether we're in initial block download
  "chainwork": "xxxx",            (string) total chainwork in the best chain (hex)
  "size_on_disk": xxxxxx,         (number) the estimated size of the block and undo files on disk
  "pruned": xx,                   (boolean) whether blockchain is pruned
  "softforks": { ... },           (object) status of softforks
  "bip9_softforks": { ... }       (object) status of BIP9 softforks
}

Examples:
> dinero-cli getblockchaininfo
> curl --user $(cat ~/.dinero/.cookie) --data-binary '{"jsonrpc":"1.0","id":"1","method":"getblockchaininfo","params":[]}' -H 'content-type:text/plain;' http://127.0.0.1:20998/
)";

        ::g_rpcRegistry.registerHandler("blockchain.getblockchaininfo", rpc_context_getblockchaininfo, meta,
                                     RegisterMode::Overwrite, "blockchain_meta");
        ::g_rpcRegistry.registerAlias("getblockchaininfo", "blockchain.getblockchaininfo");
    }

    // =========================================================================
    // getdifficulty - Get current mining difficulty
    // =========================================================================
    {
        RpcMethodMeta meta;
        meta.name = "getdifficulty";
        meta.ns = "blockchain";
        meta.description = "Returns the current proof-of-work difficulty";

        // No parameters
        meta.params = {};

        // Return value
        meta.result.type = "number";
        meta.result.desc = "The current mining difficulty (minimum 1.0)";

        // Help text
        meta.help = R"(getdifficulty

Returns the proof-of-work difficulty as a multiple of the minimum difficulty.

Result:
n.nnn    (number) the proof-of-work difficulty as a multiple of the minimum difficulty

Examples:
> dinero-cli getdifficulty
> curl --user $(cat ~/.dinero/.cookie) --data-binary '{"jsonrpc":"1.0","id":"1","method":"getdifficulty","params":[]}' -H 'content-type:text/plain;' http://127.0.0.1:20998/
)";

        ::g_rpcRegistry.registerHandler("blockchain.getdifficulty", rpc_context_getdifficulty, meta,
                                     RegisterMode::Overwrite, "blockchain_meta");
        ::g_rpcRegistry.registerAlias("getdifficulty", "blockchain.getdifficulty");
    }

    // =========================================================================
    // getbestblockhash - Get hash of the tip block
    // =========================================================================
    {
        RpcMethodMeta meta;
        meta.name = "getbestblockhash";
        meta.ns = "blockchain";
        meta.description = "Returns the hash of the best (tip) block in the most-work fully-validated chain";

        // No parameters
        meta.params = {};

        // Return value
        meta.result.type = "string";
        meta.result.desc = "The block hash of the chain tip (64-character hex string)";

        // Help text
        meta.help = R"(getbestblockhash

Returns the hash of the best (tip) block in the most-work fully-validated chain.

Result:
"hex"      (string) the block hash, hex-encoded

Examples:
> dinero-cli getbestblockhash
> curl --user $(cat ~/.dinero/.cookie) --data-binary '{"jsonrpc":"1.0","id":"1","method":"getbestblockhash","params":[]}' -H 'content-type:text/plain;' http://127.0.0.1:20998/
)";

        ::g_rpcRegistry.registerHandler("blockchain.getbestblockhash", rpc_context_getbestblockhash, meta,
                                     RegisterMode::Overwrite, "blockchain_meta");
        ::g_rpcRegistry.registerAlias("getbestblockhash", "blockchain.getbestblockhash");
    }
}

} // namespace rpc
} // namespace dinero

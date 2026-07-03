// SPDX-License-Identifier: MIT
// Dinero - Blockchain RPC Handler Implementation

#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "consensus/block_validation.h"
#include "consensus/header_sync_manager.h"
#include "consensus/chainparams.h"
#include "consensus/target_helpers.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "storage/chain_db.h"
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace dinero {
namespace rpc {

namespace {

Json::Value RpcError(int code, const std::string& message) {
    Json::Value error;
    error["error"]["code"] = code;
    error["error"]["message"] = message;
    return error;
}

std::string BytesToHex(const std::string& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0f]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

std::string FormatBitsHex(uint32_t bits) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(8) << bits;
    return ss.str();
}

std::shared_ptr<ChainstateService> RequireChainstate() {
    auto* ctx = DaemonContext::instance();
    if (!ctx || !ctx->chainstate) {
        throw std::runtime_error("Chainstate service not available");
    }
    return ctx->chainstate;
}

ChainDB* RequireChainDB(const std::shared_ptr<ChainstateService>& chainstate) {
    ChainDB* chain_db = chainstate->GetChainDB();
    if (!chain_db) {
        throw std::runtime_error("ChainDB not available");
    }
    return chain_db;
}

uint256 ParseBlockHashOrThrow(const std::string& block_hash_hex) {
    uint256 block_hash;
    if (!uint256::FromHex(block_hash_hex, block_hash)) {
        throw std::runtime_error("Invalid block hash hex");
    }
    return block_hash;
}

Json::Value BuildVerboseBlock(ChainDB* chain_db, const uint256& hash, const Block& block) {
    Json::Value result(Json::objectValue);
    result["hash"] = hash.GetHex();

    auto height_result = chain_db->getBlockHeight(hash);
    if (height_result.ok()) {
        result["height"] = height_result.value();
    } else {
        result["height"] = 0;
    }

    result["version"] = static_cast<Json::Int64>(block.header.version);
    result["merkleroot"] = block.header.merkle_root.GetHex();
    result["time"] = static_cast<Json::UInt64>(block.header.timestamp);
    result["nonce"] = static_cast<Json::UInt64>(block.header.nonce);
    result["bits"] = FormatBitsHex(block.header.difficulty);
    result["difficulty"] = ::dinero::DifficultyFromBits(block.header.difficulty, dinero::Params().pow_limit_bits);

    auto work_result = chain_db->getBlockWork(hash);
    if (work_result.ok()) {
        result["chainwork"] = work_result.value().GetHex();
    } else {
        result["chainwork"] = std::string(64, '0');
    }

    if (!block.header.prev_block_hash.IsNull()) {
        result["previousblockhash"] = block.header.prev_block_hash.GetHex();
    } else {
        result["previousblockhash"] = Json::Value(Json::nullValue);
    }

    if (height_result.ok()) {
        auto next_hash_result = chain_db->getBlockHashByHeight(height_result.value() + 1);
        if (next_hash_result.ok()) {
            result["nextblockhash"] = next_hash_result.value().GetHex();
        } else {
            result["nextblockhash"] = Json::Value(Json::nullValue);
        }
    } else {
        result["nextblockhash"] = Json::Value(Json::nullValue);
    }

    Json::Value tx_array(Json::arrayValue);
    for (const auto& tx : block.vtx) {
        tx_array.append(tx.GetTxid().AsUint256().GetHex());
    }
    result["tx"] = tx_array;

    return result;
}

Json::Value BuildVerboseHeader(ChainDB* chain_db, const uint256& hash, const BlockHeader& header) {
    Json::Value result(Json::objectValue);
    result["hash"] = hash.GetHex();

    auto height_result = chain_db->getBlockHeight(hash);
    if (height_result.ok()) {
        result["height"] = height_result.value();
    } else {
        result["height"] = 0;
    }

    result["version"] = static_cast<Json::Int64>(header.version);
    result["merkleroot"] = header.merkle_root.GetHex();
    result["time"] = static_cast<Json::UInt64>(header.timestamp);
    result["nonce"] = static_cast<Json::UInt64>(header.nonce);
    result["bits"] = FormatBitsHex(header.difficulty);
    result["difficulty"] = ::dinero::DifficultyFromBits(header.difficulty, dinero::Params().pow_limit_bits);

    auto work_result = chain_db->getBlockWork(hash);
    if (work_result.ok()) {
        result["chainwork"] = work_result.value().GetHex();
    } else {
        result["chainwork"] = std::string(64, '0');
    }

    if (!header.prev_block_hash.IsNull()) {
        result["previousblockhash"] = header.prev_block_hash.GetHex();
    } else {
        result["previousblockhash"] = Json::Value(Json::nullValue);
    }

    if (height_result.ok()) {
        auto next_hash_result = chain_db->getBlockHashByHeight(height_result.value() + 1);
        if (next_hash_result.ok()) {
            result["nextblockhash"] = next_hash_result.value().GetHex();
        } else {
            result["nextblockhash"] = Json::Value(Json::nullValue);
        }
    } else {
        result["nextblockhash"] = Json::Value(Json::nullValue);
    }

    return result;
}

}  // namespace

// Get block count RPC handler
Json::Value rpc_getblockcount(const Json::Value& params) {
    (void)params;
    try {
        const auto chainstate = RequireChainstate();
        const uint32_t block_count = chainstate->getBlockHeight();
        dinero::g_logger.info("Block count requested: " + std::to_string(block_count));
        return Json::Value(static_cast<Json::UInt>(block_count));
    } catch (const std::exception& e) {
        return RpcError(-1, std::string("Get block count error: ") + e.what());
    }
}

// Get block hash RPC handler
Json::Value rpc_getblockhash(const Json::Value& params) {
    try {
        if (params.size() < 1 || !params[0].isNumeric()) {
            return RpcError(-1, "Height parameter required");
        }

        const int height = params[0].asInt();
        if (height < 0) {
            return RpcError(-1, "Height must be non-negative");
        }

        const auto chainstate = RequireChainstate();
        ChainDB* chain_db = RequireChainDB(chainstate);
        auto hash_result = chain_db->getBlockHashByHeight(height);
        if (!hash_result.ok()) {
            return RpcError(-8, "Block height out of range");
        }

        const std::string block_hash = hash_result.value().GetHex();
        dinero::g_logger.info("Block hash requested for height: " + std::to_string(height));
        return Json::Value(block_hash);
    } catch (const std::exception& e) {
        return RpcError(-1, std::string("Get block hash error: ") + e.what());
    }
}

// Get block RPC handler
Json::Value rpc_getblock(const Json::Value& params) {
    try {
        if (params.size() < 1 || !params[0].isString()) {
            return RpcError(-1, "Block hash parameter required");
        }

        const std::string block_hash_hex = params[0].asString();
        bool verbose = true;
        if (params.size() > 1 && params[1].isBool()) {
            verbose = params[1].asBool();
        }

        const auto chainstate = RequireChainstate();
        ChainDB* chain_db = RequireChainDB(chainstate);
        const uint256 block_hash = ParseBlockHashOrThrow(block_hash_hex);

        auto block_result = chainstate->getBlockByHash(block_hash);
        if (!block_result.ok()) {
            return RpcError(-5, "Block not found");
        }

        const Block& block = block_result.value();
        Json::Value result = verbose
            ? BuildVerboseBlock(chain_db, block_hash, block)
            : Json::Value(BytesToHex(block.Serialize()));

        dinero::g_logger.info("Block requested: " + block_hash_hex + " (verbose: " + std::to_string(verbose) + ")");
        return result;
    } catch (const std::exception& e) {
        return RpcError(-1, std::string("Get block error: ") + e.what());
    }
}

// Get blockchain info RPC handler
Json::Value rpc_getblockchaininfo(const Json::Value& params) {
    try {
        const auto chainstate = RequireChainstate();
        ChainDB* chain_db = RequireChainDB(chainstate);

        Json::Value result(Json::objectValue);
        result["chain"] = dinero::Params().name;

        const uint32_t blocks = chainstate->getBlockHeight();
        result["blocks"] = static_cast<Json::UInt>(blocks);
        result["bestblockhash"] = chainstate->getBestBlockHash();

        auto tip_result = chain_db->getTip();
        if (tip_result.ok()) {
            result["chainwork"] = "0x" + tip_result.value().work.GetHex();
            result["mediantime"] = static_cast<Json::UInt64>(tip_result.value().timestamp);

            auto header_result = chain_db->getHeader(tip_result.value().hash);
            if (header_result.ok()) {
                result["difficulty"] = ::dinero::DifficultyFromBits(
                    header_result.value().difficulty, dinero::Params().pow_limit_bits);
            } else {
                result["difficulty"] = 1.0;
            }
        } else {
            result["chainwork"] = "0x" + std::string(64, '0');
            result["mediantime"] = static_cast<Json::UInt64>(0);
            result["difficulty"] = 1.0;
        }

        uint32_t headers = blocks;
        bool initial_block_download = chainstate->IsInIBD();
        if (dinero::g_header_sync_manager) {
            headers = static_cast<uint32_t>(dinero::g_header_sync_manager->GetBestHeaderHeight());
            initial_block_download = dinero::g_header_sync_manager->IsInitialBlockDownload();
        }
        result["headers"] = static_cast<Json::UInt>(headers);
        result["initialblockdownload"] = initial_block_download;

        double verification_progress = 1.0;
        if (headers > 0) {
            verification_progress = static_cast<double>(blocks) / static_cast<double>(headers);
            if (verification_progress > 1.0) {
                verification_progress = 1.0;
            }
        }
        result["verificationprogress"] = verification_progress;

        result["size_on_disk"] = static_cast<Json::UInt64>(chainstate->GetBlockchainDiskUsage()) * 1024ULL * 1024ULL;
        result["pruned"] = chainstate->GetPruningInfo().pruning_enabled;
        result["softforks"] = Json::Value(Json::objectValue);
        result["bip9_softforks"] = Json::Value(Json::objectValue);

        // Consensus-validation transparency.
        //
        // A STATELESS (utreexo) validator structurally CANNOT independently
        // validate the coinbase-maturity rule: the Utreexo leaf commits to
        // neither the creating height nor an is_coinbase flag. Such a node
        // DEFERS that single rule to consensus / most-work instead of vouching
        // a block as fully consensus-valid. Surface that here so callers /
        // wallets never treat a stateless node as a full independent validator
        // of maturity. (Durable fix: future leaf-format hard fork — see
        // FOLLOW-UP in consensus/block_validation.cpp.)
        {
            Json::Value cval(Json::objectValue);
            const auto* bv = chainstate->GetBlockValidator();
            const bool stateless = bv &&
                bv->getValidationMode() == consensus::ValidationMode::STATELESS;
            cval["mode"] = stateless ? "stateless" : "stateful";
            cval["coinbase_maturity_independently_validated"] = !stateless;
            cval["coinbase_maturity_status"] =
                stateless ? "deferred-to-consensus" : "enforced";
            // True once a stateless spend-block has been processed this session
            // without independent maturity validation (latches).
            cval["coinbase_maturity_deferral_observed"] =
                bv ? bv->statelessMaturityUnverified() : false;
            result["consensus_validation"] = cval;
        }

        dinero::g_logger.info("Blockchain info requested");
        return result;
    } catch (const std::exception& e) {
        return RpcError(-1, std::string("Get blockchain info error: ") + e.what());
    }
}

// Get block header RPC handler
Json::Value rpc_getblockheader(const Json::Value& params) {
    try {
        if (params.size() < 1 || !params[0].isString()) {
            return RpcError(-1, "Block hash parameter required");
        }

        const std::string block_hash_hex = params[0].asString();
        bool verbose = true;
        if (params.size() > 1 && params[1].isBool()) {
            verbose = params[1].asBool();
        }

        const auto chainstate = RequireChainstate();
        ChainDB* chain_db = RequireChainDB(chainstate);
        const uint256 block_hash = ParseBlockHashOrThrow(block_hash_hex);

        auto header_result = chain_db->getHeader(block_hash);
        if (!header_result.ok()) {
            return RpcError(-5, "Block header not found");
        }

        const BlockHeader& header = header_result.value();
        Json::Value result = verbose
            ? BuildVerboseHeader(chain_db, block_hash, header)
            : Json::Value(BytesToHex(header.Serialize()));

        dinero::g_logger.info("Block header requested: " + block_hash_hex + " (verbose: " + std::to_string(verbose) + ")");
        return result;
    } catch (const std::exception& e) {
        return RpcError(-1, std::string("Get block header error: ") + e.what());
    }
}

// Get difficulty RPC handler
Json::Value rpc_getdifficulty(const Json::Value& params) {
    (void)params;
    try {
        const auto chainstate = RequireChainstate();
        ChainDB* chain_db = RequireChainDB(chainstate);
        auto tip_result = chain_db->getTip();
        if (!tip_result.ok()) {
            return Json::Value(1.0);
        }

        auto header_result = chain_db->getHeader(tip_result.value().hash);
        if (!header_result.ok()) {
            return Json::Value(1.0);
        }

        const double difficulty = ::dinero::DifficultyFromBits(
            header_result.value().difficulty, dinero::Params().pow_limit_bits);
        dinero::g_logger.info("Difficulty requested: " + std::to_string(difficulty));
        return Json::Value(difficulty);
    } catch (const std::exception& e) {
        return RpcError(-1, std::string("Get difficulty error: ") + e.what());
    }
}

} // namespace rpc
} // namespace dinero

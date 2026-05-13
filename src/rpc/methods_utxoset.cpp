/**
 * UTXO Set RPC Methods
 *
 * Implements:
 *   - gettxoutsetinfo: UTXO set statistics and hash
 *   - scantxoutset: Scan for UTXOs matching an address (Address Explorer)
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "consensus/script_interpreter.h"
#include "storage/chain_db.h"
#include "common/logger.h"
#include "daemon/bech32_decode.h"
#include <vector>
#include <unordered_set>
#include <sstream>
#include <iomanip>

namespace din {
using ExecutionContext = ::ExecutionContext;
using dinero::uint256;

// Helper: convert hex string to bytes
static std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

/**
 * Compute UTXO set hash using XOR + SHA256 (order-independent)
 */
struct UTXOSetHash {
    std::vector<uint8_t> xor_hash;

    UTXOSetHash() : xor_hash(32, 0) {}

    void add(const std::string& txid_hex, uint32_t vout, int height,
             bool is_coinbase, uint64_t value, const std::string& script_hex) {
        std::vector<uint8_t> serialized;

        // txid bytes from hex
        auto txid_bytes = hexToBytes(txid_hex);
        serialized.insert(serialized.end(), txid_bytes.begin(), txid_bytes.end());

        // vout (4 bytes LE)
        serialized.push_back(vout & 0xFF);
        serialized.push_back((vout >> 8) & 0xFF);
        serialized.push_back((vout >> 16) & 0xFF);
        serialized.push_back((vout >> 24) & 0xFF);

        // height (4 bytes LE)
        uint32_t h = static_cast<uint32_t>(height);
        serialized.push_back(h & 0xFF);
        serialized.push_back((h >> 8) & 0xFF);
        serialized.push_back((h >> 16) & 0xFF);
        serialized.push_back((h >> 24) & 0xFF);

        // coinbase flag
        serialized.push_back(is_coinbase ? 1 : 0);

        // value (8 bytes LE)
        serialized.push_back(value & 0xFF);
        serialized.push_back((value >> 8) & 0xFF);
        serialized.push_back((value >> 16) & 0xFF);
        serialized.push_back((value >> 24) & 0xFF);
        serialized.push_back((value >> 32) & 0xFF);
        serialized.push_back((value >> 40) & 0xFF);
        serialized.push_back((value >> 48) & 0xFF);
        serialized.push_back((value >> 56) & 0xFF);

        // scriptPubKey bytes from hex
        auto script_bytes = hexToBytes(script_hex);
        serialized.insert(serialized.end(), script_bytes.begin(), script_bytes.end());

        // SHA256 and XOR into accumulator
        auto utxo_hash = dinero::consensus::SHA256_Hash(serialized);
        for (size_t i = 0; i < 32 && i < utxo_hash.size(); i++) {
            xor_hash[i] ^= utxo_hash[i];
        }
    }

    std::string finalize() {
        auto final_hash = dinero::consensus::SHA256_Hash(xor_hash);
        std::ostringstream oss;
        for (uint8_t byte : final_hash) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        return oss.str();
    }
};

/**
 * blockchain.gettxoutsetinfo - Get UTXO set statistics and hash
 */
Json rpc_gettxoutsetinfo(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Daemon/chainstate not available";
            return result;
        }

        dinero::ChainDB* chain_db = ctx.daemon->chainstate->GetChainDB();
        if (!chain_db) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not initialized";
            return result;
        }

        // Parse hash_type parameter
        std::string hash_type = "hash_serialized_2";
        if (params.isArray() && params.size() > 0 && params[0].isString()) {
            hash_type = params[0].asString();
            if (hash_type != "none" && hash_type != "hash_serialized_2") {
                result["error"]["code"] = -8;
                result["error"]["message"] = "Invalid hash_type";
                return result;
            }
        }

        // Get current tip
        auto tip_result = chain_db->getTip();
        if (!tip_result.ok()) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Failed to get chain tip";
            return result;
        }

        const auto& tip = tip_result.value();
        int height = tip.height;
        std::string bestblock = tip.hash.GetHex();

        dinero::g_logger.info("[gettxoutsetinfo] Computing at height " + std::to_string(height));

        uint64_t txout_count = 0;
        std::unordered_set<std::string> tx_set;
        uint64_t total_amount = 0;
        UTXOSetHash utxo_hash;
        bool compute_hash = (hash_type != "none");

        // Iterate all UTXOs via ChainDB
        auto status = chain_db->forEachUTXO(
            [&](const uint256& txid, uint32_t vout, const dinero::Coin& coin) -> bool {
                txout_count++;
                std::string txid_hex = txid.GetHex();
                tx_set.insert(txid_hex);
                total_amount += coin.amount;

                if (compute_hash) {
                    utxo_hash.add(txid_hex, vout, coin.height,
                                  coin.coinbase, coin.amount, coin.script_pubkey);
                }
                return true; // continue iteration
            });

        if (status != dinero::Status::Ok) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Failed to iterate UTXO set";
            return result;
        }

        dinero::g_logger.info("[gettxoutsetinfo] Processed " + std::to_string(txout_count) + " UTXOs");

        result["height"] = height;
        result["bestblock"] = bestblock;
        result["transactions"] = static_cast<::Json::Value::Int64>(tx_set.size());
        result["txouts"] = static_cast<::Json::Value::Int64>(txout_count);
        result["bogosize"] = static_cast<::Json::Value::Int64>(txout_count * 200);
        result["total_amount_una"] = static_cast<::Json::Value::Int64>(total_amount);

        double din_amount = static_cast<double>(total_amount) / 100000000.0;
        std::ostringstream din_str;
        din_str << std::fixed << std::setprecision(8) << din_amount;
        result["total_amount_din"] = din_str.str();

        if (compute_hash) {
            std::string set_hash = utxo_hash.finalize();
            result["hash_serialized_2"] = set_hash;
        }

        result["disk_size"] = static_cast<::Json::Value::Int64>(txout_count * 200);

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
        dinero::g_logger.error("[gettxoutsetinfo] Exception: " + std::string(e.what()));
    }

    return result;
}

/**
 * blockchain.scantxoutset - Scan UTXO set for outputs matching an address
 *
 * Params: [0] address (string, required): Bech32m address (e.g. "din1p...")
 * Returns: { address, script, height, utxos[], total_count, total_amount, total_din }
 */
Json rpc_scantxoutset(const ExecutionContext& ctx, const Json& params) {
    Json result;

    try {
        if (!params.isArray() || params.size() < 1 || !params[0].isString()) {
            result["error"]["code"] = -8;
            result["error"]["message"] = "Missing required parameter: address (string)";
            return result;
        }

        std::string address = params[0].asString();

        // Decode bech32m address → witness program
        int witver = -1;
        std::vector<uint8_t> witprog;
        std::string hrp = dinero::mining::GetBech32HRP();

        if (!dinero::mining::Bech32DecodeSegwit(address, hrp, witver, witprog)) {
            result["error"]["code"] = -5;
            result["error"]["message"] = "Invalid address: failed to decode bech32";
            return result;
        }

        // Build target scriptPubKey hex string for comparison with Coin.script_pubkey
        // P2TR (v1): OP_1 (0x51) + PUSH32 (0x20) + <32-byte witness program>
        // P2WPKH (v0): OP_0 (0x00) + PUSH20 (0x14) + <20-byte witness program>
        std::ostringstream target_hex;
        uint8_t op = witver == 0 ? 0x00 : static_cast<uint8_t>(0x50 + witver);
        uint8_t push_len = static_cast<uint8_t>(witprog.size());
        target_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(op);
        target_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(push_len);
        for (uint8_t b : witprog) {
            target_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        }
        std::string target_script = target_hex.str();

        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Daemon/chainstate not available";
            return result;
        }

        dinero::ChainDB* chain_db = ctx.daemon->chainstate->GetChainDB();
        if (!chain_db) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "ChainDB not initialized";
            return result;
        }

        // Get tip height for result
        int tip_height = 0;
        auto tip_result = chain_db->getTip();
        if (tip_result.ok()) {
            tip_height = tip_result.value().height;
        }

        // Scan UTXO set for matching scriptPubKeys
        ::Json::Value utxos_arr(::Json::arrayValue);
        uint64_t total_amount = 0;
        uint64_t total_count = 0;

        auto status = chain_db->forEachUTXO(
            [&](const uint256& txid, uint32_t vout, const dinero::Coin& coin) -> bool {
                if (coin.script_pubkey == target_script) {
                    Json entry;
                    entry["txid"] = txid.GetHex();
                    entry["vout"] = vout;
                    entry["amount"] = static_cast<::Json::Value::Int64>(coin.amount);
                    entry["height"] = coin.height;
                    entry["coinbase"] = coin.coinbase;
                    entry["script"] = coin.script_pubkey;
                    entry["script_pubkey"] = coin.script_pubkey;
                    utxos_arr.append(entry);
                    total_amount += coin.amount;
                    total_count++;
                }
                return true; // continue
            });

        if (status != dinero::Status::Ok) {
            result["error"]["code"] = -1;
            result["error"]["message"] = "Failed to scan UTXO set";
            return result;
        }

        // Format DIN amount
        double din_amount = static_cast<double>(total_amount) / 100000000.0;
        std::ostringstream din_str;
        din_str << std::fixed << std::setprecision(8) << din_amount;

        result["address"] = address;
        result["script"] = target_script;
        result["height"] = tip_height;
        result["utxos"] = utxos_arr;
        result["total_count"] = static_cast<::Json::Value::Int64>(total_count);
        result["total_amount"] = static_cast<::Json::Value::Int64>(total_amount);
        result["total_din"] = din_str.str();

    } catch (const std::exception& e) {
        result["error"]["code"] = -1;
        result["error"]["message"] = std::string("Exception: ") + e.what();
    }

    return result;
}

} // namespace din

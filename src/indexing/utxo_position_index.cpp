/**
 * Phase 11a: Global UTXO Position Index Implementation
 *
 * This index enables O(1) Utreexo proof generation for any UTXO.
 * It is non-consensus, rebuildable, and lives in the indexing layer.
 */

#include "indexing/utxo_position_index.h"
#include "common/logger.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "storage/chain_db.h"
#include <algorithm>

namespace dinero {
namespace indexing {

UTXOPositionIndex::UTXOPositionIndex() {
    // Reserve space for typical UTXO set size (~100k-1M UTXOs)
    position_map_.reserve(100000);
}

UTXOPositionIndex::~UTXOPositionIndex() {
    // Cleanup handled by std::unordered_map destructor
}

void UTXOPositionIndex::AddPosition(const TxId& txid, uint32_t vout, uint64_t position) {
    std::lock_guard<std::mutex> lock(index_mutex_);

    auto key = std::make_pair(txid, vout);

    // Check if position already exists (shouldn't happen in normal operation)
    auto it = position_map_.find(key);
    if (it != position_map_.end()) {
        g_logger.warn("[UTXOPositionIndex] Overwriting existing position for " +
                     txid.AsUint256().GetHex() + ":" + std::to_string(vout) +
                     " (old: " + std::to_string(it->second) +
                     ", new: " + std::to_string(position) + ")");
    }

    position_map_[key] = position;
}

std::optional<uint64_t> UTXOPositionIndex::RemovePosition(const TxId& txid, uint32_t vout) {
    std::lock_guard<std::mutex> lock(index_mutex_);

    auto key = std::make_pair(txid, vout);
    auto it = position_map_.find(key);

    if (it == position_map_.end()) {
        // UTXO not tracked (possible if index was disabled or rebuilt)
        return std::nullopt;
    }

    uint64_t position = it->second;
    position_map_.erase(it);
    return position;
}

std::optional<uint64_t> UTXOPositionIndex::GetPosition(const TxId& txid, uint32_t vout) const {
    std::lock_guard<std::mutex> lock(index_mutex_);

    auto key = std::make_pair(txid, vout);
    auto it = position_map_.find(key);

    if (it == position_map_.end()) {
        return std::nullopt;
    }

    return it->second;
}

bool UTXOPositionIndex::HasPosition(const TxId& txid, uint32_t vout) const {
    std::lock_guard<std::mutex> lock(index_mutex_);

    auto key = std::make_pair(txid, vout);
    return position_map_.find(key) != position_map_.end();
}

size_t UTXOPositionIndex::GetPositionCount() const {
    std::lock_guard<std::mutex> lock(index_mutex_);
    return position_map_.size();
}

void UTXOPositionIndex::Clear() {
    std::lock_guard<std::mutex> lock(index_mutex_);
    position_map_.clear();
}

UTXOPositionRebuildReport UTXOPositionIndex::Rebuild(const ChainDB& chain_db,
                                                     const consensus::UtreexoForest& forest) {
    UTXOPositionRebuildReport report;
    std::unordered_map<std::pair<TxId, uint32_t>, uint64_t, OutPointHash> rebuilt_positions;
    rebuilt_positions.reserve(static_cast<size_t>(forest.getNumLeaves()));

    auto status = chain_db.forEachUTXO(
        [&](const uint256& txid_u256, uint32_t vout, const Coin& coin) -> bool {
            if ((coin.script_pubkey.size() % 2) != 0) {
                report.malformed++;
                return true;
            }

            std::vector<uint8_t> script_pubkey;
            script_pubkey.reserve(coin.script_pubkey.size() / 2);
            for (size_t i = 0; i < coin.script_pubkey.size(); i += 2) {
                try {
                    script_pubkey.push_back(
                        static_cast<uint8_t>(std::stoul(coin.script_pubkey.substr(i, 2), nullptr, 16)));
                } catch (...) {
                    report.malformed++;
                    return true;
                }
            }

            if (!script_pubkey.empty() && script_pubkey[0] == 0x6a) {
                report.skipped_unspendable++;
                return true;
            }

            auto leaf_hash = consensus::HashUTXOForCreationHeight(
                txid_u256,
                vout,
                coin.amount,
                script_pubkey,
                static_cast<uint32_t>(coin.height),
                coin.coinbase
            );
            auto position = forest.findLeafPosition(leaf_hash);
            if (!position.has_value()) {
                report.missing++;
                return true;
            }

            rebuilt_positions[std::make_pair(TxId(txid_u256), vout)] = *position;
            report.matched++;
            return true;
        });

    if (status != Status::Ok) {
        g_logger.error("[UTXOPositionIndex] Rebuild failed while iterating ChainDB UTXOs");
        return report;
    }

    if (report.malformed > 0) {
        g_logger.error("[UTXOPositionIndex] Rebuild found " + std::to_string(report.malformed) +
                       " malformed UTXO script(s)");
        return report;
    }

    if (report.missing > 0) {
        g_logger.warn("[UTXOPositionIndex] Rebuild: " + std::to_string(report.missing) +
                      " UTXO(s) missing from forest (proof serving degraded, not fatal)");
    }

    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        position_map_.swap(rebuilt_positions);
    }

    report.success = true;

    g_logger.info("[UTXOPositionIndex] Rebuilt " + std::to_string(report.matched) +
                  " position entries from ChainDB + forest" +
                  (report.skipped_unspendable > 0
                       ? " (skipped " + std::to_string(report.skipped_unspendable) + " provably unspendable output(s))"
                       : ""));
    return report;
}

} // namespace indexing
} // namespace dinero

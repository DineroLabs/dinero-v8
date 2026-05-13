// ============================================================================
// STORAGE LAYER - PERSISTENT UTXO ADAPTER IMPLEMENTATION
// ============================================================================
//
// Phase 2: Pure Consensus Architecture
//
// Bridges pure consensus state to ChainDB persistent storage.
//
// ============================================================================

#include "storage/persistent_utxo_adapter.h"
#include <iostream>

namespace dinero {
namespace storage {

PersistentUTXOAdapter::PersistentUTXOAdapter(ChainDB& db, ChainWriteToken& token)
    : db_(db), token_(token) {}

// =============================================================================
// Initialization
// =============================================================================

bool PersistentUTXOAdapter::LoadInitialState(consensus::ConsensusUTXOSet& consensus_set) {
    // Clear existing state
    consensus_set.Clear();

    // Get tip information
    auto tip_result = db_.getTip();
    if (!tip_result.ok()) {
        std::cerr << "WARNING: No tip found in ChainDB (empty chain)" << std::endl;
        return true;  // Empty chain is valid for genesis
    }

    const TipInfo& tip = tip_result.value();
    uint32_t height = tip.height;
    uint256 best_block = tip.hash;

    // Collect UTXOs for bulk load
    std::unordered_map<OutPoint, consensus::UTXOEntry> utxos;
    size_t loaded_count = 0;

    auto callback = [&](const uint256& txid, uint32_t vout, const Coin& coin) -> bool {
        OutPoint outpoint(TxId(txid), vout);
        Coin hydrated_coin = coin;
        auto hydrated_result = db_.getCoinWithConfidentialFallback(txid, vout);
        if (hydrated_result.ok()) {
            hydrated_coin = hydrated_result.value();
        }
        consensus::UTXOEntry entry = FromDbCoin(hydrated_coin, hydrated_coin.height);
        utxos[outpoint] = entry;
        loaded_count++;

        // Progress logging
        if (loaded_count % 100000 == 0) {
            std::cout << "Loaded " << loaded_count << " UTXOs..." << std::endl;
        }

        return true;  // Continue iteration
    };

    Status status = db_.forEachUTXO(callback);
    if (status != Status::Ok) {
        std::cerr << "ERROR: Failed to iterate UTXOs: " << StatusToString(status) << std::endl;
        return false;
    }

    // Bulk load into consensus set
    if (!consensus_set.BulkLoad(utxos, height, best_block)) {
        std::cerr << "ERROR: Failed to bulk-load Utreexo forest from persisted UTXO set" << std::endl;
        return false;
    }

    std::cout << "Loaded " << loaded_count << " UTXOs from ChainDB (height " << height << ")" << std::endl;
    return true;
}

// =============================================================================
// Persistence
// =============================================================================

bool PersistentUTXOAdapter::CommitState(const consensus::ConsensusUTXOSet& consensus_set,
                                        rocksdb::WriteBatch* batch) {
    // Get all UTXOs from consensus set
    const auto& utxos = consensus_set.GetUTXOs();

    // Note: Full state commit is expensive. Use CommitDelta for incremental updates.
    // This is primarily for initialization/snapshot scenarios.

    for (const auto& [outpoint, entry] : utxos) {
        Coin coin = ToDbCoin(entry);
        Status status = db_.putCoin(token_, outpoint.txid.AsUint256(), outpoint.vout, coin, batch);
        if (status != Status::Ok) {
            std::cerr << "ERROR: Failed to commit UTXO: " << StatusToString(status) << std::endl;
            return false;
        }
    }

    return true;
}

bool PersistentUTXOAdapter::CommitDelta(
    const std::vector<std::pair<OutPoint, consensus::UTXOEntry>>& added,
    const std::vector<OutPoint>& removed,
    rocksdb::WriteBatch* batch) {

    // Write added UTXOs
    for (const auto& [outpoint, entry] : added) {
        Coin coin = ToDbCoin(entry);
        Status status = db_.putCoin(token_, outpoint.txid.AsUint256(), outpoint.vout, coin, batch);
        if (status != Status::Ok) {
            std::cerr << "ERROR: Failed to add UTXO: " << StatusToString(status) << std::endl;
            return false;
        }
    }

    // Delete removed UTXOs
    for (const auto& outpoint : removed) {
        Status status = db_.deleteCoin(token_, outpoint.txid.AsUint256(), outpoint.vout, batch);
        if (status != Status::Ok) {
            std::cerr << "ERROR: Failed to delete UTXO: " << StatusToString(status) << std::endl;
            return false;
        }
    }

    return true;
}

bool PersistentUTXOAdapter::Flush() {
    // ChainDB handles fsync internally
    // This is a placeholder for future optimization (batched flushing)
    return true;
}

// =============================================================================
// State Queries
// =============================================================================

StatusOr<TipInfo> PersistentUTXOAdapter::GetTip() const {
    return db_.getTip();
}

size_t PersistentUTXOAdapter::GetPersistentUTXOCount() const {
    size_t count = 0;
    db_.forEachUTXO([&count](const uint256&, uint32_t, const Coin&) -> bool {
        count++;
        return true;
    });
    return count;
}

// =============================================================================
// Type Conversion
// =============================================================================

Coin PersistentUTXOAdapter::ToDbCoin(const consensus::UTXOEntry& entry) {
    Coin coin;
    coin.amount = entry.value.GetUna();
    // Encode scriptPubKey as hex string (matches BlockAcceptor::ConnectBlock format)
    coin.script_pubkey.reserve(entry.scriptPubKey.size() * 2);
    for (uint8_t byte : entry.scriptPubKey) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", byte);
        coin.script_pubkey += buf;
    }
    coin.height = static_cast<int>(entry.height);
    coin.coinbase = entry.isCoinbase;
    coin.is_confidential = entry.is_confidential;
    coin.commitment = entry.commitment;
    return coin;
}

consensus::UTXOEntry PersistentUTXOAdapter::FromDbCoin(const Coin& coin, uint32_t height) {
    consensus::UTXOEntry entry;
    entry.value = AmountUna::Una(coin.amount);
    // Decode hex string → raw bytes (BlockAcceptor stores scriptPubKey as hex)
    const std::string& hex = coin.script_pubkey;
    entry.scriptPubKey.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16));
        entry.scriptPubKey.push_back(byte);
    }
    entry.height = height;
    entry.isCoinbase = coin.coinbase;
    entry.is_confidential = coin.is_confidential;
    entry.commitment = coin.commitment;
    return entry;
}

} // namespace storage
} // namespace dinero

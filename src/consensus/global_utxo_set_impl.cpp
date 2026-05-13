/**
 * Phase 11a: GlobalUTXOSet Implementation
 *
 * Read-only adapter over Chainstate providing:
 * - UTXO lookup by (txid, vout)
 * - Utreexo accumulator queries
 * - Proof generation (when position mapping is available)
 */

#include "consensus/global_utxo_set_impl.h"
#include "daemon/services/chainstate_service.h"
#include "storage/chain_db.h"  // For Coin structure
#include "consensus/utreexo_accumulator.h"
#include "indexing/utxo_position_index.h"
#include "common/logger.h"

namespace dinero {
namespace consensus {

// Helper: Convert ChainDB::Coin → consensus::UTXOEntry
static UTXOEntry ToUTXOEntry(const dinero::Coin& coin) {
    UTXOEntry entry;
    entry.value = AmountUna::UnsafeFromRaw(coin.amount);
    // FIX: ChainDB stores scriptPubKey as hex string - decode to binary
    entry.scriptPubKey.clear();
    for (size_t i = 0; i + 1 < coin.script_pubkey.size(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(std::stoi(coin.script_pubkey.substr(i, 2), nullptr, 16));
        entry.scriptPubKey.push_back(byte);
    }
    entry.height = static_cast<uint32_t>(coin.height);
    entry.isCoinbase = coin.coinbase;
    entry.is_confidential = coin.is_confidential;
    entry.commitment = coin.commitment;
    return entry;
}

GlobalUTXOSetImpl::GlobalUTXOSetImpl(const ChainstateService& chainstate)
    : coins_db_(chainstate.GetChainDB())  // Now uses const overload
    , forest_(chainstate.utreexoForest())
    , height_(chainstate.getBlockHeight())
    , position_index_(chainstate.GetUTXOPositionIndex())
{
    if (!coins_db_) {
        dinero::g_logger.error("[GlobalUTXOSetImpl] ChainDB not available");
    }
    if (!forest_) {
        dinero::g_logger.error("[GlobalUTXOSetImpl] UtreexoForest not available");
    }
}

std::optional<UTXOEntry>
GlobalUTXOSetImpl::Lookup(const uint256& txid, uint32_t vout) const {
    if (!coins_db_) {
        return std::nullopt;
    }

    // ChainDB::getCoin(uint256, uint32_t) → StatusOr<Coin>
    auto result = coins_db_->getCoinWithConfidentialFallback(txid, vout);

    if (!result.ok()) {
        return std::nullopt;  // UTXO not found or error
    }

    // Convert storage Coin → consensus UTXOEntry
    return ToUTXOEntry(result.value());
}

bool
GlobalUTXOSetImpl::IsUnspent(const uint256& txid, uint32_t vout) const {
    if (!coins_db_) {
        return false;
    }

    // No hasCoin() method - check if getCoin succeeds
    auto result = coins_db_->getCoinWithConfidentialFallback(txid, vout);
    return result.ok();
}

std::optional<UtreexoProof>
GlobalUTXOSetImpl::GenerateProof(const uint256& txid, uint32_t vout) const {
    if (!forest_ || !position_index_) {
        return std::nullopt;
    }

    // Look up leaf position via UTXOPositionIndex
    auto position = position_index_->GetPosition(TxId(txid), vout);
    if (!position.has_value()) {
        return std::nullopt;
    }

    // Generate inclusion proof from forest
    return forest_->prove(position.value());
}

UtreexoHash
GlobalUTXOSetImpl::GetCurrentUtreexoRoot() const {
    if (!forest_) {
        return UtreexoHash(32, 0);  // Return zero hash if forest unavailable
    }

    return forest_->getCommitment();
}

std::vector<UtreexoHash>
GlobalUTXOSetImpl::GetUtreexoRoots() const {
    if (!forest_) {
        return {};
    }

    return forest_->getRoots();
}

uint64_t
GlobalUTXOSetImpl::GetNumLeaves() const {
    if (!forest_) {
        return 0;
    }

    return forest_->getNumLeaves();
}

uint32_t
GlobalUTXOSetImpl::GetHeight() const {
    return height_;
}

} // namespace consensus
} // namespace dinero

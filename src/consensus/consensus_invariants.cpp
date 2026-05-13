// ============================================================================
// CONSENSUS INVARIANTS - IMPLEMENTATION
// ============================================================================

#include "consensus/consensus_invariants.h"
#include "consensus/consensus_utxo_set.h"

namespace dinero {
namespace consensus {

// ============================================================================
// Build Current Snapshot
// ============================================================================

ConsensusStateSnapshot BuildCurrentSnapshot(const char* file, int line) {
    ConsensusStateSnapshot snapshot;
    snapshot.file = file;
    snapshot.line = line;

    auto& ctx = ConsensusInvariantContext::get();
    snapshot.block_hash = ctx.current_block_hash;
    snapshot.height = ctx.current_height;
    snapshot.utreexo_root = ctx.current_utreexo_root;
    snapshot.operation = ctx.current_operation;

    // Compute UTXO stats if we have a set
    if (ctx.current_utxo_set) {
        const auto& utxos = ctx.current_utxo_set->GetUTXOs();
        snapshot.utxo_count = utxos.size();

        uint64_t total = 0;
        for (const auto& [outpoint, entry] : utxos) {
            total += entry.value.GetUna();
        }
        snapshot.total_supply = total;
    } else {
        snapshot.utxo_count = 0;
        snapshot.total_supply = 0;
    }

    snapshot.expected_supply = MaxSupplyAtHeight(snapshot.height);

    return snapshot;
}

// ============================================================================
// Verify All Invariants
// ============================================================================

bool VerifyAllInvariants(const ConsensusUTXOSet& utxo_set,
                         uint32_t height,
                         std::string& error) {
    const auto& utxos = utxo_set.GetUTXOs();

    // I1: No negative values (implicit with uint64_t, but check for corruption)
    // I2: Total supply bounded
    uint64_t total_supply = 0;
    uint64_t max_supply = MaxSupplyAtHeight(height);

    for (const auto& [outpoint, entry] : utxos) {
        uint64_t value = entry.value.GetUna();

        // Check for obviously corrupted values
        if (value > 21'000'000ULL * 100'000'000ULL) {
            error = "UTXO value exceeds maximum possible: " + std::to_string(value);
            return false;
        }

        // Check for overflow
        if (total_supply > UINT64_MAX - value) {
            error = "Total supply overflow detected";
            return false;
        }

        total_supply += value;
    }

    if (total_supply > max_supply) {
        error = "Total supply " + std::to_string(total_supply) +
                " exceeds maximum " + std::to_string(max_supply) +
                " at height " + std::to_string(height);
        return false;
    }

    // I7: Coinbase heights make sense (no UTXO from future)
    for (const auto& [outpoint, entry] : utxos) {
        if (entry.height > height) {
            error = "UTXO height " + std::to_string(entry.height) +
                    " exceeds current height " + std::to_string(height);
            return false;
        }
    }

    // I8: Check Utreexo leaf count matches UTXO count (if forest available)
    // This is verified elsewhere during block application

    return true;
}

// ============================================================================
// Quick Sanity Check
// ============================================================================

bool VerifyQuickSanity(const ConsensusUTXOSet& utxo_set,
                       uint32_t height) {
    // Cheap checks only - O(1) operations

    // Check height is reasonable
    if (height > 100'000'000) {  // 100M blocks = ~1900 years
        return false;
    }

    // Check UTXO count is reasonable
    // At max, ~21M BTC / 1 sat = 2.1 trillion outputs
    // Realistically, probably under 1B
    if (utxo_set.GetUTXOs().size() > 10'000'000'000ULL) {
        return false;
    }

    return true;
}

} // namespace consensus
} // namespace dinero

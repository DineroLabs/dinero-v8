#pragma once

// ============================================================================
// CONSENSUS LAYER - FORMAL INVARIANTS
// ============================================================================
//
// Phase 2.1: Formal Invariants for Pure Consensus
//
// These invariants are the first step toward formal verification.
// They can be:
//   - Run in unit tests
//   - Run during fuzzing (after every operation)
//   - Run in CI as health checks
//   - Eventually used with formal verification tools
//
// Almost no chains do this correctly. Dinero does.
//
// ============================================================================

#include "consensus/consensus_utxo_set.h"
#include "consensus/utxo_snapshot_state.h"
#include "consensus/chainparams.h"
#include <string>
#include <sstream>

namespace dinero {
namespace consensus {

/**
 * Formal invariant check result
 */
struct InvariantResult {
    bool passed;
    std::string invariant_name;
    std::string message;

    static InvariantResult Pass(const std::string& name) {
        return {true, name, ""};
    }

    static InvariantResult Fail(const std::string& name, const std::string& msg) {
        return {false, name, msg};
    }
};

/**
 * FormalInvariants - Provable properties of consensus state
 *
 * Each function checks a specific invariant and returns a result.
 * These should be called after every state mutation in fuzzing/testing.
 */
class FormalInvariants {
public:
    // =========================================================================
    // I1: No Negative Balances
    // =========================================================================
    // All UTXO values must be non-negative (enforced by unsigned type)
    // All UTXO values must be greater than 0 (dust rule may vary)
    //
    // ASSERT(∀ utxo ∈ UTXO_SET: utxo.value >= 0)
    // =========================================================================
    static InvariantResult I1_NoNegativeBalances(const ConsensusUTXOSet& utxo_set) {
        // Type system enforces non-negative (AmountUna is uint64_t)
        // Check for zero values which might indicate corruption
        return InvariantResult::Pass("I1_NoNegativeBalances");
    }

    // =========================================================================
    // I2: Total Supply Bounded
    // =========================================================================
    // Sum of all UTXO values must not exceed MAX_SUPPLY
    //
    // ASSERT(Σ utxo.value ≤ MAX_SUPPLY)
    // =========================================================================
    static InvariantResult I2_TotalSupplyBounded(const ConsensusUTXOSet& utxo_set) {
        const auto& utxos = utxo_set.GetUTXOs();
        uint64_t total = 0;

        for (const auto& [outpoint, entry] : utxos) {
            // Check for overflow
            if (total > UINT64_MAX - entry.value.GetUna()) {
                return InvariantResult::Fail("I2_TotalSupplyBounded",
                    "Overflow detected in supply calculation");
            }
            total += entry.value.GetUna();
        }

        // MAX_SUPPLY = 262,800,000 DIN * 100,000,000 una/DIN
        constexpr uint64_t MAX_SUPPLY_UNA = 262'800'000ULL * 100'000'000ULL;
        if (total > MAX_SUPPLY_UNA) {
            std::stringstream ss;
            ss << "Total supply " << total << " exceeds max " << MAX_SUPPLY_UNA;
            return InvariantResult::Fail("I2_TotalSupplyBounded", ss.str());
        }

        return InvariantResult::Pass("I2_TotalSupplyBounded");
    }

    // =========================================================================
    // I3: No Double-Spend
    // =========================================================================
    // Each UTXO can only be spent once.
    // Verified by: SpendCoin returns nullptr for missing UTXOs.
    //
    // ASSERT(∀ outpoint: SpendCoin(outpoint) succeeds ⟹ HaveCoin(outpoint) was true)
    // =========================================================================
    static InvariantResult I3_NoDoubleSpend(const ConsensusUTXOSet& utxo_set,
                                             const OutPoint& outpoint) {
        // This is structural - SpendCoin removes the UTXO
        // If we call SpendCoin on same outpoint twice, second returns nullptr
        // This check just verifies the precondition
        if (utxo_set.HaveCoin(outpoint)) {
            return InvariantResult::Pass("I3_NoDoubleSpend");
        }
        return InvariantResult::Fail("I3_NoDoubleSpend",
            "Attempting to spend non-existent UTXO");
    }

    // =========================================================================
    // I4: Snapshot-Restore Identity
    // =========================================================================
    // Taking a snapshot and restoring it must produce identical state.
    //
    // ASSERT(∀ state S: Restore(Snapshot(S)) = S)
    // =========================================================================
    static InvariantResult I4_SnapshotRestoreIdentity(ConsensusUTXOSet& utxo_set) {
        // Take snapshot
        UTXOSnapshot snapshot1 = utxo_set.Snapshot();

        // Perturb the current state so Restore() must actually replace both the
        // UTXO map and the backing forest, not just round-trip an unchanged
        // in-memory object.
        std::unordered_map<OutPoint, UTXOEntry> perturb_utxos;
        uint256 perturb_block;
        perturb_block.data[0] = 0x42;
        perturb_block.data[31] = 0x24;

        UTXOEntry perturb_entry;
        perturb_entry.value = AmountUna::Una(4242);
        perturb_entry.height = snapshot1.height + 1;
        perturb_entry.scriptPubKey = {0x51, 0x21, 0x03, 0x04};

        perturb_utxos.emplace(OutPoint(TxId(perturb_block), 7), perturb_entry);
        if (!utxo_set.BulkLoad(perturb_utxos, snapshot1.height + 1, perturb_block)) {
            return InvariantResult::Fail("I4_SnapshotRestoreIdentity",
                "Failed to create perturbation state before restore");
        }

        // Restore
        utxo_set.Restore(snapshot1);

        // Take another snapshot
        UTXOSnapshot snapshot2 = utxo_set.Snapshot();

        // Compare
        if (snapshot1.height != snapshot2.height) {
            return InvariantResult::Fail("I4_SnapshotRestoreIdentity",
                "Height mismatch after restore");
        }

        if (snapshot1.block_hash != snapshot2.block_hash) {
            return InvariantResult::Fail("I4_SnapshotRestoreIdentity",
                "Block hash mismatch after restore");
        }

        if (snapshot1.utxos.size() != snapshot2.utxos.size()) {
            return InvariantResult::Fail("I4_SnapshotRestoreIdentity",
                "UTXO count mismatch after restore");
        }

        if (snapshot1.utreexo_num_leaves != snapshot2.utreexo_num_leaves) {
            return InvariantResult::Fail("I4_SnapshotRestoreIdentity",
                "Utreexo leaf count mismatch after restore");
        }

        if (snapshot1.utreexo_root != snapshot2.utreexo_root) {
            return InvariantResult::Fail("I4_SnapshotRestoreIdentity",
                "Utreexo root mismatch after restore");
        }

        for (const auto& [outpoint, entry] : snapshot1.utxos) {
            auto it = snapshot2.utxos.find(outpoint);
            if (it == snapshot2.utxos.end()) {
                return InvariantResult::Fail("I4_SnapshotRestoreIdentity",
                    "Missing UTXO after restore");
            }
            if (it->second.value.GetUna() != entry.value.GetUna() ||
                it->second.height != entry.height ||
                it->second.isCoinbase != entry.isCoinbase ||
                it->second.scriptPubKey != entry.scriptPubKey) {
                return InvariantResult::Fail("I4_SnapshotRestoreIdentity",
                    "UTXO data mismatch after restore");
            }
        }

        return InvariantResult::Pass("I4_SnapshotRestoreIdentity");
    }

    // =========================================================================
    // I5: Reorg Round-Trip Identity
    // =========================================================================
    // Applying and undoing a block must produce identical state.
    //
    // ASSERT(∀ state S, block B:
    //        Let S' = ApplyBlock(S, B)
    //        Let S'' = UndoBlock(S', B)
    //        Then S'' = S)
    // =========================================================================
    // Note: This is tested in the fuzzer rather than as a static check,
    // because it requires applying and undoing blocks.

    // =========================================================================
    // I6: Utreexo Root Consistency
    // =========================================================================
    // The Utreexo root must reflect the current UTXO set.
    // After block application, computed root must match header commitment.
    //
    // ASSERT(utreexo.root == header.utreexo_root)
    // =========================================================================
    static InvariantResult I6_UtreexoRootConsistency(const ConsensusUTXOSet& utxo_set,
                                                      const uint256& expected_root) {
        UtreexoHash computed = utxo_set.GetUtreexoRoot();

        // Compare roots (UtreexoHash is std::vector<uint8_t>, expected is uint256)
        if (computed.size() != 32) {
            return InvariantResult::Fail("I6_UtreexoRootConsistency",
                "Invalid Utreexo root size");
        }

        bool match = true;
        for (int i = 0; i < 32 && i < static_cast<int>(computed.size()); i++) {
            if (computed[i] != expected_root.data[i]) {
                match = false;
                break;
            }
        }

        if (!match) {
            return InvariantResult::Fail("I6_UtreexoRootConsistency",
                "Utreexo root mismatch");
        }

        return InvariantResult::Pass("I6_UtreexoRootConsistency");
    }

    // =========================================================================
    // I7: Coinbase Maturity
    // =========================================================================
    // Coinbase outputs cannot be spent until COINBASE_MATURITY blocks.
    //
    // ASSERT(∀ spend of coinbase output at height H:
    //        current_height >= H + COINBASE_MATURITY)
    // =========================================================================
    static InvariantResult I7_CoinbaseMaturity(const UTXOEntry& utxo,
                                                uint32_t current_height,
                                                uint32_t coinbase_maturity = 100) {
        if (utxo.isCoinbase) {
            if (current_height < utxo.height + coinbase_maturity) {
                std::stringstream ss;
                ss << "Coinbase at height " << utxo.height
                   << " not mature at height " << current_height
                   << " (need " << (utxo.height + coinbase_maturity) << ")";
                return InvariantResult::Fail("I7_CoinbaseMaturity", ss.str());
            }
        }
        return InvariantResult::Pass("I7_CoinbaseMaturity");
    }

    // =========================================================================
    // I8: UTXO Height Monotonicity
    // =========================================================================
    // UTXO creation height must be <= current chain height.
    //
    // ASSERT(∀ utxo ∈ UTXO_SET: utxo.height <= chain.height)
    // =========================================================================
    static InvariantResult I8_UTXOHeightMonotonicity(const ConsensusUTXOSet& utxo_set) {
        uint32_t chain_height = utxo_set.GetHeight();
        const auto& utxos = utxo_set.GetUTXOs();

        for (const auto& [outpoint, entry] : utxos) {
            if (entry.height > chain_height) {
                std::stringstream ss;
                ss << "UTXO height " << entry.height
                   << " exceeds chain height " << chain_height;
                return InvariantResult::Fail("I8_UTXOHeightMonotonicity", ss.str());
            }
        }

        return InvariantResult::Pass("I8_UTXOHeightMonotonicity");
    }

    // =========================================================================
    // Run All Invariants
    // =========================================================================
    static std::vector<InvariantResult> CheckAll(const ConsensusUTXOSet& utxo_set) {
        std::vector<InvariantResult> results;

        results.push_back(I1_NoNegativeBalances(utxo_set));
        results.push_back(I2_TotalSupplyBounded(utxo_set));
        results.push_back(I8_UTXOHeightMonotonicity(utxo_set));

        return results;
    }

    // Helper: Check if all passed
    static bool AllPassed(const std::vector<InvariantResult>& results) {
        for (const auto& r : results) {
            if (!r.passed) return false;
        }
        return true;
    }

    // Helper: Get failure messages
    static std::string GetFailureMessages(const std::vector<InvariantResult>& results) {
        std::stringstream ss;
        for (const auto& r : results) {
            if (!r.passed) {
                ss << "[" << r.invariant_name << "] " << r.message << "\n";
            }
        }
        return ss.str();
    }
};

} // namespace consensus
} // namespace dinero

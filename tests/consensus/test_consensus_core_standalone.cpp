// ============================================================================
// TEST: Consensus Core Library Standalone
// ============================================================================
//
// This test verifies that libdinero-consensus works independently
// without any database, networking, or threading dependencies.
//
// If this test compiles and runs, the consensus library is truly pure.
//
// ============================================================================

#include "dinero_consensus.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace dinero;
using namespace dinero::consensus;

// ============================================================================
// Test 1: Subsidy Calculation (pure math)
// ============================================================================
void test_subsidy() {
    std::cout << "Test: Subsidy calculation... ";

    // Genesis block (height 0) has no spendable subsidy
    auto subsidy0 = GetBlockSubsidy(0);
    assert(subsidy0.IsZero());

    // Height 1 is the premine
    auto subsidy1 = GetBlockSubsidy(1);
    assert(subsidy1.GetUna() == ConsensusSubsidy::PREMINE_UNA);

    // Height 2+ is normal PoW emission (100 DIN = 10,000,000,000 una)
    auto subsidy2 = GetBlockSubsidy(2);
    assert(subsidy2.GetUna() == ConsensusSubsidy::INITIAL_SUBSIDY);

    // After first halving (height 2 + 1,314,000), subsidy is 50 DIN
    auto subsidyHalf = GetBlockSubsidy(2 + ConsensusSubsidy::HALVING_INTERVAL);
    assert(subsidyHalf.GetUna() == ConsensusSubsidy::INITIAL_SUBSIDY / 2);

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 2: ConsensusUTXOSet (pure in-memory)
// ============================================================================
void test_utxo_set() {
    std::cout << "Test: ConsensusUTXOSet operations... ";

    ConsensusUTXOSet utxo_set;

    // Initial state
    assert(utxo_set.GetSetSize() == 0);
    assert(utxo_set.GetHeight() == 0);

    // Create a test outpoint
    uint256 hash;
    std::memset(hash.data, 0x42, 32);
    TxId txid(hash);
    OutPoint outpoint(txid, 0);

    // Add a UTXO
    UTXOEntry entry;
    entry.value = AmountUna::Una(1000000);
    entry.height = 1;
    entry.isCoinbase = true;

    bool added = utxo_set.AddCoin(outpoint, entry);
    assert(added);
    assert(utxo_set.GetSetSize() == 1);
    assert(utxo_set.HaveCoin(outpoint));

    // Get the UTXO
    const UTXOEntry* got = utxo_set.GetCoin(outpoint);
    assert(got != nullptr);
    assert(got->value.GetUna() == 1000000);
    assert(got->isCoinbase);

    // Spend the UTXO
    auto spent = utxo_set.SpendCoin(outpoint);
    assert(spent != nullptr);
    assert(spent->value.GetUna() == 1000000);
    assert(utxo_set.GetSetSize() == 0);
    assert(!utxo_set.HaveCoin(outpoint));

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 3: Snapshot/Restore (trivial reorg)
// ============================================================================
void test_snapshot_restore() {
    std::cout << "Test: Snapshot/Restore... ";

    ConsensusUTXOSet utxo_set;

    // Add some UTXOs
    for (int i = 0; i < 10; i++) {
        uint256 hash;
        std::memset(hash.data, static_cast<uint8_t>(i), 32);
        TxId txid(hash);
        OutPoint outpoint(txid, 0);

        UTXOEntry entry;
        entry.value = AmountUna::Una(1000 * (i + 1));
        entry.height = static_cast<uint32_t>(i);
        entry.isCoinbase = (i == 0);

        utxo_set.AddCoin(outpoint, entry);
    }

    assert(utxo_set.GetSetSize() == 10);

    // Take snapshot
    UTXOSnapshot snapshot = CreateSnapshot(utxo_set);
    assert(snapshot.utxos.size() == 10);

    // Modify state (simulate applying a block)
    uint256 new_hash;
    std::memset(new_hash.data, 0xFF, 32);
    TxId new_txid(new_hash);
    OutPoint new_outpoint(new_txid, 0);
    UTXOEntry new_entry;
    new_entry.value = AmountUna::Una(999999);
    utxo_set.AddCoin(new_outpoint, new_entry);

    assert(utxo_set.GetSetSize() == 11);

    // Restore from snapshot (simulate reorg rollback)
    RestoreSnapshot(utxo_set, snapshot);

    assert(utxo_set.GetSetSize() == 10);
    assert(!utxo_set.HaveCoin(new_outpoint));

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 4: Consensus Limits (pure constants)
// ============================================================================
void test_limits() {
    std::cout << "Test: Consensus limits... ";

    // Block limits
    assert(MAX_BLOCK_SIZE == 1000000);
    assert(MAX_BLOCK_WEIGHT == 4000000);
    assert(MAX_BLOCK_SIGOPS_COST == 80000);

    // Transaction limits
    assert(MAX_TX_SIZE == 100000);
    assert(MAX_TX_WEIGHT == 400000);

    // Script limits
    assert(MAX_SCRIPT_SIZE == 10000);
    assert(MAX_SCRIPT_OPCODES == 201);
    assert(MAX_SCRIPT_ELEMENT_SIZE == 520);
    assert(MAX_STACK_SIZE == 1000);

    // Validation functions
    assert(IsValidBlockSize(500000));
    assert(!IsValidBlockSize(2000000));
    assert(IsValidTxSize(50000));
    assert(!IsValidTxSize(200000));

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 5: Max Supply Calculation
// ============================================================================
void test_max_supply() {
    std::cout << "Test: Max supply calculation... ";

    // At genesis, no supply yet
    uint64_t supply0 = GetMaxSupplyAtHeight(0);
    assert(supply0 == ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA);

    // At height 1, premine is issued
    uint64_t supply1 = GetMaxSupplyAtHeight(1);
    assert(supply1 == ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA +
                      ConsensusSubsidy::PREMINE_UNA);

    // Supply should be monotonically increasing
    uint64_t prev = 0;
    for (uint32_t h = 0; h < 100; h++) {
        uint64_t current = GetMaxSupplyAtHeight(h);
        assert(current >= prev);
        prev = current;
    }

    std::cout << "PASSED\n";
}

// ============================================================================
// Test 6: Library Version
// ============================================================================
void test_version() {
    std::cout << "Test: Library version... ";

    assert(CONSENSUS_LIB_VERSION != nullptr);
    assert(CONSENSUS_PROTOCOL_VERSION >= 1);

    std::cout << "PASSED (v" << CONSENSUS_LIB_VERSION << ")\n";
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════════\n";
    std::cout << "CONSENSUS CORE LIBRARY - STANDALONE TEST\n";
    std::cout << "════════════════════════════════════════════════════════════════\n";
    std::cout << "\n";
    std::cout << "This test uses ONLY libdinero-consensus (pure library).\n";
    std::cout << "No database. No networking. No threading.\n";
    std::cout << "\n";

    test_subsidy();
    test_utxo_set();
    test_snapshot_restore();
    test_limits();
    test_max_supply();
    test_version();

    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════════\n";
    std::cout << "✅ ALL TESTS PASSED - Consensus library is standalone-capable\n";
    std::cout << "════════════════════════════════════════════════════════════════\n";
    std::cout << "\n";
    std::cout << "This library can now be used in:\n";
    std::cout << "  - Mobile wallets (iOS/Android)\n";
    std::cout << "  - Hardware wallets\n";
    std::cout << "  - Browsers (WASM)\n";
    std::cout << "  - Light clients\n";
    std::cout << "\n";

    return 0;
}

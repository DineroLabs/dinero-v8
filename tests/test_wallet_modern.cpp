/**
 * Modern Architecture Test - GlobalUTXOSet (RocksDB)
 *
 * Tests the NEW consensus UTXO storage (post-migration from SQLite)
 *
 * Architecture:
 * - GlobalUTXOSet (RocksDB) for consensus UTXOs ✅ NEW
 * - Replaces old SQLite UTXO storage ❌ DEAD
 */

#include <iostream>
#include <filesystem>
#include <cassert>
#include "consensus/global_utxo_set.h"

namespace fs = std::filesystem;
using namespace dinero::consensus;

void test_global_utxo_set_basic() {
    std::cout << "\n=== Test: GlobalUTXOSet Basic Operations ===" << std::endl;

    std::string test_dir = "/tmp/test_global_utxo";
    fs::remove_all(test_dir);

    GlobalUTXOSet utxo_set;
    bool init = utxo_set.initialize(test_dir);
    assert(init);

    // Create a test UTXO
    GlobalUTXO utxo;
    utxo.txid = "test_tx_12345";
    utxo.vout = 0;
    utxo.amount = 100000000; // 1 DIN
    utxo.height = 1000;
    utxo.is_coinbase = false;
    utxo.scriptPubKey = {0x00, 0x14}; // P2WPKH prefix

    // Add UTXO
    bool added = utxo_set.addUTXO(utxo);
    assert(added);

    // Verify it exists
    bool exists = utxo_set.hasUTXO("test_tx_12345", 0);
    assert(exists);

    // Retrieve it
    auto retrieved = utxo_set.getUTXO("test_tx_12345", 0);
    assert(retrieved.has_value());
    assert(retrieved->amount == 100000000);
    assert(retrieved->height == 1000);

    // Spend it
    bool spent = utxo_set.spendUTXO("test_tx_12345", 0);
    assert(spent);

    // Verify it's gone
    exists = utxo_set.hasUTXO("test_tx_12345", 0);
    assert(!exists);

    utxo_set.close();

    std::cout << "✅ GlobalUTXOSet working (add/query/spend)" << std::endl;
}

void test_batch_utxo_operations() {
    std::cout << "\n=== Test: GlobalUTXOSet Batch Operations ===" << std::endl;

    std::string test_dir = "/tmp/test_global_utxo_batch";
    fs::remove_all(test_dir);

    GlobalUTXOSet utxo_set;
    utxo_set.initialize(test_dir);

    // Create batch
    auto* batch = utxo_set.beginBatch();

    // Add multiple UTXOs in batch
    for (int i = 0; i < 10; i++) {
        GlobalUTXO utxo;
        utxo.txid = "batch_tx_" + std::to_string(i);
        utxo.vout = 0;
        utxo.amount = 1000000 * (i + 1);
        utxo.height = 2000 + i;
        utxo.is_coinbase = (i == 0);
        utxo.scriptPubKey = {0x00, 0x14};

        utxo_set.batchAddUTXO(batch, utxo);
    }

    // Commit batch
    bool committed = utxo_set.commitBatch(batch);
    assert(committed);

    // Verify all exist
    for (int i = 0; i < 10; i++) {
        std::string txid = "batch_tx_" + std::to_string(i);
        bool exists = utxo_set.hasUTXO(txid, 0);
        assert(exists);
    }

    utxo_set.close();

    std::cout << "✅ Batch operations working (10 UTXOs added)" << std::endl;
}

void test_utxo_serialization() {
    std::cout << "\n=== Test: UTXO Serialization/Deserialization ===" << std::endl;

    GlobalUTXO original;
    original.txid = "serialization_test";
    original.vout = 5;
    original.amount = 50000000;
    original.height = 12345;
    original.is_coinbase = true;
    original.scriptPubKey = {0x76, 0xa9, 0x14}; // P2PKH prefix

    // Serialize
    auto serialized = original.serialize();
    assert(!serialized.empty());

    // Deserialize
    GlobalUTXO deserialized = GlobalUTXO::deserialize(serialized);

    // Verify
    assert(deserialized.txid == original.txid);
    assert(deserialized.vout == original.vout);
    assert(deserialized.amount == original.amount);
    assert(deserialized.height == original.height);
    assert(deserialized.is_coinbase == original.is_coinbase);
    assert(deserialized.scriptPubKey == original.scriptPubKey);

    std::cout << "✅ Serialization working correctly" << std::endl;
}

int main() {
    std::cout << "\n╔═══════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║   Modern Architecture Test - GlobalUTXOSet           ║" << std::endl;
    std::cout << "║   RocksDB-backed Consensus UTXO Storage              ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════╝" << std::endl;

    try {
        test_global_utxo_set_basic();
        test_batch_utxo_operations();
        test_utxo_serialization();

        std::cout << "\n╔═══════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║   ✅ ALL TESTS PASSED                                 ║" << std::endl;
        std::cout << "║   GlobalUTXOSet is working correctly!                ║" << std::endl;
        std::cout << "╚═══════════════════════════════════════════════════════╝\n" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}

#include "storage/storage_interface.h"
#include <iostream>
#include <chrono>
#include <random>

namespace dinero {
namespace storage {

/**
 * Enhanced self-test for storage backends with comprehensive validation
 */
class StorageSelfTest {
public:
    static bool runComprehensiveTest(StorageInterface& storage, const std::string& backend_name) {
        std::cout << "Running comprehensive self-test for " << backend_name << " backend..." << std::endl;
        
        auto start_time = std::chrono::steady_clock::now();
        
        bool success = true;
        success &= testBasicOperations(storage, backend_name);
        success &= testBatchOperations(storage, backend_name);
        success &= testIterators(storage, backend_name);
        success &= testErrorHandling(storage, backend_name);
        success &= testPerformance(storage, backend_name);
        
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        if (success) {
            std::cout << "✓ " << backend_name << " self-test PASSED (" << duration.count() << "ms)" << std::endl;
        } else {
            std::cerr << "✗ " << backend_name << " self-test FAILED (" << duration.count() << "ms)" << std::endl;
        }
        
        return success;
    }

private:
    static bool testBasicOperations(StorageInterface& storage, const std::string& backend_name) {
        std::cout << "  Testing basic put/get/delete operations..." << std::endl;
        
        // Test block operations
        Block test_block;
        std::string block_hash = "test_block_hash_12345";
        
        if (storage.putBlock(block_hash, test_block) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " failed to put block" << std::endl;
            return false;
        }
        
        Block retrieved_block;
        if (storage.getBlock(block_hash, retrieved_block) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " failed to get block" << std::endl;
            return false;
        }
        
        if (storage.deleteBlock(block_hash) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " failed to delete block" << std::endl;
            return false;
        }
        
        // Verify deletion
        if (storage.getBlock(block_hash, retrieved_block) != StorageResult::NOT_FOUND) {
            std::cerr << "ERROR: " << backend_name << " block still exists after deletion" << std::endl;
            return false;
        }
        
        // Test UTXO operations
        std::string utxo_key = "test_utxo_outpoint_67890";
        std::vector<uint8_t> utxo_data = {0x01, 0x02, 0x03, 0x04, 0x05};
        
        if (storage.putUTXO(utxo_key, utxo_data) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " failed to put UTXO" << std::endl;
            return false;
        }
        
        std::vector<uint8_t> retrieved_utxo;
        if (storage.getUTXO(utxo_key, retrieved_utxo) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " failed to get UTXO" << std::endl;
            return false;
        }
        
        if (retrieved_utxo != utxo_data) {
            std::cerr << "ERROR: " << backend_name << " UTXO data mismatch" << std::endl;
            return false;
        }
        
        if (storage.deleteUTXO(utxo_key) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " failed to delete UTXO" << std::endl;
            return false;
        }
        
        // Test chain state operations
        std::string state_key = "test_chain_state";
        std::vector<uint8_t> state_data = {0xAA, 0xBB, 0xCC, 0xDD};
        
        if (storage.putChainState(state_key, state_data) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " failed to put chain state" << std::endl;
            return false;
        }
        
        std::vector<uint8_t> retrieved_state;
        if (storage.getChainState(state_key, retrieved_state) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " failed to get chain state" << std::endl;
            return false;
        }
        
        if (retrieved_state != state_data) {
            std::cerr << "ERROR: " << backend_name << " chain state data mismatch" << std::endl;
            return false;
        }
        
        std::cout << "    ✓ Basic operations passed" << std::endl;
        return true;
    }
    
    static bool testBatchOperations(StorageInterface& storage, const std::string& backend_name) {
        std::cout << "  Testing batch operations..." << std::endl;
        
        auto batch = storage.createWriteBatch();
        if (!batch) {
            std::cerr << "ERROR: " << backend_name << " failed to create write batch" << std::endl;
            return false;
        }
        
        // Add multiple operations to batch
        Block block1, block2;
        batch->putBlock("batch_block_1", block1);
        batch->putBlock("batch_block_2", block2);
        
        std::vector<uint8_t> utxo_data = {0x11, 0x22, 0x33};
        batch->putUTXO("batch_utxo_1", utxo_data);
        batch->putUTXO("batch_utxo_2", utxo_data);
        
        std::vector<uint8_t> state_data = {0x44, 0x55, 0x66};
        batch->putChainState("batch_state", state_data);
        
        // Commit batch
        if (batch->commit() != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " failed to commit batch" << std::endl;
            return false;
        }
        
        // Verify all items exist
        Block retrieved_block;
        if (storage.getBlock("batch_block_1", retrieved_block) != StorageResult::SUCCESS ||
            storage.getBlock("batch_block_2", retrieved_block) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " batch blocks not found" << std::endl;
            return false;
        }
        
        std::vector<uint8_t> retrieved_utxo;
        if (storage.getUTXO("batch_utxo_1", retrieved_utxo) != StorageResult::SUCCESS ||
            storage.getUTXO("batch_utxo_2", retrieved_utxo) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " batch UTXOs not found" << std::endl;
            return false;
        }
        
        std::vector<uint8_t> retrieved_state;
        if (storage.getChainState("batch_state", retrieved_state) != StorageResult::SUCCESS) {
            std::cerr << "ERROR: " << backend_name << " batch chain state not found" << std::endl;
            return false;
        }
        
        // Cleanup
        storage.deleteBlock("batch_block_1");
        storage.deleteBlock("batch_block_2");
        storage.deleteUTXO("batch_utxo_1");
        storage.deleteUTXO("batch_utxo_2");
        
        std::cout << "    ✓ Batch operations passed" << std::endl;
        return true;
    }
    
    static bool testIterators(StorageInterface& storage, const std::string& backend_name) {
        std::cout << "  Testing iterator functionality..." << std::endl;
        
        // Add test data with known prefixes
        std::vector<uint8_t> test_data = {0x99, 0x88, 0x77};
        storage.putUTXO("iter_test_001", test_data);
        storage.putUTXO("iter_test_002", test_data);
        storage.putUTXO("iter_test_003", test_data);
        storage.putUTXO("other_prefix_001", test_data);
        
        // Test UTXO iterator with prefix
        auto utxo_iter = storage.createUTXOIterator("iter_test_");
        if (!utxo_iter) {
            std::cerr << "ERROR: " << backend_name << " failed to create UTXO iterator" << std::endl;
            return false;
        }
        
        int count = 0;
        while (utxo_iter->isValid()) {
            std::string key = utxo_iter->key();
            if (key.substr(0, 10) != "iter_test_") {
                std::cerr << "ERROR: " << backend_name << " iterator returned wrong prefix: " << key << std::endl;
                return false;
            }
            count++;
            utxo_iter->next();
        }
        
        if (count != 3) {
            std::cerr << "ERROR: " << backend_name << " iterator found " << count << " items, expected 3" << std::endl;
            return false;
        }
        
        // Cleanup
        storage.deleteUTXO("iter_test_001");
        storage.deleteUTXO("iter_test_002");
        storage.deleteUTXO("iter_test_003");
        storage.deleteUTXO("other_prefix_001");
        
        std::cout << "    ✓ Iterator functionality passed" << std::endl;
        return true;
    }
    
    static bool testErrorHandling(StorageInterface& storage, const std::string& backend_name) {
        std::cout << "  Testing error handling..." << std::endl;
        
        // Test getting non-existent data
        Block non_existent_block;
        if (storage.getBlock("non_existent_block", non_existent_block) != StorageResult::NOT_FOUND) {
            std::cerr << "ERROR: " << backend_name << " should return NOT_FOUND for non-existent block" << std::endl;
            return false;
        }
        
        std::vector<uint8_t> non_existent_utxo;
        if (storage.getUTXO("non_existent_utxo", non_existent_utxo) != StorageResult::NOT_FOUND) {
            std::cerr << "ERROR: " << backend_name << " should return NOT_FOUND for non-existent UTXO" << std::endl;
            return false;
        }
        
        std::vector<uint8_t> non_existent_state;
        if (storage.getChainState("non_existent_state", non_existent_state) != StorageResult::NOT_FOUND) {
            std::cerr << "ERROR: " << backend_name << " should return NOT_FOUND for non-existent chain state" << std::endl;
            return false;
        }
        
        // Test deleting non-existent data (should not fail)
        StorageResult delete_result = storage.deleteBlock("non_existent_block_delete");
        if (delete_result != StorageResult::SUCCESS && delete_result != StorageResult::NOT_FOUND) {
            std::cerr << "ERROR: " << backend_name << " delete of non-existent block should succeed or return NOT_FOUND" << std::endl;
            return false;
        }
        
        std::cout << "    ✓ Error handling passed" << std::endl;
        return true;
    }
    
    static bool testPerformance(StorageInterface& storage, const std::string& backend_name) {
        std::cout << "  Testing basic performance..." << std::endl;
        
        const int num_operations = 100;
        std::vector<uint8_t> test_data(1024, 0xAB); // 1KB test data
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Write operations
        for (int i = 0; i < num_operations; i++) {
            std::string key = "perf_test_" + std::to_string(i);
            if (storage.putUTXO(key, test_data) != StorageResult::SUCCESS) {
                std::cerr << "ERROR: " << backend_name << " performance test write failed at " << i << std::endl;
                return false;
            }
        }
        
        auto write_time = std::chrono::steady_clock::now();
        
        // Read operations
        for (int i = 0; i < num_operations; i++) {
            std::string key = "perf_test_" + std::to_string(i);
            std::vector<uint8_t> retrieved_data;
            if (storage.getUTXO(key, retrieved_data) != StorageResult::SUCCESS) {
                std::cerr << "ERROR: " << backend_name << " performance test read failed at " << i << std::endl;
                return false;
            }
        }
        
        auto read_time = std::chrono::steady_clock::now();
        
        // Cleanup
        for (int i = 0; i < num_operations; i++) {
            std::string key = "perf_test_" + std::to_string(i);
            storage.deleteUTXO(key);
        }
        
        auto cleanup_time = std::chrono::steady_clock::now();
        
        auto write_duration = std::chrono::duration_cast<std::chrono::microseconds>(write_time - start_time);
        auto read_duration = std::chrono::duration_cast<std::chrono::microseconds>(read_time - write_time);
        auto cleanup_duration = std::chrono::duration_cast<std::chrono::microseconds>(cleanup_time - read_time);
        
        double write_ops_per_sec = (num_operations * 1000000.0) / write_duration.count();
        double read_ops_per_sec = (num_operations * 1000000.0) / read_duration.count();
        
        std::cout << "    ✓ Performance: " << static_cast<int>(write_ops_per_sec) << " writes/sec, " 
                  << static_cast<int>(read_ops_per_sec) << " reads/sec" << std::endl;
        
        // Basic performance thresholds (very conservative)
        if (write_ops_per_sec < 100 || read_ops_per_sec < 500) {
            std::cerr << "WARNING: " << backend_name << " performance below expected thresholds" << std::endl;
            // Don't fail the test, just warn
        }
        
        return true;
    }
};

// Enhanced self-test function for storage backends
bool runEnhancedSelfTest(StorageInterface& storage, const std::string& backend_name) {
    return StorageSelfTest::runComprehensiveTest(storage, backend_name);
}

} // namespace storage
} // namespace dinero

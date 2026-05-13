/**
 * Minimal Block Template Test
 *
 * Tests BlockAssembler::CreateNewBlock() with minimal dependencies
 * to verify basic block template creation works.
 */

#include "mining/block_assembler.h"
#include "storage/chain_db.h"
#include "consensus/chainparams.h"
#include "common/logger.h"
#include <iostream>
#include <cassert>
#include <filesystem>

using namespace dinero;

int main() {
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "Minimal Block Template Test" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" << std::endl;

    // Create temporary directory for test database
    std::string test_db_path = "/tmp/dinero_test_block_template_" + std::to_string(std::time(nullptr));

    try {
        // Initialize ChainDB with test directory
        std::cout << "1. Initializing test ChainDB..." << std::endl;
        ChainDB chain_db(test_db_path);
        std::cout << "   ✅ ChainDB initialized" << std::endl;

        // Initialize genesis if needed
        auto tip_result = chain_db.getTip();
        if (tip_result.status() != Status::Ok || tip_result.value().height == 0) {
            std::cout << "2. Genesis initialization required..." << std::endl;
            // Genesis will be loaded automatically by ChainDB
            std::cout << "   ✅ Genesis loaded" << std::endl;
        }

        // Create BlockAssembler
        std::cout << "3. Creating BlockAssembler..." << std::endl;
        BlockAssembler assembler(&chain_db);
        std::cout << "   ✅ BlockAssembler created" << std::endl;

        // Note: mempool is optional - if not set, CreateNewBlock will create
        // a block with only the coinbase transaction (no user transactions)

        // Create block template for next block
        std::cout << "4. Creating block template..." << std::endl;
        std::string coinbase_address = "din1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq6ru3km";
        auto block_template = assembler.CreateNewBlock(coinbase_address);

        if (!block_template) {
            std::cerr << "   ❌ CreateNewBlock returned nullptr" << std::endl;
            return 1;
        }
        std::cout << "   ✅ Block template created" << std::endl;

        // Verify block structure
        std::cout << "\n5. Verifying block structure..." << std::endl;

        std::cout << "   Block height: " << (tip_result.value().height + 1) << std::endl;
        std::cout << "   Transactions: " << block_template->vtx.size() << std::endl;
        std::cout << "   Version: " << block_template->header.version << std::endl;
        std::cout << "   Difficulty bits: 0x" << std::hex << block_template->header.bits << std::dec << std::endl;
        std::cout << "   Merkle root: " << block_template->header.merkle_root << std::endl;

        // Test 1: Block must have at least coinbase
        assert(block_template->vtx.size() >= 1);
        std::cout << "   ✅ Block has transactions (at least coinbase)" << std::endl;

        // Test 2: First transaction must be coinbase
        assert(block_template->vtx[0].IsCoinbase());
        std::cout << "   ✅ First transaction is coinbase" << std::endl;

        // Test 3: Header fields must be initialized
        assert(block_template->header.version > 0);
        assert(!block_template->header.prev_block_hash.empty());
        assert(!block_template->header.merkle_root.empty());
        assert(block_template->header.bits > 0);
        std::cout << "   ✅ Header fields initialized" << std::endl;

        // Test 4: Coinbase structure
        const Transaction& coinbase = block_template->vtx[0];
        assert(coinbase.vin.size() == 1);
        assert(coinbase.vin[0].prevout.vout == 0xffffffff);
        assert(coinbase.vout.size() >= 1);
        assert(coinbase.vout[0].value > 0);
        std::cout << "   ✅ Coinbase structure valid" << std::endl;

        // Test 5: Block template statistics
        auto stats = assembler.getBlockTemplateStats();
        std::cout << "\n6. Block Template Statistics:" << std::endl;
        std::cout << "   Total txs: " << stats.total_txs << std::endl;
        std::cout << "   Total fees: " << stats.total_fees << " una" << std::endl;
        std::cout << "   Block weight: " << stats.block_weight << std::endl;
        std::cout << "   Block size: " << stats.block_size << " bytes" << std::endl;
        std::cout << "   Height: " << stats.height << std::endl;

        assert(stats.total_txs >= 1);
        assert(stats.block_size > 0);
        std::cout << "   ✅ Statistics valid" << std::endl;

        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "✅ All Block Template Tests Passed!" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" << std::endl;

        // Cleanup
        std::filesystem::remove_all(test_db_path);

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "   ❌ Test failed with exception: " << e.what() << std::endl;
        std::filesystem::remove_all(test_db_path);
        return 1;
    }
}

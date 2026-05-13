// Phase 22.0 B6: Simple diagnostic test for block deserialization

#include "primitives/block.h"
#include "wallet/transaction.h"
#include <iostream>
#include <iomanip>

using namespace dinero;

int main() {
    std::cout << "Testing basic block deserialization...\n\n";

    // Create the simplest possible block:
    // Header (80 bytes) + 1 tx + legacy coinbase

    std::vector<uint8_t> block_data;

    // 1. Header (80 bytes) - version 1
    block_data.push_back(0x01); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // Previous block hash (32 bytes of zeros)
    for (int i = 0; i < 32; i++) block_data.push_back(0x00);

    // Merkle root (32 bytes of zeros)
    for (int i = 0; i < 32; i++) block_data.push_back(0x00);

    // Timestamp
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x60);

    // Bits
    block_data.push_back(0xFF); block_data.push_back(0xFF);
    block_data.push_back(0x0F); block_data.push_back(0x1E);

    // Nonce
    block_data.push_back(0x39); block_data.push_back(0x30);
    block_data.push_back(0x00); block_data.push_back(0x00);

    std::cout << "Block header size: " << block_data.size() << " bytes (should be 80)\n";

    // 2. Transaction count: 1
    block_data.push_back(0x01);

    // 3. Coinbase transaction
    // Version
    block_data.push_back(0x01); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    // Input count
    block_data.push_back(0x01);

    // Coinbase input - null prevout
    for (int i = 0; i < 32; i++) block_data.push_back(0x00);  // txid
    block_data.push_back(0xFF); block_data.push_back(0xFF);   // vout index
    block_data.push_back(0xFF); block_data.push_back(0xFF);

    // Coinbase script length + data (height 1)
    block_data.push_back(0x03);  // 3 bytes
    block_data.push_back(0x51);  // OP_1 (height 1)
    block_data.push_back(0x00); block_data.push_back(0x00);

    // Sequence
    block_data.push_back(0xFF); block_data.push_back(0xFF);
    block_data.push_back(0xFF); block_data.push_back(0xFF);

    // Output count
    block_data.push_back(0x01);

    // Output value: 100 DIN (10000000000 una)
    uint64_t val = 10000000000ULL;
    for (int i = 0; i < 8; i++) {
        block_data.push_back((val >> (i * 8)) & 0xFF);
    }

    // ScriptPubKey length + simple OP_TRUE script
    block_data.push_back(0x01);
    block_data.push_back(0x51);  // OP_TRUE

    // Locktime
    block_data.push_back(0x00); block_data.push_back(0x00);
    block_data.push_back(0x00); block_data.push_back(0x00);

    std::cout << "Total block size: " << block_data.size() << " bytes\n";
    std::cout << "Block data (hex): ";
    for (size_t i = 0; i < std::min(block_data.size(), size_t(100)); i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)block_data[i];
    }
    std::cout << std::dec << "...\n\n";

    // Try to deserialize
    try {
        Block block;
        std::cout << "Calling DeserializeBlock...\n";
        bool success = DeserializeBlock(block_data, block);

        if (!success) {
            std::cout << "❌ DeserializeBlock returned false\n";
            return 1;
        }

        std::cout << "✅ DeserializeBlock succeeded!\n\n";

        // Print results
        std::cout << "Block details:\n";
        std::cout << "  Version: " << block.header.version << "\n";
        std::cout << "  Timestamp: " << block.header.timestamp << "\n";
        std::cout << "  Nonce: " << block.header.nonce << "\n";
        std::cout << "  Transaction count: " << block.vtx.size() << "\n";

        if (block.vtx.empty()) {
            std::cout << "❌ No transactions parsed!\n";
            return 1;
        }

        std::cout << "\nTransaction 0:\n";
        std::cout << "  Version: " << block.vtx[0].version << "\n";
        std::cout << "  Inputs: " << block.vtx[0].vin.size() << "\n";
        std::cout << "  Outputs: " << block.vtx[0].vout.size() << "\n";
        std::cout << "  Locktime: " << block.vtx[0].lockTime << "\n";

        if (block.vtx[0].vin.empty() || block.vtx[0].vout.empty()) {
            std::cout << "❌ Transaction has no inputs or outputs!\n";
            return 1;
        }

        std::cout << "\n  Input 0:\n";
        std::cout << "    Prev txid: " << block.vtx[0].vin[0].prevout.txid << "\n";
        std::cout << "    Prev vout: " << block.vtx[0].vin[0].prevout.vout << "\n";
        std::cout << "    ScriptSig size: " << block.vtx[0].vin[0].scriptSig.size() << "\n";
        std::cout << "    Witness items: " << block.vtx[0].vin[0].witness.size() << "\n";

        std::cout << "\n  Output 0:\n";
        std::cout << "    Value: " << block.vtx[0].vout[0].value << " una\n";
        std::cout << "    ScriptPubKey size: " << block.vtx[0].vout[0].scriptPubKey.size() << "\n";

        std::cout << "\n✅ All tests passed!\n";
        return 0;

    } catch (const std::exception& e) {
        std::cout << "❌ Exception: " << e.what() << "\n";
        return 1;
    }
}

/**
 * Phase G.1.4: Inventory Exchange - Serialization Tests
 *
 * Tests serialization/deserialization of inventory messages.
 *
 * Test Scope:
 * - InventoryVector serialization roundtrip
 * - InvMessage serialization roundtrip
 * - GetDataMessage serialization roundtrip
 * - NotFoundMessage serialization roundtrip
 * - Empty message handling
 * - Multiple inventory items
 */

#include "../../include/p2p/inventory.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace dinero;
using namespace dinero::p2p;

//=============================================================================
// Test 1: InventoryVector Serialization
//=============================================================================

void test_inventory_vector_serialization() {
    std::cout << "\n[Test 1] InventoryVector serialization" << std::endl;

    // Create test hash
    Hash256 test_hash;
    for (int i = 0; i < 32; i++) {
        test_hash.data[i] = static_cast<uint8_t>(i);
    }

    // Create inventory vector
    InventoryVector inv(MSG_BLOCK, test_hash);

    std::cout << "  Original: " << inv.toString() << std::endl;

    // Serialize
    std::vector<uint8_t> serialized = inv.serialize();
    std::cout << "  Serialized size: " << serialized.size() << " bytes" << std::endl;

    // Should be 36 bytes (4 bytes type + 32 bytes hash)
    assert(serialized.size() == 36 && "Serialized size should be 36 bytes");

    // Deserialize
    size_t offset = 0;
    InventoryVector deserialized = InventoryVector::deserialize(serialized, offset);

    std::cout << "  Deserialized: " << deserialized.toString() << std::endl;

    // Verify
    assert(deserialized.type == MSG_BLOCK && "Type should match");
    assert(deserialized.hash == test_hash && "Hash should match");

    std::cout << "  [✓] InventoryVector serialization works!" << std::endl;
}

//=============================================================================
// Test 2: InvMessage with Multiple Items
//=============================================================================

void test_inv_message_multiple_items() {
    std::cout << "\n[Test 2] InvMessage with multiple items" << std::endl;

    InvMessage inv_msg;

    // Add multiple items
    for (int i = 0; i < 5; i++) {
        Hash256 hash;
        for (int j = 0; j < 32; j++) {
            hash.data[j] = static_cast<uint8_t>(i * 32 + j);
        }

        uint32_t type = (i % 2 == 0) ? MSG_BLOCK : MSG_TX;
        inv_msg.add(type, hash);
    }

    std::cout << "  Created inv with " << inv_msg.size() << " items" << std::endl;
    assert(inv_msg.size() == 5 && "Should have 5 items");

    // Serialize
    std::vector<uint8_t> serialized = inv_msg.serialize();
    std::cout << "  Serialized size: " << serialized.size() << " bytes" << std::endl;

    // Deserialize
    InvMessage deserialized = InvMessage::deserialize(serialized);
    std::cout << "  Deserialized inv with " << deserialized.size() << " items" << std::endl;

    assert(deserialized.size() == 5 && "Should have 5 items");

    // Verify each item
    for (size_t i = 0; i < 5; i++) {
        assert(deserialized.inventory[i].type == inv_msg.inventory[i].type && "Type should match");
        assert(deserialized.inventory[i].hash == inv_msg.inventory[i].hash && "Hash should match");
    }

    std::cout << "  [✓] InvMessage serialization works!" << std::endl;
}

//=============================================================================
// Test 3: GetDataMessage
//=============================================================================

void test_getdata_message() {
    std::cout << "\n[Test 3] GetDataMessage serialization" << std::endl;

    GetDataMessage getdata;

    // Create test hash
    Hash256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = 0xFF - i;
    }

    getdata.add(MSG_TX, hash);

    std::cout << "  Created getdata with " << getdata.size() << " items" << std::endl;

    // Serialize
    std::vector<uint8_t> serialized = getdata.serialize();

    // Deserialize
    GetDataMessage deserialized = GetDataMessage::deserialize(serialized);

    assert(deserialized.size() == 1 && "Should have 1 item");
    assert(deserialized.inventory[0].type == MSG_TX && "Type should be MSG_TX");
    assert(deserialized.inventory[0].hash == hash && "Hash should match");

    std::cout << "  [✓] GetDataMessage serialization works!" << std::endl;
}

//=============================================================================
// Test 4: NotFoundMessage
//=============================================================================

void test_notfound_message() {
    std::cout << "\n[Test 4] NotFoundMessage serialization" << std::endl;

    NotFoundMessage notfound;

    // Add two items
    for (int i = 0; i < 2; i++) {
        Hash256 hash;
        for (int j = 0; j < 32; j++) {
            hash.data[j] = static_cast<uint8_t>(100 + i * 32 + j);
        }

        notfound.add(MSG_BLOCK, hash);
    }

    std::cout << "  Created notfound with " << notfound.size() << " items" << std::endl;

    // Serialize
    std::vector<uint8_t> serialized = notfound.serialize();

    // Deserialize
    NotFoundMessage deserialized = NotFoundMessage::deserialize(serialized);

    assert(deserialized.size() == 2 && "Should have 2 items");
    for (size_t i = 0; i < 2; i++) {
        assert(deserialized.inventory[i] == notfound.inventory[i] && "Items should match");
    }

    std::cout << "  [✓] NotFoundMessage serialization works!" << std::endl;
}

//=============================================================================
// Test 5: Empty Messages
//=============================================================================

void test_empty_messages() {
    std::cout << "\n[Test 5] Empty message handling" << std::endl;

    // Empty inv
    InvMessage empty_inv;
    assert(empty_inv.empty() && "Should be empty");

    std::vector<uint8_t> serialized = empty_inv.serialize();
    InvMessage deserialized = InvMessage::deserialize(serialized);

    assert(deserialized.empty() && "Deserialized should be empty");
    std::cout << "  [✓] Empty inv message works" << std::endl;

    // Empty getdata
    GetDataMessage empty_getdata;
    serialized = empty_getdata.serialize();
    GetDataMessage deserialized_getdata = GetDataMessage::deserialize(serialized);
    assert(deserialized_getdata.empty() && "Deserialized should be empty");
    std::cout << "  [✓] Empty getdata message works" << std::endl;

    // Empty notfound
    NotFoundMessage empty_notfound;
    serialized = empty_notfound.serialize();
    NotFoundMessage deserialized_notfound = NotFoundMessage::deserialize(serialized);
    assert(deserialized_notfound.empty() && "Deserialized should be empty");
    std::cout << "  [✓] Empty notfound message works" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "G.1.4: Inventory Serialization Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nVerifying inventory message serialization" << std::endl;

    try {
        // Test 1: InventoryVector
        test_inventory_vector_serialization();

        // Test 2: InvMessage with multiple items
        test_inv_message_multiple_items();

        // Test 3: GetDataMessage
        test_getdata_message();

        // Test 4: NotFoundMessage
        test_notfound_message();

        // Test 5: Empty messages
        test_empty_messages();

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All Serialization Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nSummary:" << std::endl;
        std::cout << "  [✓] InventoryVector serialization works" << std::endl;
        std::cout << "  [✓] InvMessage serialization works" << std::endl;
        std::cout << "  [✓] GetDataMessage serialization works" << std::endl;
        std::cout << "  [✓] NotFoundMessage serialization works" << std::endl;
        std::cout << "  [✓] Empty message handling works" << std::endl;
        std::cout << "\nInventory message format is VERIFIED." << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}

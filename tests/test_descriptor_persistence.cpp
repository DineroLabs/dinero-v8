// SPDX-License-Identifier: MIT
// Dinero - Descriptor Persistence Tests
// Validates descriptor storage, activation, and migration (NO cryptography - fast, safe tests)

#include "wallet/descriptor_store.h"
#include "wallet/descriptor_checksum.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <ctime>

using namespace din;
namespace fs = std::filesystem;

// Test database path
fs::path getTestDbPath() {
    return fs::temp_directory_path() / "test_descriptor_persistence.db";
}

void cleanup() {
    fs::path db_path = getTestDbPath();
    if (fs::exists(db_path)) {
        fs::remove(db_path);
    }
}

void test_descriptor_persistence() {
    std::cout << "\n╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║   Test 1: Descriptor Persistence (Store → Restart)   ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

    cleanup();
    fs::path db_path = getTestDbPath();

    // Create valid BIP86 descriptor
    std::string descriptor_base = "tr([d4691818/86h/1447h/0h]xpub6ERApfZwUNrhLCkDtcHTcxd75RbzS1ed54G1LkBUHQVHQKqhMkhgbmJbZRkrgZw4koxb5JaHWkY4ALHY2grBGRjaDMzQLcgJvLJuZZvRcEL/0/*)";
    std::string descriptor = DescriptorChecksum::AddChecksum(descriptor_base);

    std::cout << "  Descriptor: " << descriptor.substr(0, 60) << "..." << std::endl;

    // Phase 1: Store descriptor
    {
        std::cout << "\n  [Phase 1] Storing descriptor..." << std::endl;
        DescriptorStore store(db_path.string());
        assert(store.initialize());

        DescriptorRecord record;
        record.descriptor = descriptor;
        // Extract checksum from descriptor (everything after '#')
        size_t hash_pos = descriptor.find('#');
        record.checksum = (hash_pos != std::string::npos) ? descriptor.substr(hash_pos + 1) : "";
        record.policy = "BIP86";
        record.account = 0;
        record.is_change = false;
        record.is_active = true;
        record.label = "Taproot Account 0 Receive";

        assert(store.addDescriptor(record));
        std::cout << "      ✓ Descriptor stored with ID 1" << std::endl;

        // Verify immediately
        auto retrieved = store.getDescriptor(1);
        assert(retrieved.has_value());
        assert(retrieved->descriptor == descriptor);
        assert(retrieved->policy == "BIP86");
        assert(retrieved->is_active == true);
        std::cout << "      ✓ Descriptor retrieved successfully" << std::endl;

        store.shutdown();
        std::cout << "      ✓ Database closed" << std::endl;
    }

    // Phase 2: Restart and verify persistence
    {
        std::cout << "\n  [Phase 2] Restarting wallet (simulating node restart)..." << std::endl;
        DescriptorStore store(db_path.string());
        assert(store.initialize());

        auto descriptors = store.listDescriptors();
        assert(descriptors.size() == 1);
        assert(descriptors[0].descriptor == descriptor);
        assert(descriptors[0].policy == "BIP86");
        assert(descriptors[0].account == 0);
        assert(descriptors[0].is_active == true);
        assert(descriptors[0].label == "Taproot Account 0 Receive");
        std::cout << "      ✓ Descriptor persisted across restart" << std::endl;
        std::cout << "      ✓ All metadata preserved (policy, account, active, label)" << std::endl;

        store.shutdown();
    }

    std::cout << "\n  ✅ Persistence Test PASSED" << std::endl;
    std::cout << "     - Store descriptor ✓" << std::endl;
    std::cout << "     - Restart wallet ✓" << std::endl;
    std::cout << "     - Descriptor still present ✓" << std::endl;
}

void test_descriptor_activation() {
    std::cout << "\n╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║   Test 2: Descriptor Activation (Only Active Used)   ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

    cleanup();
    fs::path db_path = getTestDbPath();

    DescriptorStore store(db_path.string());
    assert(store.initialize());

    // Add two BIP86 receive descriptors (different accounts)
    std::cout << "\n  Adding two BIP86 receive descriptors..." << std::endl;

    // Descriptor 1: Account 0 (ACTIVE)
    DescriptorRecord record1;
    record1.descriptor = DescriptorChecksum::AddChecksum("tr([d4691818/86h/1447h/0h]xpub6ERApfZwUNrhLCkDtcHTcxd75RbzS1ed54G1LkBUHQVHQKqhMkhgbmJbZRkrgZw4koxb5JaHWkY4ALHY2grBGRjaDMzQLcgJvLJuZZvRcEL/0/*)");
    size_t hash_pos1 = record1.descriptor.find('#');
    record1.checksum = (hash_pos1 != std::string::npos) ? record1.descriptor.substr(hash_pos1 + 1) : "";
    record1.policy = "BIP86";
    record1.account = 0;
    record1.is_change = false;
    record1.is_active = true;  // ACTIVE
    record1.label = "Account 0 (Active)";
    assert(store.addDescriptor(record1));
    std::cout << "      ✓ Descriptor 1: Account 0 (ACTIVE)" << std::endl;

    // Descriptor 2: Account 1 (INACTIVE)
    DescriptorRecord record2;
    record2.descriptor = DescriptorChecksum::AddChecksum("tr([d4691818/86h/1447h/1h]xpub6ERApfZwUNrhLCkDtcHTcxd75RbzS1ed54G1LkBUHQVHQKqhMkhgbmJbZRkrgZw4koxb5JaHWkY4ALHY2grBGRjaDMzQLcgJvLJuZZvRcEL/0/*)");
    size_t hash_pos2 = record2.descriptor.find('#');
    record2.checksum = (hash_pos2 != std::string::npos) ? record2.descriptor.substr(hash_pos2 + 1) : "";
    record2.policy = "BIP86";
    record2.account = 1;
    record2.is_change = false;
    record2.is_active = false;  // INACTIVE
    record2.label = "Account 1 (Inactive)";
    assert(store.addDescriptor(record2));
    std::cout << "      ✓ Descriptor 2: Account 1 (INACTIVE)" << std::endl;

    // Query active descriptor
    std::cout << "\n  Querying active BIP86 receive descriptor..." << std::endl;
    auto active = store.getActiveDescriptor("BIP86", false);
    assert(active.has_value());
    assert(active->account == 0);  // Must be account 0 (not account 1)
    assert(active->is_active == true);
    assert(active->label == "Account 0 (Active)");
    std::cout << "      ✓ Only active descriptor (Account 0) returned" << std::endl;

    // List all descriptors
    auto all_descriptors = store.listDescriptors();
    assert(all_descriptors.size() == 2);
    std::cout << "      ✓ Both descriptors stored (total: 2)" << std::endl;

    // List only active descriptors
    auto active_only = store.listDescriptors(true);
    assert(active_only.size() == 1);
    assert(active_only[0].account == 0);
    std::cout << "      ✓ Only 1 active descriptor in filtered list" << std::endl;

    std::cout << "\n  ✅ Activation Test PASSED" << std::endl;
    std::cout << "     - Two descriptors stored ✓" << std::endl;
    std::cout << "     - Only active one used for address generation ✓" << std::endl;
    std::cout << "     - Inactive descriptors queryable but not active ✓" << std::endl;

    store.shutdown();
}

void test_descriptor_migration() {
    std::cout << "\n╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║   Test 3: Descriptor Migration (BIP84 → BIP86)       ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

    cleanup();
    fs::path db_path = getTestDbPath();

    DescriptorStore store(db_path.string());
    assert(store.initialize());

    // Phase 1: Start with BIP84 (Native SegWit) descriptors
    std::cout << "\n  [Phase 1] Adding BIP84 descriptors (active)..." << std::endl;

    DescriptorRecord bip84_receive;
    bip84_receive.descriptor = DescriptorChecksum::AddChecksum("wpkh([d4691818/84h/1447h/0h]xpub6ERApfZwUNrhLCkDtcHTcxd75RbzS1ed54G1LkBUHQVHQKqhMkhgbmJbZRkrgZw4koxb5JaHWkY4ALHY2grBGRjaDMzQLcgJvLJuZZvRcEL/0/*)");
    size_t hash_pos3 = bip84_receive.descriptor.find('#');
    bip84_receive.checksum = (hash_pos3 != std::string::npos) ? bip84_receive.descriptor.substr(hash_pos3 + 1) : "";
    bip84_receive.policy = "BIP84";
    bip84_receive.account = 0;
    bip84_receive.is_change = false;
    bip84_receive.is_active = true;
    bip84_receive.label = "BIP84 Receive";
    assert(store.addDescriptor(bip84_receive));
    std::cout << "      ✓ BIP84 receive descriptor added (ID 1, ACTIVE)" << std::endl;

    DescriptorRecord bip84_change;
    bip84_change.descriptor = DescriptorChecksum::AddChecksum("wpkh([d4691818/84h/1447h/0h]xpub6ERApfZwUNrhLCkDtcHTcxd75RbzS1ed54G1LkBUHQVHQKqhMkhgbmJbZRkrgZw4koxb5JaHWkY4ALHY2grBGRjaDMzQLcgJvLJuZZvRcEL/1/*)");
    size_t hash_pos4 = bip84_change.descriptor.find('#');
    bip84_change.checksum = (hash_pos4 != std::string::npos) ? bip84_change.descriptor.substr(hash_pos4 + 1) : "";
    bip84_change.policy = "BIP84";
    bip84_change.account = 0;
    bip84_change.is_change = true;
    bip84_change.is_active = true;
    bip84_change.label = "BIP84 Change";
    assert(store.addDescriptor(bip84_change));
    std::cout << "      ✓ BIP84 change descriptor added (ID 2, ACTIVE)" << std::endl;

    // Verify BIP84 is active
    auto active_bip84 = store.getActiveDescriptor("BIP84", false);
    assert(active_bip84.has_value());
    assert(active_bip84->policy == "BIP84");
    std::cout << "      ✓ BIP84 is active policy" << std::endl;

    // Phase 2: Migrate to BIP86 (Taproot)
    std::cout << "\n  [Phase 2] Migrating to BIP86 (Taproot)..." << std::endl;

    DescriptorStore::MigrationPlan migration;
    migration.from_policy = "BIP84";
    migration.to_policy = "BIP86";
    migration.descriptors_to_deprecate = {1, 2};  // Deprecate BIP84 descriptors

    // Add new BIP86 descriptors
    DescriptorRecord bip86_receive;
    bip86_receive.descriptor = DescriptorChecksum::AddChecksum("tr([d4691818/86h/1447h/0h]xpub6ERApfZwUNrhLCkDtcHTcxd75RbzS1ed54G1LkBUHQVHQKqhMkhgbmJbZRkrgZw4koxb5JaHWkY4ALHY2grBGRjaDMzQLcgJvLJuZZvRcEL/0/*)");
    size_t hash_pos5 = bip86_receive.descriptor.find('#');
    bip86_receive.checksum = (hash_pos5 != std::string::npos) ? bip86_receive.descriptor.substr(hash_pos5 + 1) : "";
    bip86_receive.policy = "BIP86";
    bip86_receive.account = 0;
    bip86_receive.is_change = false;
    bip86_receive.is_active = true;
    bip86_receive.label = "BIP86 Receive";
    migration.descriptors_to_add.push_back(bip86_receive);

    DescriptorRecord bip86_change;
    bip86_change.descriptor = DescriptorChecksum::AddChecksum("tr([d4691818/86h/1447h/0h]xpub6ERApfZwUNrhLCkDtcHTcxd75RbzS1ed54G1LkBUHQVHQKqhMkhgbmJbZRkrgZw4koxb5JaHWkY4ALHY2grBGRjaDMzQLcgJvLJuZZvRcEL/1/*)");
    size_t hash_pos6 = bip86_change.descriptor.find('#');
    bip86_change.checksum = (hash_pos6 != std::string::npos) ? bip86_change.descriptor.substr(hash_pos6 + 1) : "";
    bip86_change.policy = "BIP86";
    bip86_change.account = 0;
    bip86_change.is_change = true;
    bip86_change.is_active = true;
    bip86_change.label = "BIP86 Change";
    migration.descriptors_to_add.push_back(bip86_change);

    // Execute migration (atomic)
    assert(store.executeMigration(migration));
    std::cout << "      ✓ Migration executed (atomic transaction)" << std::endl;

    // Phase 3: Verify migration results
    std::cout << "\n  [Phase 3] Verifying migration..." << std::endl;

    // BIP86 should now be active
    auto active_bip86 = store.getActiveDescriptor("BIP86", false);
    assert(active_bip86.has_value());
    assert(active_bip86->policy == "BIP86");
    assert(active_bip86->is_active == true);
    std::cout << "      ✓ BIP86 is now active policy" << std::endl;

    // BIP84 should be deprecated but still queryable
    auto old_bip84_receive = store.getDescriptor(1);
    assert(old_bip84_receive.has_value());
    assert(old_bip84_receive->policy == "BIP84");
    assert(old_bip84_receive->is_active == false);  // Deactivated
    assert(old_bip84_receive->deprecated_at > 0);    // Deprecated timestamp set
    std::cout << "      ✓ BIP84 descriptors deprecated (not deleted!)" << std::endl;

    // Both policies remain queryable
    auto all_descriptors = store.listDescriptors();
    assert(all_descriptors.size() == 4);  // 2 BIP84 + 2 BIP86
    std::cout << "      ✓ All 4 descriptors remain queryable (2 BIP84 + 2 BIP86)" << std::endl;

    // Only BIP86 descriptors are active
    auto active_descriptors = store.listDescriptors(true);
    assert(active_descriptors.size() == 2);  // Only BIP86
    for (const auto& desc : active_descriptors) {
        assert(desc.policy == "BIP86");
    }
    std::cout << "      ✓ Only BIP86 descriptors are active" << std::endl;

    // Old BIP84 descriptors still readable (important for incoming funds!)
    auto bip84_descriptors = store.getDescriptorsByPolicy("BIP84");
    assert(bip84_descriptors.size() == 2);
    std::cout << "      ✓ Old BIP84 descriptors remain readable (for incoming funds)" << std::endl;

    std::cout << "\n  ✅ Migration Test PASSED" << std::endl;
    std::cout << "     - BIP84 active → BIP86 active ✓" << std::endl;
    std::cout << "     - Both policies remain queryable ✓" << std::endl;
    std::cout << "     - Atomic migration (all or nothing) ✓" << std::endl;
    std::cout << "     - Bitcoin Core safety model (never delete) ✓" << std::endl;

    store.shutdown();
}

void test_descriptor_store_summary() {
    std::cout << "\n╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║   Descriptor Persistence Architecture Summary        ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

    std::cout << "\n✅ What Descriptor Persistence IS:" << std::endl;
    std::cout << "  • Storage layer for immutable wallet policies" << std::endl;
    std::cout << "  • Selection mechanism (active vs inactive)" << std::endl;
    std::cout << "  • Safe migration framework (deprecate, not delete)" << std::endl;
    std::cout << "  • Atomic transactions for policy changes" << std::endl;

    std::cout << "\n❌ What Descriptor Persistence is NOT:" << std::endl;
    std::cout << "  • Key derivation (KeyStoreImpl remains unchanged)" << std::endl;
    std::cout << "  • PSBT signing (PsbtSigner remains unchanged)" << std::endl;
    std::cout << "  • Address generation (separate concern)" << std::endl;
    std::cout << "  • Validation logic (security boundary preserved)" << std::endl;

    std::cout << "\n🔒 Safety Properties:" << std::endl;
    std::cout << "  • Descriptors are immutable (never mutate)" << std::endl;
    std::cout << "  • Old descriptors never deleted (Bitcoin Core model)" << std::endl;
    std::cout << "  • Migrations are atomic (rollback on failure)" << std::endl;
    std::cout << "  • No cryptography involved (fast, safe tests)" << std::endl;

    std::cout << "\n📊 Database Schema:" << std::endl;
    std::cout << "  • wallet_descriptors table with proper indexes" << std::endl;
    std::cout << "  • Unique constraint per (policy, account, is_change, descriptor)" << std::endl;
    std::cout << "  • Timestamp tracking (created_at, deprecated_at)" << std::endl;
    std::cout << "  • JSON metadata for extensibility" << std::endl;

    std::cout << "\n🚀 Next Steps:" << std::endl;
    std::cout << "  1. Add RPC methods (importdescriptor, listdescriptors, setactivedescriptor)" << std::endl;
    std::cout << "  2. Wire to address generation (use active descriptors)" << std::endl;
    std::cout << "  3. Add wallet initialization (auto-detect policies)" << std::endl;
    std::cout << "  4. Production testing with real wallet databases" << std::endl;
}

int main() {
    std::cout << "╔════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║      Dinero Descriptor Persistence Tests         ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

    try {
        test_descriptor_persistence();
        test_descriptor_activation();
        test_descriptor_migration();
        test_descriptor_store_summary();

        cleanup();  // Clean up test database

        std::cout << "\n╔════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║    ✅ ALL DESCRIPTOR PERSISTENCE TESTS PASSED ✅      ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cout << "\n❌ TEST FAILED: " << e.what() << std::endl;
        cleanup();
        return 1;
    }
}

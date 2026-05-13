/**
 * Descriptor RPC Integration Test
 *
 * Tests the descriptor RPC methods:
 * - descriptor.import
 * - descriptor.list
 * - descriptor.setactive
 */

#include "wallet/descriptor_store.h"
#include "wallet/descriptor_checksum.h"
#include "din_json.h"
#include "rpc/rpc_registry.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

// Forward declarations from methods_descriptor.cpp
namespace din {
namespace rpc {
    extern din::Json importdescriptor_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json listdescriptors_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json setactivedescriptor_impl(const ExecutionContext& ctx, const din::Json& params);
}
}

// Helper to create descriptor with correct checksum
std::string make_descriptor_with_checksum(const std::string& desc_without_checksum) {
    std::string checksum = din::DescriptorChecksum::Compute(desc_without_checksum);
    return desc_without_checksum + "#" + checksum;
}

// Test descriptors (checksums will be computed)
const std::string TEST_DESC_BASE_1 = "tr([d4691818/86h/1447h/0h]xpub6DDUPHpUo4pcy43iJeZjbSVWGav1SMMmuWdMHiGtkK8rhKmfbomtkwW6GKs1GGAKehT6QRocrmda3WWxXawpjmwaUHfFRXuKrXSapdckEYF/0/*)";
const std::string TEST_DESC_BASE_2 = "tr([d4691818/86h/1447h/0h]xpub6DDUPHpUo4pcy43iJeZjbSVWGav1SMMmuWdMHiGtkK8rhKmfbomtkwW6GKs1GGAKehT6QRocrmda3WWxXawpjmwaUHfFRXuKrXSapdckEYF/1/*)";

void test_import_descriptor() {
    std::cout << "[TEST] Import Descriptor RPC..." << std::endl;

    ExecutionContext ctx;

    // Generate descriptors with correct checksums
    std::string test_descriptor_1 = make_descriptor_with_checksum(TEST_DESC_BASE_1);
    std::cout << "  Using descriptor: " << test_descriptor_1 << std::endl;

    // Test 1: Import valid descriptor
    din::Json params = din::obj();
    params["descriptor"] = test_descriptor_1;
    params["policy"] = "BIP86";
    params["account"] = 0;
    params["is_change"] = false;
    params["active"] = true;
    params["label"] = "Test BIP86 Receive";
    params["wallet"] = "test_rpc";

    din::Json response = din::rpc::importdescriptor_impl(ctx, params);

    // Debug: Print response
    std::cout << "  Response: " << response.toStyledString() << std::endl;

    // Verify success
    assert(response["rpc_schema"] == "din.rpc.v1");
    if (response.isMember("error")) {
        std::cerr << "  ❌ Error: " << response["error"]["message"].asString() << std::endl;
    }
    assert(response.isMember("result"));
    assert(response["result"]["success"].asBool() == true);
    assert(response["result"]["policy"] == "BIP86");
    assert(response["result"]["active"].asBool() == true);

    std::cout << "  ✅ Import descriptor succeeded" << std::endl;
    std::cout << "  - ID: " << response["result"]["id"].asInt64() << std::endl;
    std::cout << "  - Policy: " << response["result"]["policy"].asString() << std::endl;
    std::cout << "  - Active: " << response["result"]["active"].asBool() << std::endl;

    // Test 2: Import duplicate descriptor (should fail)
    din::Json response2 = din::rpc::importdescriptor_impl(ctx, params);
    assert(response2.isMember("error"));
    assert(response2["error"]["code"].asInt() == -4);
    std::cout << "  ✅ Duplicate import correctly rejected" << std::endl;

    // Test 3: Import without checksum (should fail)
    din::Json params_no_checksum = din::obj();
    params_no_checksum["descriptor"] = "tr([fpr/86h/1447h/0h]xpub/0/*)";  // No checksum
    params_no_checksum["policy"] = "BIP86";
    params_no_checksum["account"] = 1;
    params_no_checksum["is_change"] = false;

    din::Json response3 = din::rpc::importdescriptor_impl(ctx, params_no_checksum);
    assert(response3.isMember("error"));
    assert(response3["error"]["code"].asInt() == -5);  // Invalid checksum
    std::cout << "  ✅ Invalid checksum correctly rejected" << std::endl;
}

void test_list_descriptors() {
    std::cout << "[TEST] List Descriptors RPC..." << std::endl;

    ExecutionContext ctx;

    // Generate descriptor 2 with correct checksum
    std::string test_descriptor_2 = make_descriptor_with_checksum(TEST_DESC_BASE_2);

    // Import a second descriptor first
    din::Json import_params = din::obj();
    import_params["descriptor"] = test_descriptor_2;
    import_params["policy"] = "BIP86";
    import_params["account"] = 0;
    import_params["is_change"] = true;
    import_params["active"] = false;
    import_params["label"] = "Test BIP86 Change";
    import_params["wallet"] = "test_rpc";

    din::Json import_response = din::rpc::importdescriptor_impl(ctx, import_params);
    assert(import_response["result"]["success"].asBool() == true);

    // Test 1: List all descriptors
    din::Json list_params = din::obj();
    list_params["wallet"] = "test_rpc";

    din::Json response = din::rpc::listdescriptors_impl(ctx, list_params);
    assert(response["rpc_schema"] == "din.rpc.v1");
    assert(response.isMember("result"));
    assert(response["result"]["descriptors"].isArray());
    assert(response["result"]["count"].asInt() >= 2);

    std::cout << "  ✅ List all descriptors: " << response["result"]["count"].asInt() << " found" << std::endl;

    // Test 2: Filter by active
    din::Json active_params = din::obj();
    active_params["wallet"] = "test_rpc";
    active_params["active"] = true;

    din::Json active_response = din::rpc::listdescriptors_impl(ctx, active_params);
    assert(active_response["result"]["descriptors"].isArray());
    assert(active_response["result"]["count"].asInt() >= 1);

    std::cout << "  ✅ Filter by active: " << active_response["result"]["count"].asInt() << " found" << std::endl;

    // Test 3: Filter by policy
    din::Json policy_params = din::obj();
    policy_params["wallet"] = "test_rpc";
    policy_params["policy"] = "BIP86";

    din::Json policy_response = din::rpc::listdescriptors_impl(ctx, policy_params);
    assert(policy_response["result"]["descriptors"].isArray());
    assert(policy_response["result"]["count"].asInt() >= 2);

    std::cout << "  ✅ Filter by policy: " << policy_response["result"]["count"].asInt() << " BIP86 descriptors" << std::endl;
}

void test_set_active_descriptor() {
    std::cout << "[TEST] Set Active Descriptor RPC..." << std::endl;

    ExecutionContext ctx;

    // Get current descriptors to find IDs
    din::Json list_params = din::obj();
    list_params["wallet"] = "test_rpc";

    din::Json list_response = din::rpc::listdescriptors_impl(ctx, list_params);
    assert(list_response["result"]["descriptors"].size() >= 2);

    int64_t id1 = list_response["result"]["descriptors"][0]["id"].asInt64();
    int64_t id2 = list_response["result"]["descriptors"][1]["id"].asInt64();
    bool is_change_1 = list_response["result"]["descriptors"][0]["is_change"].asBool();
    bool is_change_2 = list_response["result"]["descriptors"][1]["is_change"].asBool();

    std::cout << "  - Descriptor 1: ID=" << id1 << ", is_change=" << is_change_1 << std::endl;
    std::cout << "  - Descriptor 2: ID=" << id2 << ", is_change=" << is_change_2 << std::endl;

    // Test 1: Activate descriptor 2 (change)
    din::Json activate_params = din::obj();
    activate_params["id"] = Json::Int64(id2);
    activate_params["active"] = true;
    activate_params["wallet"] = "test_rpc";

    din::Json activate_response = din::rpc::setactivedescriptor_impl(ctx, activate_params);
    assert(activate_response["rpc_schema"] == "din.rpc.v1");
    assert(activate_response.isMember("result"));
    assert(activate_response["result"]["success"].asBool() == true);
    assert(activate_response["result"]["id"].asInt64() == id2);
    assert(activate_response["result"]["active"].asBool() == true);

    std::cout << "  ✅ Activated descriptor " << id2 << std::endl;

    // Test 2: Deactivate descriptor
    din::Json deactivate_params = din::obj();
    deactivate_params["id"] = Json::Int64(id2);
    deactivate_params["active"] = false;
    deactivate_params["wallet"] = "test_rpc";

    din::Json deactivate_response = din::rpc::setactivedescriptor_impl(ctx, deactivate_params);
    assert(deactivate_response["result"]["success"].asBool() == true);
    assert(deactivate_response["result"]["active"].asBool() == false);

    std::cout << "  ✅ Deactivated descriptor " << id2 << std::endl;

    // Test 3: Try to activate non-existent descriptor (should fail)
    din::Json invalid_params = din::obj();
    invalid_params["id"] = Json::Int64(99999);
    invalid_params["active"] = true;
    invalid_params["wallet"] = "test_rpc";

    din::Json invalid_response = din::rpc::setactivedescriptor_impl(ctx, invalid_params);
    assert(invalid_response.isMember("error"));
    assert(invalid_response["error"]["code"].asInt() == -5);

    std::cout << "  ✅ Invalid descriptor ID correctly rejected" << std::endl;
}

int main() {
    std::cout << "=== Descriptor RPC Integration Tests ===" << std::endl;

    try {
        // Clean up test database before starting
        const char* home = std::getenv("HOME");
        if (home) {
            fs::path test_db = fs::path(home) / ".dinero" / "wallets" / "descriptor_test_rpc.db";
            if (fs::exists(test_db)) {
                fs::remove(test_db);
                std::cout << "[SETUP] Cleaned up test database" << std::endl;
            }
        }

        test_import_descriptor();
        test_list_descriptors();
        test_set_active_descriptor();

        std::cout << "\n✅ All descriptor RPC tests passed!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
        return 1;
    }
}

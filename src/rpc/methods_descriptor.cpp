/**
 * Descriptor RPC Methods - Phase 3C Descriptor Persistence
 *
 * Provides RPC interface for descriptor wallet management:
 * - importdescriptor: Import descriptor into wallet
 * - listdescriptors: List all descriptors (with filtering)
 * - setactivedescriptor: Activate/deactivate descriptors
 *
 * These RPCs are pure storage + selection layer operations.
 * They do NOT touch key derivation, signing, or address generation.
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "wallet/descriptor_store.h"
#include "wallet/descriptor_checksum.h"
#include "wallet/retired_coin_type_guard.h"
#include "wallet/bip84_descriptor.h"
#include "wallet/bip86_descriptor.h"
#include <memory>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

namespace din {
namespace rpc {

// Helper: Get descriptor store path for wallet
static std::string getDescriptorStorePath(const std::string& wallet_name = "default") {
    const char* home = std::getenv("HOME");
    if (!home) {
        throw std::runtime_error("HOME environment variable not set");
    }

    fs::path wallet_dir = fs::path(home) / ".dinero" / "wallets";
    fs::create_directories(wallet_dir);

    return (wallet_dir / ("descriptor_" + wallet_name + ".db")).string();
}

static std::string stripDescriptorChecksum(const std::string& descriptor) {
    const size_t hash_pos = descriptor.find('#');
    return hash_pos == std::string::npos ? descriptor : descriptor.substr(0, hash_pos);
}

static bool validateCanonicalDescriptor(const std::string& descriptor,
                                        const std::string& policy,
                                        std::string& error) {
    const std::string body = stripDescriptorChecksum(descriptor);
    if (policy == "BIP86" || policy == "bip86" || policy == "tr") {
        return BIP86DescriptorFactory::validateBIP86Descriptor(body, error);
    }
    if (policy == "BIP84" || policy == "bip84" || policy == "wpkh") {
        return BIP84DescriptorFactory::validateBIP84Descriptor(body, error);
    }
    error = "Unsupported descriptor policy for canonical coin_type validation: " + policy;
    return false;
}

/**
 * importdescriptor
 *
 * Import a descriptor into the wallet database.
 *
 * Arguments:
 * {
 *   "descriptor": "tr([fpr/86h/1448h/0h]xpub/0/\\*)#checksum",  // Required
 *   "policy": "BIP86",                                        // Required (BIP84, BIP86, etc.)
 *   "account": 0,                                             // Required
 *   "is_change": false,                                       // Required
 *   "active": true,                                           // Optional (default: false)
 *   "label": "Taproot Account 0 Receive",                     // Optional
 *   "wallet": "default"                                       // Optional (default: "default")
 * }
 *
 * Returns:
 * {
 *   "success": true,
 *   "id": 1,
 *   "descriptor": "tr([fpr/86h/1448h/0h]xpub/0/\\*)#checksum",
 *   "policy": "BIP86",
 *   "active": true
 * }
 */
din::Json importdescriptor_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";

    // Validate required parameters
    if (!params.isMember("descriptor") || !params["descriptor"].isString()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "Missing required parameter: descriptor";
        return reply;
    }

    if (!params.isMember("policy") || !params["policy"].isString()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "Missing required parameter: policy";
        return reply;
    }

    if (!params.isMember("account") || !params["account"].isInt()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "Missing required parameter: account";
        return reply;
    }

    if (!params.isMember("is_change") || !params["is_change"].isBool()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "Missing required parameter: is_change";
        return reply;
    }

    try {
        std::string descriptor = params["descriptor"].asString();
        dinero::wallet::RejectRetiredLegacyCoinTypeText(descriptor, "importdescriptor");
        std::string policy = params["policy"].asString();
        uint32_t account = params["account"].asUInt();
        bool is_change = params["is_change"].asBool();
        bool active = params.get("active", false).asBool();
        std::string label = params.get("label", "").asString();
        std::string wallet_name = params.get("wallet", "default").asString();

        // Verify descriptor checksum
        if (!DescriptorChecksum::Verify(descriptor)) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -5;
            reply["error"]["message"] = "Invalid descriptor checksum";
            return reply;
        }

        std::string validation_error;
        if (!validateCanonicalDescriptor(descriptor, policy, validation_error)) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -8;
            reply["error"]["message"] = validation_error;
            return reply;
        }

        // Extract checksum
        size_t hash_pos = descriptor.find('#');
        std::string checksum = (hash_pos != std::string::npos) ? descriptor.substr(hash_pos + 1) : "";

        // Open descriptor store
        std::string db_path = getDescriptorStorePath(wallet_name);
        DescriptorStore store(db_path);
        if (!store.initialize()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "Failed to initialize descriptor store";
            return reply;
        }

        // Check for duplicate
        auto existing = store.findDescriptor(descriptor);
        if (existing.has_value()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -4;
            reply["error"]["message"] = "Descriptor already exists (ID: " + std::to_string(existing->id) + ")";
            return reply;
        }

        // Create descriptor record
        DescriptorRecord record;
        record.descriptor = descriptor;
        record.checksum = checksum;
        record.policy = policy;
        record.account = account;
        record.is_change = is_change;
        record.is_active = active;
        record.label = label;

        // Add to store
        if (!store.addDescriptor(record)) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "Failed to add descriptor to store";
            return reply;
        }

        // Retrieve to get ID
        auto added = store.findDescriptor(descriptor);
        if (!added.has_value()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "Failed to retrieve added descriptor";
            return reply;
        }

        // Build success response
        din::Json result = din::obj();
        result["success"] = true;
        result["id"] = Json::Int64(added->id);
        result["descriptor"] = added->descriptor;
        result["policy"] = added->policy;
        result["account"] = added->account;
        result["is_change"] = added->is_change;
        result["active"] = added->is_active;
        result["label"] = added->label;
        result["created_at"] = Json::Int64(added->created_at);

        reply["result"] = result;
        return reply;

    } catch (const std::exception& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("Failed to import descriptor: ") + e.what();
        return reply;
    }
}

/**
 * listdescriptors
 *
 * List all descriptors in the wallet.
 *
 * Arguments:
 * {
 *   "active": true,           // Optional: Filter by active status
 *   "policy": "BIP86",        // Optional: Filter by policy
 *   "wallet": "default"       // Optional: Wallet name
 * }
 *
 * Returns:
 * {
 *   "descriptors": [
 *     {
 *       "id": 1,
 *       "descriptor": "tr([fpr/86h/1448h/0h]xpub/0/\\*)#checksum",
 *       "policy": "BIP86",
 *       "account": 0,
 *       "is_change": false,
 *       "active": true,
 *       "label": "Taproot Account 0 Receive",
 *       "created_at": 1234567890,
 *       "deprecated_at": 0
 *     }
 *   ],
 *   "count": 1
 * }
 */
din::Json listdescriptors_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";

    try {
        std::string wallet_name = params.get("wallet", "default").asString();
        bool filter_active = params.isMember("active");
        bool active_value = params.get("active", false).asBool();
        bool filter_policy = params.isMember("policy");
        std::string policy_value = params.get("policy", "").asString();

        // Open descriptor store
        std::string db_path = getDescriptorStorePath(wallet_name);
        DescriptorStore store(db_path);
        if (!store.initialize()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "Failed to initialize descriptor store";
            return reply;
        }

        // Get descriptors
        std::vector<DescriptorRecord> descriptors;
        if (filter_policy) {
            descriptors = store.getDescriptorsByPolicy(policy_value);
        } else if (filter_active) {
            descriptors = store.listDescriptors(active_value);
        } else {
            descriptors = store.listDescriptors(false);  // All descriptors
        }

        // Build response
        din::Json result = din::obj();
        din::Json desc_array = din::arr();

        for (const auto& desc : descriptors) {
            // Apply additional filtering if both policy and active specified
            if (filter_active && filter_policy) {
                if (desc.is_active != active_value) continue;
            }

            din::Json desc_obj = din::obj();
            desc_obj["id"] = Json::Int64(desc.id);
            desc_obj["descriptor"] = desc.descriptor;
            desc_obj["policy"] = desc.policy;
            desc_obj["account"] = desc.account;
            desc_obj["is_change"] = desc.is_change;
            desc_obj["active"] = desc.is_active;
            desc_obj["label"] = desc.label;
            desc_obj["created_at"] = Json::Int64(desc.created_at);
            desc_obj["deprecated_at"] = Json::Int64(desc.deprecated_at);

            desc_array.append(desc_obj);
        }

        result["descriptors"] = desc_array;
        result["count"] = static_cast<int>(desc_array.size());

        reply["result"] = result;
        return reply;

    } catch (const std::exception& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("Failed to list descriptors: ") + e.what();
        return reply;
    }
}

/**
 * setactivedescriptor
 *
 * Activate or deactivate a descriptor.
 *
 * IMPORTANT: Only one descriptor per (policy, is_change) can be active at a time.
 * This RPC will automatically deactivate other descriptors when activating a new one.
 *
 * Arguments:
 * {
 *   "id": 1,                 // Required: Descriptor ID
 *   "active": true,          // Required: Active status
 *   "wallet": "default"      // Optional: Wallet name
 * }
 *
 * Returns:
 * {
 *   "success": true,
 *   "id": 1,
 *   "active": true,
 *   "deactivated": [2, 3]    // IDs of descriptors that were deactivated
 * }
 */
din::Json setactivedescriptor_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json reply;
    reply["rpc_schema"] = "din.rpc.v1";

    // Validate required parameters
    if (!params.isMember("id") || !params["id"].isInt64()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "Missing required parameter: id";
        return reply;
    }

    if (!params.isMember("active") || !params["active"].isBool()) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -8;
        reply["error"]["message"] = "Missing required parameter: active";
        return reply;
    }

    try {
        int64_t id = params["id"].asInt64();
        bool active = params["active"].asBool();
        std::string wallet_name = params.get("wallet", "default").asString();

        // Open descriptor store
        std::string db_path = getDescriptorStorePath(wallet_name);
        DescriptorStore store(db_path);
        if (!store.initialize()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "Failed to initialize descriptor store";
            return reply;
        }

        // Get descriptor to check if it exists
        auto descriptor_opt = store.getDescriptor(id);
        if (!descriptor_opt.has_value()) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -5;
            reply["error"]["message"] = "Descriptor not found (ID: " + std::to_string(id) + ")";
            return reply;
        }

        auto descriptor = descriptor_opt.value();
        din::Json deactivated_ids = din::arr();

        // If activating, deactivate other descriptors with same (policy, is_change)
        if (active) {
            auto same_policy = store.getDescriptorsByPolicy(descriptor.policy);
            for (const auto& other : same_policy) {
                if (other.id != id && other.is_change == descriptor.is_change && other.is_active) {
                    if (!store.setActive(other.id, false)) {
                        reply["error"] = din::obj();
                        reply["error"]["code"] = -1;
                        reply["error"]["message"] = "Failed to deactivate descriptor ID: " + std::to_string(other.id);
                        return reply;
                    }
                    deactivated_ids.append(Json::Int64(other.id));
                }
            }
        }

        // Set active status
        if (!store.setActive(id, active)) {
            reply["error"] = din::obj();
            reply["error"]["code"] = -1;
            reply["error"]["message"] = "Failed to set active status";
            return reply;
        }

        // Build success response
        din::Json result = din::obj();
        result["success"] = true;
        result["id"] = Json::Int64(id);
        result["active"] = active;
        result["deactivated"] = deactivated_ids;

        reply["result"] = result;
        return reply;

    } catch (const std::exception& e) {
        reply["error"] = din::obj();
        reply["error"]["code"] = -1;
        reply["error"]["message"] = std::string("Failed to set active descriptor: ") + e.what();
        return reply;
    }
}

} // namespace rpc
} // namespace din

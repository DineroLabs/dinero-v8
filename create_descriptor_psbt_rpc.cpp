// SPDX-License-Identifier: MIT
// Dinero - wallet.createpsbtfromdescriptor RPC Handler (Phase 2 Step 3)

#include "wallet/descriptor_psbt.h"
#include "wallet/descriptor_checksum.h"
#include "wallet/wallet_manager.h"
#include "wallet/descriptor_activation.h"
#include "common/logger.h"
#include "common/address_script_builder.h"
#include <json/json.h>

namespace dinero {
namespace rpc {

/**
 * @brief RPC: wallet.createpsbtfromdescriptor
 *
 * Create a PSBT from an active descriptor with policy enforcement.
 *
 * Parameters:
 * {
 *   "descriptor": "tr([fp/86h/1447h/0h]xpub/0/*)#checksum",
 *   "inputs": [{"txid": "...", "vout": 0, "amount": 50000, "scriptPubKey": "...", "address_index": 0}],
 *   "outputs": [{"address": "din1...", "amount": 45000}],
 *   "locktime": 0,  // optional
 *   "sighashtype": 1 // optional (SIGHASH_ALL)
 * }
 *
 * Returns:
 * {
 *   "psbt": "cHNidP8BA...",  // Base64-encoded PSBT
 *   "complete": false,
 *   "descriptor_type": "tr",
 *   "wallet_policy": "bip86",
 *   "signing_capability": "external",
 *   "fee": 5000,
 *   "inputs": 1,
 *   "outputs": 1
 * }
 */
Json::Value wallet_createpsbtfromdescriptor(
    const Json::Value& params,
    WalletManager* wallet_manager
) {
    Json::Value result;

    try {
        // Validate wallet is loaded
        if (!wallet_manager || !wallet_manager->hasActiveWallet()) {
            Json::Value error;
            error["code"] = -1;
            error["message"] = "No wallet loaded";
            result["error"] = error;
            return result;
        }

        // Validate required parameters
        if (!params.isMember("descriptor") || !params["descriptor"].isString()) {
            Json::Value error;
            error["code"] = -8;
            error["message"] = "Missing descriptor parameter";
            result["error"] = error;
            return result;
        }

        if (!params.isMember("inputs") || !params["inputs"].isArray()) {
            Json::Value error;
            error["code"] = -8;
            error["message"] = "Missing inputs array";
            result["error"] = error;
            return result;
        }

        if (!params.isMember("outputs") || !params["outputs"].isArray()) {
            Json::Value error;
            error["code"] = -8;
            error["message"] = "Missing outputs array";
            result["error"] = error;
            return result;
        }

        // Parse request
        DescriptorPsbtRequest request;
        request.descriptor = params["descriptor"].asString();

        // Parse inputs
        for (const auto& input_json : params["inputs"]) {
            PsbtInputInfo input;
            input.txid = input_json["txid"].asString();
            input.vout = input_json["vout"].asUInt();
            input.amount = input_json["amount"].asUInt64();

            // Parse scriptPubKey hex
            std::string spk_hex = input_json["scriptPubKey"].asString();
            input.scriptPubKey = DescriptorPsbtFactory::hexToBytes(spk_hex);

            if (input_json.isMember("sequence")) {
                input.sequence = input_json["sequence"].asUInt();
            }

            if (input_json.isMember("address_index")) {
                input.address_index = input_json["address_index"].asUInt();
            }

            request.inputs.push_back(input);
        }

        // Parse outputs
        for (const auto& output_json : params["outputs"]) {
            PsbtOutputInfo output;
            output.address = output_json["address"].asString();
            output.amount = output_json["amount"].asUInt64();
            request.outputs.push_back(output);
        }

        // Optional parameters
        if (params.isMember("locktime")) {
            request.locktime = params["locktime"].asUInt();
        }

        if (params.isMember("sighashtype")) {
            request.sighash_type = params["sighashtype"].asUInt();
        }

        // Get wallet policy and fingerprint from wallet
        auto hd_wallet = wallet_manager->getHDWallet();
        if (!hd_wallet) {
            Json::Value error;
            error["code"] = -1;
            error["message"] = "HD wallet not initialized";
            result["error"] = error;
            return result;
        }

        std::string wallet_fingerprint = hd_wallet->GetMasterFingerprintHex();

        // TODO: Get wallet policy from database
        // For now, assume BIP86 based on descriptor type
        WalletPolicy wallet_policy = WalletPolicy::BIP86_TAPROOT;
        std::string clean_desc = din::DescriptorChecksum::StripChecksum(request.descriptor);
        if (clean_desc.substr(0, 5) == "wpkh(") {
            wallet_policy = WalletPolicy::BIP84_LEGACY;
        }

        // Create PSBT from descriptor
        DescriptorPsbtResult psbt_result = DescriptorPsbtFactory::createPsbtFromDescriptor(
            request,
            wallet_policy,
            wallet_fingerprint
        );

        if (!psbt_result.success) {
            Json::Value error;
            error["code"] = -5;
            error["message"] = psbt_result.error;
            result["error"] = error;
            return result;
        }

        // Build success response
        Json::Value response;
        response["psbt"] = psbt_result.psbt_base64;
        response["complete"] = false;  // Always false - needs signing
        response["descriptor_type"] = psbt_result.descriptor_type;
        response["wallet_policy"] = (wallet_policy == WalletPolicy::BIP86_TAPROOT) ? "bip86" : "bip84";
        response["fee"] = Json::UInt64(psbt_result.fee);
        response["inputs"] = Json::UInt64(psbt_result.input_count);
        response["outputs"] = Json::UInt64(psbt_result.output_count);

        result["result"] = response;
        g_logger.info("Created PSBT from descriptor: " + psbt_result.descriptor_type +
                     ", " + std::to_string(psbt_result.input_count) + " inputs, " +
                     std::to_string(psbt_result.output_count) + " outputs");

    } catch (const std::exception& e) {
        Json::Value error;
        error["code"] = -1;
        error["message"] = std::string("Failed to create PSBT: ") + e.what();
        result["error"] = error;
        g_logger.error("wallet.createpsbtfromdescriptor error: " + std::string(e.what()));
    }

    return result;
}

} // namespace rpc
} // namespace dinero

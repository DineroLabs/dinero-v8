#include "din_json.h"
#include "rpc/rpc_registry.h"

#include "consensus/chainparams.h"
#include "consensus/covenant_activation.h"
#include "address/addr_codec.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include "primitives/transaction.h"
#include "wallet/covenant_profile.h"
#include "wallet/transaction_builder.h"
#include "external/bech32/bech32.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

din::Json rpc_context_wallet_sendtoaddress(
    const ExecutionContext& context,
    const din::Json& params);

namespace {

using dinero::wallet::covenant::CCVPlan;
using dinero::wallet::covenant::CCVAuthorization;
using dinero::wallet::covenant::CTVPlan;
using dinero::wallet::covenant::Output;
using dinero::wallet::covenant::ProfileType;
using dinero::wallet::covenant::TaprootArtifact;

std::string Hex(const uint8_t* data, size_t size) {
    static constexpr char table[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (size_t index = 0; index < size; ++index) {
        result[index * 2] = table[data[index] >> 4];
        result[index * 2 + 1] = table[data[index] & 0x0f];
    }
    return result;
}

std::string Hex(const std::vector<uint8_t>& data) {
    return Hex(data.data(), data.size());
}

template <size_t N>
std::string Hex(const std::array<uint8_t, N>& data) {
    return Hex(data.data(), data.size());
}

uint8_t Nibble(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<uint8_t>(value - 'A' + 10);
    }
    throw std::invalid_argument("invalid hex character");
}

std::vector<uint8_t> ParseHex(
    const std::string& value,
    const char* field,
    bool allowEmpty = false) {
    if ((value.size() & 1U) != 0) {
        throw std::invalid_argument(
            std::string(field) + " must contain an even number of hex digits");
    }
    if (!allowEmpty && value.empty()) {
        throw std::invalid_argument(
            std::string(field) + " must not be empty");
    }
    std::vector<uint8_t> result;
    result.reserve(value.size() / 2);
    for (size_t index = 0; index < value.size(); index += 2) {
        result.push_back(static_cast<uint8_t>(
            (Nibble(value[index]) << 4) | Nibble(value[index + 1])));
    }
    return result;
}

uint32_t UInt32(
    const din::Json& object,
    const char* field,
    uint32_t fallback) {
    if (!object.isMember(field)) {
        return fallback;
    }
    if (!object[field].isUInt() && !object[field].isUInt64()) {
        throw std::invalid_argument(
            std::string(field) + " must be an unsigned integer");
    }
    const Json::UInt64 value = object[field].asUInt64();
    if (value > UINT32_MAX) {
        throw std::invalid_argument(
            std::string(field) + " exceeds uint32");
    }
    return static_cast<uint32_t>(value);
}

uint64_t UInt64(
    const din::Json& object,
    const char* field) {
    if (!object.isMember(field) ||
        (!object[field].isUInt() && !object[field].isUInt64())) {
        throw std::invalid_argument(
            std::string(field) + " must be an unsigned integer");
    }
    return object[field].asUInt64();
}

std::vector<uint8_t> ParseOutputScript(const din::Json& output) {
    if (!output.isObject()) {
        throw std::invalid_argument("output must be an object");
    }
    if (output.isMember("script_pubkey")) {
        return ParseHex(
            output["script_pubkey"].asString(), "script_pubkey");
    }
    if (output.isMember("address")) {
        const auto script =
            dinero::TransactionBuilder::AddressToScriptPubKey(
                output["address"].asString());
        if (script.empty()) {
            throw std::invalid_argument("invalid output address");
        }
        return script;
    }
    throw std::invalid_argument(
        "output requires script_pubkey or address");
}

std::vector<Output> ParseOutputs(
    const din::Json& value,
    bool allowEmpty = false) {
    if (!value.isArray() || (!allowEmpty && value.empty())) {
        throw std::invalid_argument(
            allowEmpty
                ? "outputs must be an array"
                : "outputs must be a non-empty array");
    }
    std::vector<Output> result;
    result.reserve(value.size());
    for (const auto& item : value) {
        result.push_back(Output{
            dinero::AmountUna::Una(UInt64(item, "value_una")),
            ParseOutputScript(item)});
    }
    return result;
}

dinero::TxOutPoint ParseOutpoint(const din::Json& value) {
    if (!value.isObject() ||
        !value.isMember("txid") ||
        !value["txid"].isString()) {
        throw std::invalid_argument(
            "prevout requires txid and vout");
    }
    dinero::uint256 txid;
    if (!dinero::uint256::FromHex(value["txid"].asString(), txid)) {
        throw std::invalid_argument("invalid prevout txid");
    }
    return dinero::TxOutPoint{
        dinero::TxId(txid),
        UInt32(value, "vout", 0)};
}

din::Json TaprootJson(const TaprootArtifact& artifact) {
    din::Json result(Json::objectValue);
    result["internal_key"] = Hex(artifact.internalKey);
    result["merkle_root"] = Hex(artifact.merkleRoot);
    result["output_key_parity"] =
        static_cast<Json::UInt>(artifact.outputKeyParity);
    result["tapscript"] = Hex(artifact.tapscript);
    result["control_block"] = Hex(artifact.controlBlock);
    result["script_pubkey"] = Hex(artifact.scriptPubKey);
    if (artifact.scriptPubKey.size() == 34 &&
        artifact.scriptPubKey[0] == 0x51 &&
        artifact.scriptPubKey[1] == 0x20) {
        const std::vector<uint8_t> program(
            artifact.scriptPubKey.begin() + 2,
            artifact.scriptPubKey.end());
        result["address"] = bech32::Encode(
            dinero::HrpForActiveNetworkRef(),
            1,
            program,
            bech32::Encoding::BECH32M);
    }
    return result;
}

din::Json CtvJson(const CTVPlan& plan) {
    din::Json result(Json::objectValue);
    result["profile"] = "ctv";
    result["descriptor_id"] = plan.descriptorId;
    result["recovery_descriptor"] = plan.recoveryDescriptor;
    result["template_hash"] = Hex(plan.templateHash);
    result["covenant_input_index"] =
        static_cast<Json::UInt>(plan.covenantInputIndex);
    result["taproot"] = TaprootJson(plan.taproot);
    return result;
}

din::Json CcvJson(const CCVPlan& plan) {
    din::Json result(Json::objectValue);
    const bool ownerAuthorized =
        plan.authorization == CCVAuthorization::OwnerSchnorr;
    result["profile"] = ownerAuthorized ? "ccv-owner" : "ccv";
    result["descriptor_id"] = plan.descriptorId;
    result["recovery_descriptor"] = plan.recoveryDescriptor;
    result["state_hash"] = Hex(plan.state.stateHash);
    result["code_hash"] = Hex(plan.state.codeHash);
    result["counter"] = static_cast<Json::UInt>(plan.state.counter);
    result["data_hex"] = Hex(plan.state.data);
    result["authorization"] =
        ownerAuthorized ? "bip340-owner" : "none";
    result["permissionless"] = !ownerAuthorized;
    if (ownerAuthorized) {
        result["owner_public_key"] = Hex(plan.ownerPublicKey);
        result["owner_key_origin"] = plan.ownerKeyOrigin;
    }
    result["taproot"] = TaprootJson(plan.taproot);
    return result;
}

void RequireScheduledProfileNetwork() {
    if (!dinero::IsChainSelected()) {
        throw std::runtime_error("covenant wallet RPC requires a selected network");
    }
    const auto& params = dinero::Params();
    const bool ctvScheduled = params.ctv_activation_height != UINT32_MAX;
    const bool ccvScheduled = params.ccv_activation_height != UINT32_MAX;
    if (!ctvScheduled || !ccvScheduled ||
        params.ctv_activation_height != params.ccv_activation_height) {
        throw std::runtime_error(
            "covenant wallet RPC is unavailable because profile v1 is not "
            "atomically scheduled on this network");
    }
}

void RequireExplicitPermissionless(const din::Json& params) {
    if (!params.isMember("permissionless") ||
        !params["permissionless"].isBool() ||
        !params["permissionless"].asBool()) {
        throw std::invalid_argument(
            "legacy permissionless CCV requires permissionless=true to "
            "acknowledge that any party may choose the next state");
    }
}

void RequireSpendActivation(
    const ExecutionContext& context,
    ProfileType type) {
    if (context.daemon == nullptr || !context.daemon->chainstate) {
        throw std::runtime_error(
            "chainstate is unavailable for covenant activation check");
    }
    const uint32_t tip = context.daemon->chainstate->getBlockHeight();
    if (tip == UINT32_MAX) {
        throw std::runtime_error(
            "active-chain height is unavailable for covenant activation check");
    }
    const uint32_t spendHeight = tip + 1;
    const auto& params = dinero::Params();
    const uint32_t activationHeight =
        type == ProfileType::CTV
            ? params.ctv_activation_height
            : params.ccv_activation_height;
    if (!dinero::consensus::CovenantActivationParams::IsScriptPathActive(
            spendHeight, params) ||
        !dinero::consensus::CovenantActivationParams::IsActive(
            spendHeight, activationHeight)) {
        throw std::runtime_error(
            std::string(type == ProfileType::CTV ? "CTV" : "CCV") +
            " spend construction is disabled at candidate height " +
            std::to_string(spendHeight) +
            "; before activation its opcode does not enforce the intended "
            "covenant");
    }
}

dinero::WalletManager& ActiveWallet(const ExecutionContext& context) {
    if (context.daemon == nullptr || !context.daemon->wallet) {
        throw std::runtime_error("wallet service is unavailable");
    }
    auto walletService =
        std::dynamic_pointer_cast<dinero::WalletService>(
            context.daemon->wallet);
    if (!walletService || !walletService->hasActiveWallet()) {
        throw std::runtime_error("no active wallet");
    }
    return walletService->get();
}

bool Track(
    dinero::WalletManager& wallet,
    const CTVPlan& plan,
    const std::string& label) {
    dinero::CovenantDescriptorRecord record;
    record.descriptor_id = plan.descriptorId;
    record.profile = "ctv";
    record.descriptor = plan.recoveryDescriptor;
    record.script_pubkey = plan.taproot.scriptPubKey;
    record.label = label;
    return wallet.storeCovenantDescriptor(record);
}

bool Track(
    dinero::WalletManager& wallet,
    const CCVPlan& plan,
    const std::string& label,
    const std::string& parent = {}) {
    dinero::CovenantDescriptorRecord record;
    record.descriptor_id = plan.descriptorId;
    record.profile = "ccv";
    record.descriptor = plan.recoveryDescriptor;
    record.script_pubkey = plan.taproot.scriptPubKey;
    record.label = label;
    record.parent_descriptor_id = parent;
    return wallet.storeCovenantDescriptor(record);
}

template <typename Function>
din::Json RpcResult(Function&& function) {
    try {
        RequireScheduledProfileNetwork();
        din::Json result = function();
        result["success"] = true;
        result["rpc_schema"] = "din.wallet.covenant.profile.v1";
        return result;
    } catch (const std::exception& error) {
        din::Json result(Json::objectValue);
        result["success"] = false;
        result["error"] = error.what();
        result["rpc_schema"] = "din.wallet.covenant.profile.v1";
        return result;
    }
}

din::Json RpcCtvCreate(
    const ExecutionContext& context,
    const din::Json& params) {
    return RpcResult([&] {
        if (!params.isObject() ||
            !params.isMember("outputs")) {
            throw std::invalid_argument(
                "wallet.covenant.ctvcreate requires an object with outputs");
        }
        std::vector<uint32_t> sequences;
        if (params.isMember("input_sequences")) {
            if (!params["input_sequences"].isArray() ||
                params["input_sequences"].empty()) {
                throw std::invalid_argument(
                    "input_sequences must be a non-empty array");
            }
            for (const auto& value : params["input_sequences"]) {
                if (!value.isUInt() && !value.isUInt64()) {
                    throw std::invalid_argument(
                        "input sequence must be uint32");
                }
                const Json::UInt64 sequence = value.asUInt64();
                if (sequence > UINT32_MAX) {
                    throw std::invalid_argument(
                        "input sequence exceeds uint32");
                }
                sequences.push_back(static_cast<uint32_t>(sequence));
            }
        } else {
            sequences.push_back(0xfffffffeU);
        }
        const auto plan =
            dinero::wallet::covenant::BuildCTVPlan(
                sequences,
                UInt32(params, "covenant_input_index", 0),
                ParseOutputs(params["outputs"]),
                UInt32(params, "locktime", 0),
                static_cast<int32_t>(UInt32(params, "version", 2)));
        const bool track = params.get("track", false).asBool();
        if (track &&
            !Track(
                ActiveWallet(context),
                plan,
                params.get("label", "").asString())) {
            throw std::runtime_error(
                "failed to persist CTV recovery descriptor");
        }
        din::Json result = CtvJson(plan);
        result["tracked"] = track;
        return result;
    });
}

din::Json RpcCtvFund(
    const ExecutionContext& context,
    const din::Json& params) {
    return RpcResult([&] {
        if (!params.isObject() || !params.isMember("outputs")) {
            throw std::invalid_argument(
                "wallet.covenant.ctvfund requires an object with outputs");
        }
        // A pre-activation CTV leaf executes as an unenforced script. Never
        // let a consumer wallet fund it before the next-block spend rules are
        // active, even though offline descriptor construction remains useful.
        RequireSpendActivation(context, ProfileType::CTV);
        const auto outputs = ParseOutputs(params["outputs"]);
        uint64_t outputTotal = 0;
        for (const auto& output : outputs) {
            const uint64_t value = output.value.GetUna();
            if (UINT64_MAX - outputTotal < value) {
                throw std::invalid_argument("CTV output total overflows uint64");
            }
            outputTotal += value;
        }
        const uint64_t spendFee = params.isMember("spend_fee_una")
            ? UInt64(params, "spend_fee_una")
            : 1000;
        if (spendFee == 0 || UINT64_MAX - outputTotal < spendFee) {
            throw std::invalid_argument(
                "spend_fee_una must be positive and not overflow");
        }

        const std::vector<uint32_t> sequences{
            UInt32(params, "sequence", 0xfffffffeU)};
        const auto plan = dinero::wallet::covenant::BuildCTVPlan(
            sequences,
            0,
            outputs,
            UInt32(params, "locktime", 0),
            2);
        if (!Track(
                ActiveWallet(context),
                plan,
                params.get("label", "Personal contract").asString())) {
            throw std::runtime_error(
                "failed to persist CTV recovery descriptor");
        }

        const auto planJson = CtvJson(plan);
        const std::string address =
            planJson["taproot"]["address"].asString();
        if (address.empty()) {
            throw std::runtime_error(
                "failed to encode covenant funding address");
        }
        din::Json sendParams(Json::objectValue);
        sendParams["address"] = address;
        sendParams["amount"] = static_cast<double>(outputTotal + spendFee) /
            100000000.0;
        if (params.isMember("fee_rate")) {
            sendParams["fee_rate"] = params["fee_rate"];
        }
        const din::Json funding =
            rpc_context_wallet_sendtoaddress(context, sendParams);
        if (funding.isMember("error") &&
            !funding["error"].asString().empty()) {
            throw std::runtime_error(
                "covenant funding failed: " + funding["error"].asString());
        }

        din::Json result = planJson;
        result["tracked"] = true;
        result["funding"] = funding;
        result["funding_value_una"] =
            static_cast<Json::UInt64>(outputTotal + spendFee);
        result["spend_fee_una"] = static_cast<Json::UInt64>(spendFee);
        return result;
    });
}

din::Json RpcCtvSpend(
    const ExecutionContext& context,
    const din::Json& params) {
    return RpcResult([&] {
        if (!params.isObject() ||
            !params.isMember("descriptor") ||
            !params["descriptor"].isString() ||
            !params.isMember("prevouts") ||
            !params["prevouts"].isArray()) {
            throw std::invalid_argument(
                "wallet.covenant.ctvspend requires descriptor and prevouts");
        }
        const auto plan =
            dinero::wallet::covenant::RecoverCTVPlan(
                params["descriptor"].asString());
        RequireSpendActivation(context, ProfileType::CTV);
        std::vector<dinero::TxOutPoint> prevouts;
        prevouts.reserve(params["prevouts"].size());
        for (const auto& item : params["prevouts"]) {
            prevouts.push_back(ParseOutpoint(item));
        }
        const auto tx =
            dinero::wallet::covenant::BuildCTVSpend(plan, prevouts);
        din::Json result = CtvJson(plan);
        result["hex"] = tx.SerializeHex(
            dinero::TxSerializationMode::WithWitness);
        result["txid"] = tx.GetTxid().AsUint256().GetHex();
        result["wtxid"] = tx.GetWtxid().AsUint256().GetHex();
        return result;
    });
}

din::Json RpcCcvCreate(
    const ExecutionContext& context,
    const din::Json& params) {
    return RpcResult([&] {
        if (!params.isObject()) {
            throw std::invalid_argument(
                "wallet.covenant.ccvcreate requires an object");
        }
        const auto data = ParseHex(
            params.get("data_hex", "").asString(),
            "data_hex",
            true);
        CCVPlan plan;
        const bool permissionless =
            params.isMember("permissionless") &&
            params["permissionless"].isBool() &&
            params["permissionless"].asBool();
        if (permissionless) {
            RequireExplicitPermissionless(params);
            plan = dinero::wallet::covenant::BuildCCVPlan(
                UInt32(params, "counter", 0), data);
        } else {
            auto& wallet = ActiveWallet(context);
            const std::string ownerAddress = wallet.getNewAddress(
                params.get("label", "ccv-owner").asString() +
                    " owner key",
                "taproot");
            if (ownerAddress.empty()) {
                throw std::runtime_error(
                    "failed to allocate a wallet key for CCV ownership");
            }
            const auto ownerScript =
                wallet.getScriptPubKeyForAddress(ownerAddress);
            if (!ownerScript.has_value()) {
                throw std::runtime_error(
                    "wallet did not persist the CCV owner key locator");
            }
            const auto ownerOrigin = wallet.getDerivationPath(*ownerScript);
            const auto ownerPrivateKey =
                wallet.deriveKeyForScriptPubKey(*ownerScript);
            if (!ownerOrigin.has_value() ||
                !ownerPrivateKey.has_value()) {
                throw std::runtime_error(
                    "wallet cannot recover the allocated CCV owner key");
            }
            plan = dinero::wallet::covenant::BuildOwnerAuthorizedCCVPlan(
                UInt32(params, "counter", 0),
                data,
                dinero::wallet::covenant::OwnerXOnlyPublicKey(
                    *ownerPrivateKey),
                *ownerOrigin);
        }
        const bool track = params.get("track", false).asBool();
        if (track &&
            !Track(
                ActiveWallet(context),
                plan,
                params.get("label", "").asString())) {
            throw std::runtime_error(
                "failed to persist CCV recovery descriptor");
        }
        din::Json result = CcvJson(plan);
        result["tracked"] = track;
        return result;
    });
}

din::Json RpcCcvAdvance(
    const ExecutionContext& context,
    const din::Json& params) {
    return RpcResult([&] {
        if (!params.isObject() ||
            !params.isMember("descriptor") ||
            !params["descriptor"].isString() ||
            !params.isMember("inputs") ||
            !params["inputs"].isArray() ||
            params["inputs"].empty()) {
            throw std::invalid_argument(
                "wallet.covenant.ccvadvance requires descriptor and inputs");
        }
        const auto current =
            dinero::wallet::covenant::RecoverCCVPlan(
                params["descriptor"].asString());
        if (current.authorization == CCVAuthorization::Permissionless) {
            RequireExplicitPermissionless(params);
        }
        RequireSpendActivation(
            context,
            current.authorization == CCVAuthorization::OwnerSchnorr
                ? ProfileType::CCV_OWNER
                : ProfileType::CCV);
        std::vector<dinero::wallet::covenant::Input> inputs;
        inputs.reserve(params["inputs"].size());
        for (const auto& item : params["inputs"]) {
            inputs.push_back({
                ParseOutpoint(item),
                UInt32(item, "sequence", 0xfffffffeU)});
        }
        std::vector<Output> outputs;
        if (params.isMember("outputs")) {
            outputs = ParseOutputs(params["outputs"], true);
        }
        const auto covenantValue = dinero::AmountUna::Una(
            UInt64(params, "covenant_value_una"));
        const auto nextData = ParseHex(
            params.get("next_data_hex", "").asString(),
            "next_data_hex",
            true);
        dinero::wallet::covenant::CCVTransition transition;
        if (current.authorization == CCVAuthorization::OwnerSchnorr) {
            std::vector<dinero::wallet::covenant::Prevout> prevouts;
            prevouts.reserve(inputs.size());
            prevouts.push_back({
                covenantValue,
                current.taproot.scriptPubKey});
            for (Json::ArrayIndex index = 1;
                 index < params["inputs"].size();
                 ++index) {
                const auto& item = params["inputs"][index];
                if (!item.isMember("prevout_value_una")) {
                    throw std::invalid_argument(
                        "owner CCV fee inputs require prevout_value_una");
                }
                prevouts.push_back({
                    dinero::AmountUna::Una(
                        UInt64(item, "prevout_value_una")),
                    ParseOutputScript(item)});
            }
            const std::string ownerPrivateKeyHex =
                ActiveWallet(context).getPrivateKeyForPath(
                    current.ownerKeyOrigin);
            if (ownerPrivateKeyHex.empty()) {
                throw std::runtime_error(
                    "wallet is locked or cannot recover the CCV owner key");
            }
            transition = dinero::wallet::covenant::
                BuildOwnerAuthorizedCCVTransition(
                    current,
                    inputs,
                    prevouts,
                    covenantValue,
                    nextData,
                    ParseHex(
                        ownerPrivateKeyHex,
                        "wallet CCV owner private key"),
                    outputs,
                    UInt32(params, "locktime", 0),
                    static_cast<int32_t>(
                        UInt32(params, "version", 2)));
        } else {
            transition = dinero::wallet::covenant::BuildCCVTransition(
                current,
                inputs,
                covenantValue,
                nextData,
                outputs,
                UInt32(params, "locktime", 0),
                static_cast<int32_t>(
                    UInt32(params, "version", 2)));
        }
        const bool track =
            params.get("track_successor", false).asBool();
        if (track &&
            !Track(
                ActiveWallet(context),
                transition.successor,
                params.get("label", "").asString(),
                current.descriptorId)) {
            throw std::runtime_error(
                "failed to persist CCV successor descriptor");
        }
        din::Json result(Json::objectValue);
        result["current_descriptor_id"] = current.descriptorId;
        result["successor"] = CcvJson(transition.successor);
        result["successor_tracked"] = track;
        result["hex"] = transition.tx.SerializeHex(
            dinero::TxSerializationMode::WithWitness);
        result["txid"] =
            transition.tx.GetTxid().AsUint256().GetHex();
        result["wtxid"] =
            transition.tx.GetWtxid().AsUint256().GetHex();
        return result;
    });
}

din::Json RpcImport(
    const ExecutionContext& context,
    const din::Json& params) {
    return RpcResult([&] {
        if (!params.isObject() ||
            !params.isMember("descriptor") ||
            !params["descriptor"].isString()) {
            throw std::invalid_argument(
                "wallet.covenant.import requires descriptor");
        }
        const std::string descriptor =
            params["descriptor"].asString();
        auto& wallet = ActiveWallet(context);
        din::Json result;
        switch (dinero::wallet::covenant::DescriptorType(descriptor)) {
            case ProfileType::CTV: {
                const auto plan =
                    dinero::wallet::covenant::RecoverCTVPlan(descriptor);
                if (!Track(
                        wallet,
                        plan,
                        params.get("label", "").asString())) {
                    throw std::runtime_error(
                        "failed to import CTV descriptor");
                }
                result = CtvJson(plan);
                break;
            }
            case ProfileType::CCV:
            case ProfileType::CCV_OWNER: {
                const auto plan =
                    dinero::wallet::covenant::RecoverCCVPlan(descriptor);
                if (!Track(
                        wallet,
                        plan,
                        params.get("label", "").asString(),
                        params.get(
                            "parent_descriptor_id", "").asString())) {
                    throw std::runtime_error(
                        "failed to import CCV descriptor");
                }
                result = CcvJson(plan);
                break;
            }
        }
        result["tracked"] = true;
        return result;
    });
}

din::Json RpcInspect(
    const ExecutionContext&,
    const din::Json& params) {
    return RpcResult([&] {
        if (!params.isObject() ||
            !params.isMember("descriptor") ||
            !params["descriptor"].isString()) {
            throw std::invalid_argument(
                "wallet.covenant.inspect requires descriptor");
        }
        const std::string descriptor =
            params["descriptor"].asString();
        if (dinero::wallet::covenant::DescriptorType(descriptor) ==
            ProfileType::CTV) {
            return CtvJson(
                dinero::wallet::covenant::RecoverCTVPlan(descriptor));
        }
        return CcvJson(
            dinero::wallet::covenant::RecoverCCVPlan(descriptor));
    });
}

din::Json RpcList(
    const ExecutionContext& context,
    const din::Json&) {
    return RpcResult([&] {
        din::Json result(Json::objectValue);
        din::Json records(Json::arrayValue);
        for (const auto& record :
             ActiveWallet(context).listCovenantDescriptors()) {
            din::Json item(Json::objectValue);
            item["descriptor_id"] = record.descriptor_id;
            if (record.profile == "ccv") {
                const auto plan = dinero::wallet::covenant::RecoverCCVPlan(
                    record.descriptor);
                const bool ownerAuthorized =
                    plan.authorization == CCVAuthorization::OwnerSchnorr;
                item["profile"] = ownerAuthorized ? "ccv-owner" : "ccv";
                item["authorization"] =
                    ownerAuthorized ? "bip340-owner" : "none";
                item["permissionless"] = !ownerAuthorized;
                if (ownerAuthorized) {
                    item["owner_public_key"] = Hex(plan.ownerPublicKey);
                    item["owner_key_origin"] = plan.ownerKeyOrigin;
                }
            } else {
                item["profile"] = record.profile;
            }
            item["recovery_descriptor"] = record.descriptor;
            item["script_pubkey"] = Hex(record.script_pubkey);
            item["label"] = record.label;
            item["parent_descriptor_id"] =
                record.parent_descriptor_id;
            item["created_at"] =
                static_cast<Json::Int64>(record.created_at);
            records.append(item);
        }
        result["descriptors"] = records;
        result["count"] = static_cast<Json::UInt64>(records.size());
        return result;
    });
}

} // namespace

void register_context_wallet_covenant_profile_methods() {
    g_rpcRegistry.registerHandler(
        "wallet.covenant.ctvcreate",
        RpcCtvCreate,
        RegisterMode::Overwrite,
        "covenant-profile-v1");
    g_rpcRegistry.registerHandler(
        "wallet.covenant.ctvfund",
        RpcCtvFund,
        RegisterMode::Overwrite,
        "covenant-profile-v1");
    g_rpcRegistry.registerHandler(
        "wallet.covenant.ctvspend",
        RpcCtvSpend,
        RegisterMode::Overwrite,
        "covenant-profile-v1");
    g_rpcRegistry.registerHandler(
        "wallet.covenant.ccvcreate",
        RpcCcvCreate,
        RegisterMode::Overwrite,
        "covenant-profile-v1");
    g_rpcRegistry.registerHandler(
        "wallet.covenant.ccvadvance",
        RpcCcvAdvance,
        RegisterMode::Overwrite,
        "covenant-profile-v1");
    g_rpcRegistry.registerHandler(
        "wallet.covenant.import",
        RpcImport,
        RegisterMode::Overwrite,
        "covenant-profile-v1");
    g_rpcRegistry.registerHandler(
        "wallet.covenant.inspect",
        RpcInspect,
        RegisterMode::Overwrite,
        "covenant-profile-v1");
    g_rpcRegistry.registerHandler(
        "wallet.covenant.list",
        RpcList,
        RegisterMode::Overwrite,
        "covenant-profile-v1");
}

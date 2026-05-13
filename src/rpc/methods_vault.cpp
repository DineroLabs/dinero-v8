// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault RPC handlers.

#include "rpc/methods_vault.h"

#include "address/addr_codec.h"
#include "common/logger.h"
#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "vault/ledger_entry.h"
#include "vault/vault_runtime.h"
#include "vault/vault_service.h"
#include "vault/withdrawal_queue.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

extern RpcRegistry g_rpcRegistry;

namespace din {
namespace {

dinero::vault::VaultService* g_vault_service = nullptr;

std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (uint8_t byte : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

std::string arrayToHex(const std::array<uint8_t, 32>& bytes) {
    std::ostringstream oss;
    for (uint8_t byte : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

bool hexToBytes32(const std::string& hex, std::array<uint8_t, 32>& out) {
    if (hex.size() != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        std::string byte_str = hex.substr(i * 2, 2);
        try {
            out[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        } catch (...) {
            return false;
        }
    }
    return true;
}

bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        try {
            out.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
        } catch (...) {
            return false;
        }
    }
    return true;
}

bool hexToBytes16(const std::string& hex, std::array<uint8_t, 16>& out) {
    if (hex.size() != 32) {
        return false;
    }
    for (size_t i = 0; i < 16; ++i) {
        std::string byte_str = hex.substr(i * 2, 2);
        try {
            out[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        } catch (...) {
            return false;
        }
    }
    return true;
}

std::string arrayToHex16(const std::array<uint8_t, 16>& bytes) {
    std::ostringstream oss;
    for (uint8_t byte : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

Json errorObj(const std::string& msg, int code = -1) {
    Json result;
    result["error"]["code"] = code;
    result["error"]["message"] = msg;
    return result;
}

dinero::vault::VaultService* requireService() {
    return g_vault_service;
}

}  // namespace

dinero::vault::VaultService* GetVaultService() { return g_vault_service; }
void SetVaultService(dinero::vault::VaultService* service) { g_vault_service = service; }

Json rpc_vault_account_spendable(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    auto* svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    if (params.size() < 1 || !params[0].isString()) {
        return errorObj("missing required parameter: account_id");
    }
    dinero::vault::AccountId account{params[0].asString()};
    result["account_id"] = account.raw;
    result["spendable_una"] = static_cast<Json::UInt64>(svc->accountSpendable(account));
    return result;
}

Json rpc_vault_account_metrics(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    auto* svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    if (params.size() < 1 || !params[0].isString()) {
        return errorObj("missing required parameter: account_id");
    }
    dinero::vault::AccountId account{params[0].asString()};
    result["account_id"] = account.raw;
    result["spendable_una"] = static_cast<Json::UInt64>(svc->accountSpendable(account));
    result["confirmed_una"] = static_cast<Json::UInt64>(svc->accountConfirmed(account));
    result["pending_una"] = static_cast<Json::UInt64>(svc->accountPending(account));
    result["locked_una"] = static_cast<Json::UInt64>(svc->accountLocked(account));
    result["operator_loss_una"] = static_cast<Json::UInt64>(svc->accountOperatorLoss(account));
    return result;
}

Json rpc_vault_observe(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    auto* svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    // Params: { txid (display hex), vout, account_id, amount_una, height, block_hash (display hex) }
    if (params.size() < 1 || !params[0].isObject()) {
        return errorObj("missing parameter object");
    }
    const Json& obj = params[0];
    std::string txid_hex = obj["txid"].asString();
    std::array<uint8_t, 32> txid{};
    if (!hexToBytes32(txid_hex, txid)) {
        return errorObj("invalid txid hex");
    }
    // Display-hex → raw byte order: reverse.
    std::array<uint8_t, 32> txid_raw{};
    for (size_t i = 0; i < 32; ++i) {
        txid_raw[i] = txid[31 - i];
    }
    auto vout = static_cast<uint32_t>(obj["vout"].asUInt());
    dinero::vault::AccountId account{obj["account_id"].asString()};
    auto amount = static_cast<dinero::vault::UnaAmount>(obj["amount_una"].asUInt64());
    auto height = static_cast<uint64_t>(obj["height"].asUInt64());
    std::string bh_hex = obj["block_hash"].asString();
    std::array<uint8_t, 32> block_hash_display{};
    if (!hexToBytes32(bh_hex, block_hash_display)) {
        return errorObj("invalid block_hash hex");
    }
    std::array<uint8_t, 32> block_hash{};
    for (size_t i = 0; i < 32; ++i) {
        block_hash[i] = block_hash_display[31 - i];
    }
    svc->recordDeposit(txid_raw, vout, account, amount, height, block_hash);
    result["status"] = "observed";
    return result;
}

Json rpc_vault_withdraw(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    auto* svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    if (params.size() < 1 || !params[0].isObject()) {
        return errorObj("missing parameter object");
    }
    const Json& obj = params[0];
    dinero::vault::AccountId account{obj["account_id"].asString()};
    auto amount = static_cast<dinero::vault::UnaAmount>(obj["amount_una"].asUInt64());
    std::vector<uint8_t> spk;
    // Accept either `destination_address` (a bech32m din1p…) or
    // `destination_script_pub_key` (raw hex). Address path is the
    // operator-friendly form; the script path is the legacy /
    // machine-friendly form. Address takes precedence if both are set.
    if (obj.isMember("destination_address") && obj["destination_address"].isString()) {
        try {
            std::vector<uint8_t> witness_program =
                dinero::DecodeTaprootWitnessProgram(obj["destination_address"].asString());
            spk = dinero::CreateP2TRScriptPubKey(witness_program);
        } catch (const std::exception& e) {
            return errorObj(std::string("invalid destination_address: ") + e.what());
        }
    } else {
        std::string spk_hex = obj["destination_script_pub_key"].asString();
        if (!hexToBytes(spk_hex, spk)) {
            return errorObj("invalid destination_script_pub_key hex");
        }
    }
    try {
        dinero::vault::WithdrawalId id = svc->enqueueWithdrawal(account, amount, spk);
        result["request_id"] = arrayToHex16(id);
        result["status"] = "pending";
    } catch (const dinero::vault::WithdrawalQueueError& e) {
        return errorObj(std::string("withdrawal_queue: ") + e.what());
    }
    return result;
}

Json rpc_vault_processnext(const ExecutionContext& /*ctx*/, const Json& /*params*/) {
    Json result;
    auto* svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    try {
        auto id = svc->processNextWithdrawal();
        if (!id.has_value()) {
            result["status"] = "queue_empty";
            return result;
        }
        result["request_id"] = arrayToHex16(id.value());
        result["status"] = "advanced";
    } catch (const std::exception& e) {
        return errorObj(std::string("processNext: ") + e.what());
    }
    return result;
}

Json rpc_vault_withdrawal_status(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    auto* svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    if (params.size() < 1 || !params[0].isString()) {
        return errorObj("missing request_id");
    }
    std::array<uint8_t, 16> id_arr{};
    if (!hexToBytes16(params[0].asString(), id_arr)) {
        return errorObj("invalid request_id hex");
    }
    auto state = svc->withdrawalState(id_arr);
    result["request_id"] = params[0].asString();
    if (std::holds_alternative<dinero::vault::WithdrawalPending>(state)) {
        result["state"] = "pending";
    } else if (std::holds_alternative<dinero::vault::WithdrawalSigning>(state)) {
        result["state"] = "signing";
    } else if (auto* bc = std::get_if<dinero::vault::WithdrawalBroadcast>(&state); bc != nullptr) {
        result["state"] = "broadcast";
        result["txid"] = arrayToHex(bc->txid);
        result["included_at_height"] = static_cast<Json::UInt64>(bc->included_at_height);
    } else if (auto* settled = std::get_if<dinero::vault::WithdrawalSettledOnChain>(&state); settled != nullptr) {
        result["state"] = "settled";
        result["txid"] = arrayToHex(settled->txid);
    } else if (auto* reverted = std::get_if<dinero::vault::WithdrawalRevertedOnChain>(&state);
               reverted != nullptr) {
        result["state"] = "reverted";
        result["txid"] = arrayToHex(reverted->txid);
    } else if (auto* failed = std::get_if<dinero::vault::WithdrawalFailed>(&state); failed != nullptr) {
        result["state"] = "failed";
        result["reason"] = failed->reason;
    }
    return result;
}

Json rpc_vault_setoperator(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    if (params.size() < 1 || !params[0].isObject()) {
        return errorObj("missing parameter object");
    }
    const Json& obj = params[0];
    std::string address = obj.isMember("address") && obj["address"].isString()
                              ? obj["address"].asString()
                              : "";
    std::string account = obj.isMember("account") && obj["account"].isString()
                              ? obj["account"].asString()
                              : "";
    std::string err;
    if (!dinero::vault::SetVaultOperator(address, account, &err)) {
        return errorObj(err);
    }
    auto bound = dinero::vault::GetVaultOperator();
    result["address"] = bound.address;
    result["account"] = bound.account;
    result["status"] = address.empty() ? "disabled" : "bound";
    return result;
}

Json rpc_vault_getoperator(const ExecutionContext& /*ctx*/, const Json& /*params*/) {
    Json result;
    auto bound = dinero::vault::GetVaultOperator();
    result["address"] = bound.address;
    result["account"] = bound.account;
    result["enabled"] = !bound.address.empty();
    return result;
}

Json rpc_vault_metrics(const ExecutionContext& /*ctx*/, const Json& /*params*/) {
    Json result;
    auto* svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    result["total_open_credits_una"] = static_cast<Json::UInt64>(svc->totalOpenCredits());
    result["total_operator_loss_una"] = static_cast<Json::UInt64>(svc->totalOperatorLoss());
    result["account_count"] = static_cast<Json::UInt64>(svc->accountCount());
    result["ledger_next_seq"] = static_cast<Json::UInt64>(svc->ledgerNextSeq());
    result["withdrawal_queue_depth"] = svc->withdrawalQueueDepth();
    return result;
}

}  // namespace din

void RegisterVaultRPC() {
    dinero::g_logger.info("  Registering Liquidity Vault RPC methods...");
    g_rpcRegistry.registerHandler("vault.account.spendable", din::rpc_vault_account_spendable);
    g_rpcRegistry.registerHandler("vault.account.metrics", din::rpc_vault_account_metrics);
    g_rpcRegistry.registerHandler("vault.observe", din::rpc_vault_observe);
    g_rpcRegistry.registerHandler("vault.withdraw", din::rpc_vault_withdraw);
    g_rpcRegistry.registerHandler("vault.processnext", din::rpc_vault_processnext);
    g_rpcRegistry.registerHandler("vault.withdrawal.status", din::rpc_vault_withdrawal_status);
    g_rpcRegistry.registerHandler("vault.metrics", din::rpc_vault_metrics);
    g_rpcRegistry.registerHandler("vault.setoperator", din::rpc_vault_setoperator);
    g_rpcRegistry.registerHandler("vault.getoperator", din::rpc_vault_getoperator);
    dinero::g_logger.info("  Registered 6 Liquidity Vault RPC methods");
}

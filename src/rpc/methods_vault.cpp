// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault RPC handlers.

#include "rpc/methods_vault.h"

#include "address/addr_codec.h"
#include "consensus/chainparams.h"
#include "primitives/uint256.h"
#include "common/logger.h"
#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "wallet/wallet_transaction_signer.h"
#include "vault/ledger_entry.h"
#include "vault/vault_runtime.h"
#include "vault/vault_service.h"
#include "vault/withdrawal_queue.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

extern RpcRegistry g_rpcRegistry;

namespace din {
namespace {

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

// Decode every nibble before returning bytes. No numeric coercion or partial
// token parsing at the RPC boundary; caller destinations remain untouched on
// refusal. Uppercase and lowercase are both accepted.
bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    const auto nibble=[](char c)->int {
        if(c>='0' && c<='9')return c-'0';
        if(c>='a' && c<='f')return c-'a'+10;
        if(c>='A' && c<='F')return c-'A'+10;
        return -1;
    };
    std::vector<uint8_t> candidate;candidate.reserve(hex.size()/2);
    for(size_t i=0;i<hex.size();i+=2) {
        const int hi=nibble(hex[i]),lo=nibble(hex[i+1]);
        if(hi<0 || lo<0)return false;
        candidate.push_back(static_cast<uint8_t>((hi<<4)|lo));
    }
    out.swap(candidate);return true;
}
template<size_t N>
bool hexToArray(const std::string& hex,std::array<uint8_t,N>& out) {
    if(hex.size()!=N*2)return false;
    std::vector<uint8_t> bytes;
    if(!hexToBytes(hex,bytes))return false;
    std::copy(bytes.begin(),bytes.end(),out.begin());return true;
}
bool hexToBytes32(const std::string& hex,std::array<uint8_t,32>& out) {return hexToArray(hex,out);}
bool hexToBytes16(const std::string& hex,std::array<uint8_t,16>& out) {return hexToArray(hex,out);}
bool unsignedInteger(const Json& value,uint64_t& out) {
    if(value.type()!=::Json::intValue && value.type()!=::Json::uintValue)return false;
    if(value.type()==::Json::intValue && value.asInt64()<0)return false;
    out=value.asUInt64();return true;
}
bool accountString(const Json& value) {
    return value.isString() && !value.asString().empty() &&
           value.asString().find('\0')==std::string::npos;
}
bool oneObject(const Json& params) {return params.isArray() && params.size()==1 && params[0].isObject();}

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

dinero::vault::VaultStateDomain selectedVaultDomain() {
    dinero::vault::VaultStateDomain domain;
    domain.network=static_cast<uint8_t>(dinero::GetActiveChain());dinero::uint256 genesis;
    if(!dinero::uint256::FromHex(dinero::Params().genesis_hash,genesis))
        throw std::runtime_error("vault chain domain unavailable");
    std::copy(genesis.begin(),genesis.end(),domain.genesis.begin());return domain;
}

std::shared_ptr<dinero::vault::VaultService> requireService() {
    return dinero::vault::GetVaultRuntimeService();
}

}  // namespace

std::shared_ptr<dinero::vault::VaultService> GetVaultService() { return requireService(); }

Json rpc_vault_create(const ExecutionContext& ctx,const Json& params) {
    if(!oneObject(params) || params[0].size()!=13)
        return errorObj("creation requires operator_address, account_id, shadow_mode, confirmation policy and all capacity limits");
    if(!ctx.daemon)return errorObj("daemon context unavailable");
    try {
        const auto& obj=params[0];
        if(!obj["operator_address"].isString() || !accountString(obj["account_id"]) ||
           obj["account_id"].asString().size()>1024 || !obj["shadow_mode"].isBool())
            return errorObj("invalid operator, account or shadow policy");
        const auto amount=[&](const char* name) {
            uint64_t value=0;if(!unsignedInteger(obj[name],value) || value==0)
                throw std::runtime_error(std::string(name)+" must be a positive integer");
            return value;
        };
        dinero::vault::VaultServiceConfig config;
        config.shadow_mode=obj["shadow_mode"].asBool();
        config.operator_binding=dinero::vault::VaultOperatorBinding{
            dinero::CreateP2TRScriptPubKey(dinero::DecodeTaprootWitnessProgram(obj["operator_address"].asString())),
            obj["account_id"].asString()};
        auto& confirmations=config.confirmation_policy;
        confirmations.k_observe=amount("k_observe");confirmations.k_credit=amount("k_credit");
        confirmations.k_settle=amount("k_settle");
        config.withdrawal_policy.k_settle=amount("withdrawal_k_settle");
        if(confirmations.k_observe>confirmations.k_credit || confirmations.k_credit>confirmations.k_settle)
            return errorObj("confirmation thresholds must satisfy observe <= credit <= settle");
        config.ledger_caps={amount("per_deposit_cap_una"),amount("per_account_cap_una"),amount("global_cap_una")};
        config.withdrawal_caps.per_request=amount("per_withdrawal_cap_una");
        config.withdrawal_caps.per_account_outstanding=amount("per_account_outstanding_cap_una");
        const auto depth=amount("max_queue_depth");
        if(depth>static_cast<uint64_t>(std::numeric_limits<int>::max()))return errorObj("max_queue_depth is out of range");
        config.withdrawal_caps.global_queue_depth=static_cast<int>(depth);
        if(config.ledger_caps.per_deposit>config.ledger_caps.per_user || config.ledger_caps.per_user>config.ledger_caps.global ||
           config.withdrawal_caps.per_request>config.withdrawal_caps.per_account_outstanding)
            return errorObj("capacity limits are inconsistent");
        auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        auto use=dinero::WalletService::AcquireWalletUse(wallet);
        const auto selected=dinero::CaptureWalletSigningIdentity(use->Wallet(),ctx.walletName);
        auto transaction=dinero::vault::VaultStateTransaction::CreateNewOwned(
            use->Wallet(),selected.session,selectedVaultDomain(),config);
        // Allocate the reply before checked commit. No runtime attachment,
        // source observation, coin selection, signing or submission occurs.
        Json result;result["created"]=true;result["attached"]=false;
        result["vault_id"]=arrayToHex(transaction->Current().identity);result["wallet"]=selected.name;
        transaction->Commit();return result;
    } catch(const std::exception& e) {return errorObj(e.what());}
}

Json rpc_vault_list(const ExecutionContext& ctx,const Json& params) {
    if(!params.isArray() || !params.empty())return errorObj("vault.list takes no parameters");
    if(!ctx.daemon)return errorObj("daemon context unavailable");
    try {
        auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        auto use=dinero::WalletService::AcquireWalletUse(wallet);
        const auto selected=dinero::CaptureWalletSigningIdentity(use->Wallet(),ctx.walletName);
        const auto summaries=dinero::vault::VaultStateTransaction::ListExisting(use->Wallet(),selected.session,selectedVaultDomain());
        Json result;result["wallet"]=selected.name;result["vaults"]=Json(::Json::arrayValue);
        result["historical_completeness_verified"]=false;
        for(const auto& summary:summaries) {
            Json row;row["vault_id"]=arrayToHex(summary.identity);row["revision"]=static_cast<Json::UInt64>(summary.revision);
            row["operator_bound"]=summary.operator_binding.has_value();
            if(summary.operator_binding) {
                row["operator_script_pub_key"]=bytesToHex(summary.operator_binding->script_pub_key);
                row["account_id"]=summary.operator_binding->account;
            }
            result["vaults"].append(std::move(row));
        }
        return result;
    } catch(const std::exception& e) {return errorObj(e.what());}
}

Json rpc_vault_open(const ExecutionContext& ctx,const Json& params) {
    if(!oneObject(params) || params[0].size()!=1 || !params[0]["vault_id"].isString())
        return errorObj("expected [{vault_id: existing 64-character hex identity}]");
    dinero::vault::VaultIdentity identity{};
    if(!hexToBytes32(params[0]["vault_id"].asString(),identity) ||
       std::all_of(identity.begin(),identity.end(),[](uint8_t v){return v==0;}))
        return errorObj("invalid vault identity");
    if(!ctx.daemon)return errorObj("daemon context unavailable");
    try {
        auto wallet=std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        dinero::WalletSigningIdentity selected;
        {
            auto use=dinero::WalletService::AcquireWalletUse(wallet);
            selected=dinero::CaptureWalletSigningIdentity(use->Wallet(),ctx.walletName);
        }
        auto bound_context=ctx;bound_context.walletName=selected.name;
        dinero::vault::VaultRuntimeConfig config;config.enabled=true;
        config.block_hash_at_height=dinero::vault::MakeChainstateBlockHashClosure(*ctx.daemon);
        config.tx_included_at=dinero::vault::MakeChainstateTxIncludedClosure(*ctx.daemon);
        config.capture_tip=dinero::vault::MakeChainstateVaultSnapshotClosure(*ctx.daemon);
        // Prepare response fields before publication. HTTP response delivery
        // remains separate from successful local attachment.
        Json result;result["attached"]=true;result["vault_id"]=arrayToHex(identity);
        result["wallet"]=selected.name;
        dinero::vault::OpenExistingVaultRuntime(std::move(config),bound_context,wallet,selected,identity);
        return result;
    } catch(const std::exception& e) {return errorObj(e.what());}
}

Json rpc_vault_account_spendable(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    auto svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    if (!params.isArray() || params.size() != 1 || !accountString(params[0])) {
        return errorObj("missing required parameter: account_id");
    }
    dinero::vault::AccountId account{params[0].asString()};
    result["account_id"] = account.raw;
    result["spendable_una"] = static_cast<Json::UInt64>(svc->accountSpendable(account));
    return result;
}

Json rpc_vault_account_metrics(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    auto svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    if (!params.isArray() || params.size() != 1 || !accountString(params[0])) {
        return errorObj("missing required parameter: account_id");
    }
    dinero::vault::AccountId account{params[0].asString()};
    const auto metrics = svc->accountMetrics(account);
    result["account_id"] = account.raw;
    result["spendable_una"] = static_cast<Json::UInt64>(metrics.spendable);
    result["confirmed_una"] = static_cast<Json::UInt64>(metrics.confirmed);
    result["pending_una"] = static_cast<Json::UInt64>(metrics.pending);
    result["locked_una"] = static_cast<Json::UInt64>(metrics.locked);
    result["operator_loss_una"] = static_cast<Json::UInt64>(metrics.operator_loss);
    return result;
}

Json rpc_vault_observe(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    auto svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    // Params: { txid (display hex), vout, account_id, amount_una, height, block_hash (display hex) }
    if (!oneObject(params)) {
        return errorObj("missing parameter object");
    }
    const Json& obj = params[0];
    uint64_t vout_value=0;
    if(!obj["txid"].isString() || !accountString(obj["account_id"]) ||
       !unsignedInteger(obj["vout"],vout_value) || vout_value>std::numeric_limits<uint32_t>::max())
        return errorObj("txid/account_id must be strings and vout a uint32 integer");
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
    auto vout = static_cast<uint32_t>(vout_value);
    dinero::vault::AccountId account{obj["account_id"].asString()};

    // SECURITY (F-CRIT-03, 2026-05-29): do NOT trust caller-supplied amount/height/
    // block_hash. Verify the outpoint against chainstate — it must exist in the UTXO
    // set, pay the configured vault operator script, and be transparent — and use the
    // REAL on-chain value/height/hash. This is what makes unbacked credit-minting
    // impossible via this RPC. (The legacy amount_una/height/block_hash request fields
    // are now ignored.)
    uint64_t amount = 0;
    uint64_t height = 0;
    std::array<uint8_t, 32> block_hash{};
    std::string verr;
    if (!dinero::vault::VerifyOperatorDeposit(svc, txid_raw, vout, amount, height, block_hash, verr)) {
        return errorObj(std::string("deposit verification failed: ") + verr);
    }

    svc->recordDeposit(txid_raw, vout, account,
                       static_cast<dinero::vault::UnaAmount>(amount), height, block_hash);
    result["status"] = "observed";
    return result;
}

Json rpc_vault_withdraw(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    auto svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    if (!oneObject(params)) {
        return errorObj("missing parameter object");
    }
    const Json& obj = params[0];
    uint64_t amount=0;
    if(!accountString(obj["account_id"]) || !unsignedInteger(obj["amount_una"],amount) || amount==0)
        return errorObj("account_id must be a nonempty string and amount_una a positive integer");
    dinero::vault::AccountId account{obj["account_id"].asString()};
    std::vector<uint8_t> spk;
    // Accept either `destination_address` (a bech32m din1p…) or
    // `destination_script_pub_key` (raw hex). Address path is the
    // operator-friendly form; the script path is the legacy /
    // machine-friendly form. Address takes precedence if both are set.
    if (obj.isMember("destination_address")) {
        if(!obj["destination_address"].isString())return errorObj("destination_address must be a string");
        try {
            std::vector<uint8_t> witness_program =
                dinero::DecodeTaprootWitnessProgram(obj["destination_address"].asString());
            spk = dinero::CreateP2TRScriptPubKey(witness_program);
        } catch (const std::exception& e) {
            return errorObj(std::string("invalid destination_address: ") + e.what());
        }
    } else {
        if(!obj["destination_script_pub_key"].isString())return errorObj("destination_script_pub_key must be a string");
        std::string spk_hex = obj["destination_script_pub_key"].asString();
        if (!hexToBytes(spk_hex, spk)) {
            return errorObj("invalid destination_script_pub_key hex");
        }
    }
    try {
        std::optional<dinero::vault::WithdrawalPaymentTerms> terms;
        if(obj.isMember("fee_rate_hint") || obj.isMember("maximum_fee_una") || obj.isMember("audit_context")) {
            uint64_t rate=0,maximum=0;
            if(!unsignedInteger(obj["fee_rate_hint"],rate) || !unsignedInteger(obj["maximum_fee_una"],maximum) || !obj["audit_context"].isString())
                return errorObj("durable withdrawal requires explicit integer fees and string audit_context");
            terms=dinero::vault::WithdrawalPaymentTerms{rate,maximum,obj["audit_context"].asString()};
            dinero::vault::ValidateWithdrawalPaymentTerms(*terms);
        }
        dinero::vault::WithdrawalId id = terms?svc->enqueueWithdrawal(account, amount, spk,*terms):svc->enqueueWithdrawal(account, amount, spk);
        result["request_id"] = arrayToHex16(id);
        result["status"] = "pending";
    } catch (const dinero::vault::WithdrawalQueueError& e) {
        return errorObj(std::string("withdrawal_queue: ") + e.what());
    } catch (const std::exception& e) {
        return errorObj(std::string("withdrawal owner: ") + e.what());
    }
    return result;
}

Json rpc_vault_processnext(const ExecutionContext& /*ctx*/, const Json& /*params*/) {
    Json result;
    auto svc = requireService();
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
    auto svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    if (!params.isArray() || params.size() != 1 || !accountString(params[0])) {
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
    } else if (auto* retained = std::get_if<dinero::vault::WithdrawalPaymentRetained>(&state); retained != nullptr) {
        result["state"] = "payment_retained";
        auto display=retained->txid;std::reverse(display.begin(),display.end());
        result["txid"] = arrayToHex(display);
        result["vout"] = static_cast<Json::UInt64>(retained->vout);
        result["fee_una"] = static_cast<Json::UInt64>(retained->fee_una);
    } else if (auto* failed = std::get_if<dinero::vault::WithdrawalFailed>(&state); failed != nullptr) {
        result["state"] = "failed";
        result["reason"] = failed->reason;
    }
    return result;
}

Json rpc_vault_setoperator(const ExecutionContext& /*ctx*/, const Json& params) {
    Json result;
    if (!oneObject(params)) {
        return errorObj("missing parameter object");
    }
    const Json& obj = params[0];
    if(!obj["address"].isString() || obj["address"].asString().find('\0')!=std::string::npos ||
       (obj.isMember("account") && (!obj["account"].isString() || obj["account"].asString().find('\0')!=std::string::npos)))
        return errorObj("address must be an explicit string; account must be a string when supplied");
    const std::string address=obj["address"].asString();
    const std::string account=obj.isMember("account")?obj["account"].asString():"";
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
    auto svc = requireService();
    if (svc == nullptr) {
        return errorObj("vault service not initialized");
    }
    const auto metrics = svc->metrics();
    result["total_open_credits_una"] = static_cast<Json::UInt64>(metrics.total_open_credits);
    result["total_operator_loss_una"] = static_cast<Json::UInt64>(metrics.total_operator_loss);
    result["account_count"] = static_cast<Json::UInt64>(metrics.account_count);
    result["ledger_next_seq"] = static_cast<Json::UInt64>(metrics.ledger_next_seq);
    result["withdrawal_queue_depth"] = metrics.withdrawal_queue_depth;
    return result;
}

}  // namespace din

void RegisterVaultRPC() {
    dinero::g_logger.info("  Registering Liquidity Vault RPC methods...");
    g_rpcRegistry.registerHandler("vault.create", din::rpc_vault_create);
    g_rpcRegistry.registerHandler("vault.list", din::rpc_vault_list);
    g_rpcRegistry.registerHandler("vault.open", din::rpc_vault_open);
    g_rpcRegistry.registerHandler("vault.account.spendable", din::rpc_vault_account_spendable);
    g_rpcRegistry.registerHandler("vault.account.metrics", din::rpc_vault_account_metrics);
    g_rpcRegistry.registerHandler("vault.observe", din::rpc_vault_observe);
    g_rpcRegistry.registerHandler("vault.withdraw", din::rpc_vault_withdraw);
    g_rpcRegistry.registerHandler("vault.processnext", din::rpc_vault_processnext);
    g_rpcRegistry.registerHandler("vault.withdrawal.status", din::rpc_vault_withdrawal_status);
    g_rpcRegistry.registerHandler("vault.metrics", din::rpc_vault_metrics);
    g_rpcRegistry.registerHandler("vault.setoperator", din::rpc_vault_setoperator);
    g_rpcRegistry.registerHandler("vault.getoperator", din::rpc_vault_getoperator);
    dinero::g_logger.info("  Registered 12 Liquidity Vault RPC methods");
}

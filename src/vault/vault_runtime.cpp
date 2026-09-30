// Copyright (c) 2026 Dinero Labs.
//
// Daemon-scoped runtime owner for the singleton VaultService.

#include "vault/vault_runtime.h"

#include "address/addr_codec.h"
#include "external/bech32/bech32.hpp"
#include "common/logger.h"
#include "util/hex.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include "primitives/block.h"
#include "primitives/hash_domains.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "rpc/methods_vault.h"
#include "rpc/rpc_registry.h"
#include "storage/chain_db.h"
#include "vault/deposit_flow.h"
#include "vault/ledger.h"
#include "vault/ledger_entry.h"
#include "vault/ledger_store.h"
#include "vault/signing_backend.h"
#include "vault/vault_service.h"
#include "vault/vault_types.h"
#include "vault/wallet_signing_backend.h"
#include "vault/withdrawal_queue.h"
#include "vault/wallet_withdrawal_dispatch.h"
#include "consensus/chainparams.h"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace dinero::vault {

namespace {

std::mutex g_runtime_mu;
std::shared_ptr<VaultService> g_service;
std::unique_ptr<LedgerStore> g_store;
std::atomic<bool> g_initialized{false};
std::shared_ptr<WalletWithdrawalDispatchOwner> g_dispatch_owner;
bool g_closing{false};
uint64_t g_runtime_generation{0};

// This backend identifies the retained wallet-payment path. Legacy broadcast
// is forbidden: real signing/admission happens through the bound dispatcher.
// No independently certified liquidity float exists at this boundary yet.
class RetainedWalletBackend final : public SigningBackend {
    BackendId id_{"wallet-retained"};
public:
    const BackendId& backendId() const noexcept override {return id_;}
    UnaAmount availableFloat() override {
        throw SigningBackendError(SigningBackendError::Kind::UNAVAILABLE,
            "vault liquidity float is not independently qualified");
    }
    std::array<uint8_t,32> signAndBroadcast(const UnsignedTx&) override {
        throw SigningBackendError(SigningBackendError::Kind::UNAVAILABLE,
            "durable vault requires retained wallet-payment dispatch");
    }
    HealthReport healthcheck() override {
        return {id_,BackendDegraded{"liquidity float is not independently qualified"},0,0};
    }
};

struct ClosingRuntime {
    std::shared_ptr<VaultService> service;
    std::shared_ptr<WalletWithdrawalDispatchOwner> dispatch;
};
ClosingRuntime PrepareRuntimeClose() {
    ClosingRuntime current;
    {
        std::lock_guard lock(g_runtime_mu);
        g_closing=true;
        if(g_runtime_generation!=UINT64_MAX)++g_runtime_generation;
        current={g_service,g_dispatch_owner};
    }
    try {if(current.dispatch)current.dispatch->Close();}
    catch(...) {
        std::lock_guard lock(g_runtime_mu);
        if(g_service==current.service && g_dispatch_owner==current.dispatch)g_closing=false;
        throw;
    }
    return current;
}

// Decoded operator scriptPubKey — the auto-observer's match key.
// Empty means no auto-observer; deposits flow only via vault.observe.
std::vector<uint8_t> g_operator_script;
// The operator address as configured (display-order). Cached so
// GetVaultOperator can return it without re-encoding from the script.
std::string g_operator_address_str;
AccountId g_default_account{};

std::string toHexLower(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[(data[i] >> 4) & 0xf]);
        out.push_back(digits[data[i] & 0xf]);
    }
    return out;
}

// Convert a Taproot/P2WPKH/P2WSH/P2MR scriptPubKey to its bech32m
// address. Mirror of wallet_worker.cpp's ScriptPubKeyToAddress; kept
// local to vault_runtime so the wallet send closure can render the
// destination string without pulling in the wallet helper.
std::string scriptToAddress(const std::vector<uint8_t>& spk) {
    const std::string& hrp = HrpForActiveNetworkRef();
    // P2TR: OP_1 PUSH32 <32-byte witness program>
    if (spk.size() == 34 && spk[0] == 0x51 && spk[1] == 0x20) {
        std::vector<uint8_t> wp(spk.begin() + 2, spk.end());
        return bech32::Encode(hrp, 1, wp, bech32::Encoding::BECH32M);
    }
    return {};
}

}  // namespace

bool InitializeVaultRuntime(VaultRuntimeConfig config) {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    if (g_closing || g_runtime_generation==UINT64_MAX)
        throw std::runtime_error("vault runtime is closing or exhausted");
    if (g_initialized.load()) {
        return true;
    }
    if (!config.enabled) {
        dinero::g_logger.info("[Vault] disabled by config; runtime not initialised");
        return false;
    }

    if (!config.block_hash_at_height || !config.tx_included_at)
        throw std::runtime_error("enabled vault runtime requires canonical readers");

    // Prepare all publication state locally. Invalid configuration, file reads,
    // allocation or backend construction must not leave a partial global owner.
    std::vector<uint8_t> operator_script;
    std::string operator_address;
    AccountId default_account{};
    if (!config.operator_address.empty()) {
        if (config.operator_address.find('\0')!=std::string::npos ||
            config.default_account.find('\0')!=std::string::npos)
            throw std::runtime_error("invalid vault operator binding");
        operator_script=CreateP2TRScriptPubKey(DecodeTaprootWitnessProgram(config.operator_address));
        operator_address=config.operator_address;
        default_account.raw=config.default_account.empty()?"default":config.default_account;
    }
    std::unique_ptr<LedgerStore> store;
    std::string idempotency_path;
    if (!config.persistence_path.empty()) {
        const std::filesystem::path path(config.persistence_path);
        if (config.persistence_path.find('\0')!=std::string::npos)
            throw std::runtime_error("invalid vault persistence path");
        idempotency_path=(path.parent_path()/"idempotency.jsonl").string();
        // This initializer has no complete-state restore implementation. Even
        // apparently valid old rows or sidecar IDs cannot authorize an empty
        // service. Inspect before append-open/backend construction; preserve
        // all bytes and leave recovery to an explicit authenticated owner.
        for (const auto& existing:{path,std::filesystem::path(idempotency_path)}) {
            if (!std::filesystem::exists(existing))continue;
            if (!std::filesystem::is_regular_file(existing) ||
                std::filesystem::file_size(existing)!=0)
                throw std::runtime_error("legacy vault records require authenticated complete-state recovery");
        }
        if (!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        store=std::make_unique<FileLedgerStore>(config.persistence_path);
        if (!store->loadAll().empty())
            throw std::runtime_error("legacy vault records changed during initialization");
    }

    // The legacy signing callback remains scoped to this initializer. New
    // durable wallet withdrawals use their separate retained-body dispatcher.
    auto send_via_wallet_rpc = [](const std::vector<uint8_t>& script_pub_key,
                                   UnaAmount amount, UnaAmount fee_rate_hint,
                                   const std::string& audit_context) -> std::array<uint8_t, 32> {
        std::string address = scriptToAddress(script_pub_key);
        if (address.empty()) {
            throw SigningBackendError(SigningBackendError::Kind::REJECTED_BY_POLICY,
                                      "vault withdrawal: only Taproot destinations are supported");
        }
        auto* handler = g_rpcRegistry.lookup("wallet.sendtoaddress");
        if (handler == nullptr) {
            throw SigningBackendError(SigningBackendError::Kind::UNAVAILABLE,
                                      "wallet.sendtoaddress not registered");
        }
        ExecutionContext ctx;
        ctx.daemon = ::DaemonContext::instance();
        ctx.logger = ctx.daemon ? ctx.daemon->logger_interface : nullptr;

        din::Json params;
        params["address"] = address;
        params["amount"] = static_cast<double>(amount) / 1e8;  // una → DIN
        if (fee_rate_hint > 0) {
            params["fee_rate"] = static_cast<double>(fee_rate_hint);
        }
        if (!audit_context.empty()) {
            params["comment"] = audit_context;
        }

        din::Json result = (*handler)(ctx, params);
        if (result.isMember("error")) {
            throw SigningBackendError(SigningBackendError::Kind::BROADCAST_FAILED,
                                      "wallet.sendtoaddress: " + result["error"].asString());
        }
        if (!result.isMember("txid") || !result["txid"].isString()) {
            throw SigningBackendError(SigningBackendError::Kind::BROADCAST_FAILED,
                                      "wallet.sendtoaddress returned no txid");
        }
        std::string txid_hex = result["txid"].asString();
        if (txid_hex.size() != 64) {
            throw SigningBackendError(SigningBackendError::Kind::BROADCAST_FAILED,
                                      "wallet.sendtoaddress returned malformed txid");
        }
        std::array<uint8_t, 32> txid{};
        for (size_t i = 0; i < 32; ++i) {
            unsigned hi = 0;
            unsigned lo = 0;
            std::sscanf(txid_hex.c_str() + (2 * i), "%1x", &hi);
            std::sscanf(txid_hex.c_str() + (2 * i) + 1, "%1x", &lo);
            txid[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        return txid;
    };

    auto wallet_float_lookup = []() -> UnaAmount {
        auto* daemon = ::DaemonContext::instance();
        if (daemon == nullptr || !daemon->wallet) {
            return 0;
        }
        auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(daemon->wallet);
        if (!wallet_service || !wallet_service->hasActiveWallet()) {
            return 0;
        }
        try {
            auto wallet_use=WalletService::AcquireWalletUse(wallet_service);
            auto balance = wallet_use->Wallet().getBalance(nullptr);
            // Balance is in DIN; convert to una.
            return static_cast<UnaAmount>(balance.spendable * 1e8);
        } catch (...) {
            return 0;
        }
    };

    auto backend = std::make_unique<WalletSigningBackend>(
        BackendId{"wallet"}, std::move(send_via_wallet_rpc),
        std::move(wallet_float_lookup), idempotency_path);

    VaultServiceConfig service_config;
    service_config.shadow_mode = config.shadow_mode;
    service_config.ledger_caps = LedgerCaps::unbounded();
    service_config.withdrawal_caps = WithdrawalCaps::unbounded();
    service_config.confirmation_policy.k_observe = config.k_observe;
    service_config.confirmation_policy.k_credit = config.k_credit;
    service_config.confirmation_policy.k_settle = config.k_settle;

    auto block_hash_fn = std::move(config.block_hash_at_height);
    auto tx_included_fn = std::move(config.tx_included_at);
    auto block_hash_for_watcher = [block_hash_fn](uint64_t h) { return block_hash_fn(h); };
    auto tx_included_for_watcher =
        [tx_included_fn](const OutpointId& op, uint64_t h, const std::array<uint8_t, 32>& bh) {
            return tx_included_fn(op.txid_raw, op.vout, h, bh);
        };

    auto service = std::make_shared<VaultService>(
        std::move(backend), service_config,
        std::move(block_hash_for_watcher), std::move(tx_included_for_watcher),
        std::move(config.capture_tip));

    std::string status = "[Vault] runtime initialised; shadow_mode=";
    status += (config.shadow_mode ? "true" : "false");
    status += ", k_credit=" + std::to_string(config.k_credit);
    status += ", k_settle=" + std::to_string(config.k_settle);
    if (!operator_script.empty()) {
        status += ", auto-observer=ON address=" + config.operator_address;
        status += " account=" + default_account.raw;
    } else {
        status += ", auto-observer=OFF";
    }
    if (!idempotency_path.empty()) {
        status += ", idempotency=" + idempotency_path;
    }
    // The publication operations below do not allocate. The initialized flag
    // is last, after every reader/backend and configured file has prepared.
    g_operator_script.swap(operator_script);
    g_operator_address_str.swap(operator_address);
    g_default_account.raw.swap(default_account.raw);
    g_store.swap(store);
    g_service.swap(service);
    g_initialized.store(true);
    try {dinero::g_logger.info(status);}catch(...) {} // logging cannot undo publication
    return true;
}

void OpenExistingVaultRuntime(VaultRuntimeConfig config,const ExecutionContext& context,
    std::shared_ptr<WalletService> wallet,const WalletSigningIdentity& selected,const VaultIdentity& identity) {
    uint64_t generation;
    {
        std::lock_guard lock(g_runtime_mu);
        if(g_initialized.load() || g_closing || g_runtime_generation==UINT64_MAX)
            throw std::runtime_error("vault runtime is already attached or unavailable");
        generation=g_runtime_generation;
    }
    if(!config.enabled || !config.block_hash_at_height || !config.tx_included_at ||
       !config.capture_tip || !config.persistence_path.empty())
        throw std::runtime_error("existing vault attachment requires canonical readers and wallet storage");
    VaultStateDomain domain;domain.network=static_cast<uint8_t>(GetActiveChain());
    uint256 genesis;
    if(!uint256::FromHex(Params().genesis_hash,genesis))
        throw std::runtime_error("vault chain domain unavailable");
    std::copy(genesis.begin(),genesis.end(),domain.genesis.begin());
    auto dispatch=std::make_shared<WalletWithdrawalDispatchOwner>(context,wallet,selected,domain);
    auto tx_included=std::move(config.tx_included_at);
    auto bound=WalletVaultStateOwner::OpenExistingService(wallet,selected.session,domain,identity,
        std::make_unique<RetainedWalletBackend>(),std::move(config.block_hash_at_height),
        [tx_included](const OutpointId& out,uint64_t height,const std::array<uint8_t,32>& hash) {
            return tx_included(out.txid_raw,out.vout,height,hash);
        },std::move(config.capture_tip),dispatch->Factory());
    const auto binding=bound.service->operatorBinding();
    if(!binding)throw std::runtime_error("historical vault has no authenticated operator binding");
    ValidateVaultOperatorBinding(*binding);
    auto script=binding->script_pub_key;
    auto address=scriptToAddress(script);auto account=binding->account;
    if(address.empty() || (!config.operator_address.empty() && config.operator_address!=address) ||
       (!config.default_account.empty() && config.default_account!=account))
        throw std::runtime_error("vault configuration differs from the authenticated operator binding");
    // All wallet/SQLite/seed owners have been released. Shutdown invalidates
    // this preparation by generation; it never waits while holding those owners.
    // Only nonthrowing swaps follow the final publication check.
    std::lock_guard lock(g_runtime_mu);
    if(g_initialized.load() || g_closing || generation!=g_runtime_generation)
        throw std::runtime_error("vault runtime changed during attachment");
    g_operator_script.swap(script);g_operator_address_str.swap(address);g_default_account.raw.swap(account);
    g_dispatch_owner.swap(dispatch);g_service.swap(bound.service);g_initialized.store(true);
}

void CloseVaultRuntimeDispatch() {(void)PrepareRuntimeClose();}

void ShutdownVaultRuntime() {
    const auto closing=PrepareRuntimeClose();
    std::shared_ptr<VaultService> service;
    std::shared_ptr<WalletWithdrawalDispatchOwner> dispatch;
    std::unique_ptr<LedgerStore> store;
    {
        std::lock_guard lock(g_runtime_mu);
        // A second close must never detach a replacement published after the
        // first close completed. Each retained dispatcher is closed separately.
        if(g_service!=closing.service || g_dispatch_owner!=closing.dispatch)return;
        service=std::move(g_service);dispatch=std::move(g_dispatch_owner);store=std::move(g_store);
        g_operator_script.clear();g_operator_address_str.clear();g_default_account=AccountId{};
        g_initialized.store(false);g_closing=false;
    }
    service.reset();dispatch.reset();
    if(store)store->flush();
    dinero::g_logger.info("[Vault] runtime shut down");
}

void NotifyVaultTipConnected(uint64_t height) {
    const auto svc=GetVaultRuntimeService();
    if (!svc) return;
    try {
        svc->tipChanged(height);
    } catch (const std::exception& e) {
        dinero::g_logger.warn(std::string("[Vault] tipChanged threw: ") + e.what());
    }
}

void NotifyVaultTipDisconnected(uint64_t /*height*/) {
    // Reserved hook. The reorg watcher detects disconnections via
    // the next NotifyVaultTipConnected call's block-hash mismatch,
    // which is the safer signal source (chain-tip transient states
    // don't accidentally trigger compensating debits).
    if (!g_initialized.load()) {
        return;
    }
}

std::shared_ptr<VaultService> GetVaultRuntimeService() {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    return g_initialized.load() && !g_closing?g_service:std::shared_ptr<VaultService>{};
}

bool SetVaultOperator(const std::string& address, const std::string& account,
                      std::string* error_out) {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    if (!g_initialized.load() || g_closing) {
        if (error_out != nullptr) {
            *error_out = "vault runtime not initialised";
        }
        return false;
    }

    if(g_dispatch_owner) {
        if(error_out)*error_out="authenticated vault operator binding is immutable";
        return false;
    }

    if (address.empty()) {
        // Caller wants to disable the auto-observer.
        g_operator_script.clear();
        g_operator_address_str.clear();
        if (!account.empty()) {
            g_default_account.raw = account;
        }
        dinero::g_logger.info("[Vault] auto-observer disabled (no operator address)");
        return true;
    }

    std::vector<uint8_t> script;
    try {
        std::vector<uint8_t> witness_program = DecodeTaprootWitnessProgram(address);
        script = CreateP2TRScriptPubKey(witness_program);
    } catch (const std::exception& e) {
        if (error_out != nullptr) {
            *error_out = std::string("decode failed: ") + e.what();
        }
        return false;
    }

    g_operator_script = std::move(script);
    g_operator_address_str = address;
    g_default_account.raw = account.empty() ? "default" : account;

    dinero::g_logger.info(std::string("[Vault] operator bound: address=") + address +
                          " account=" + g_default_account.raw);
    return true;
}

OperatorBinding GetVaultOperator() {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    OperatorBinding out;
    if(g_closing)return out;
    out.address = g_operator_address_str;
    out.account = g_default_account.raw;
    return out;
}

bool VerifyOperatorDeposit(const std::shared_ptr<VaultService>& expected_service,
                           const std::array<uint8_t,32>& txid_raw,uint32_t vout,
                           uint64_t& out_amount,uint64_t& out_height,
                           std::array<uint8_t,32>& out_block_hash_raw,std::string& err) {
    std::vector<uint8_t> script;
    {
        std::lock_guard<std::mutex> lock(g_runtime_mu);
        if (!g_initialized.load() || g_closing || !expected_service || g_service!=expected_service) {
            err="vault runtime owner changed or unavailable";return false;
        }
        script=g_operator_script;
    }
    if (script.empty()) {err="no vault operator script configured";return false;}
    auto* daemon=::DaemonContext::instance();
    const auto source=daemon?daemon->chainstate:nullptr;
    if (!source) {err="chainstate unavailable";return false;}
    try {
        const auto lifetime=ChainstateService::AcquireWalletIndexUse(source);
        const auto selected=source->AcquireBlockIngressActivationLock();
        auto* db=source->GetChainDB();
        if (!db) {err="chain db unavailable";return false;}
        uint256 txid;std::copy(txid_raw.begin(),txid_raw.end(),txid.begin());
        const auto coin=db->getCoin(txid,vout);
        if (!coin.ok() || coin->height<0) {err="canonical unspent output unavailable";return false;}
        if (coin->is_confidential || coin->amount==0) {
            err="deposit must be a nonzero transparent output";return false;
        }
        std::vector<uint8_t> recorded_script;
        const auto ascii_hex=[](unsigned char c) {
            return (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F');
        };
        if (coin->script_pubkey.size()!=script.size()*2 ||
            !std::all_of(coin->script_pubkey.begin(),coin->script_pubkey.end(),ascii_hex) ||
            !util::unhex(coin->script_pubkey,recorded_script) || recorded_script!=script) {
            err="outpoint does not pay the vault operator script";return false;
        }
        const auto included=source->getCanonicalOutputInclusion(txid,vout,static_cast<uint32_t>(coin->height));
        if (!included.ok() || !included->included) {err="canonical deposit inclusion unavailable";return false;}
        if (!included->MatchesTransparent(coin->amount,recorded_script)) {
            err="unspent coin differs from its canonical output";return false;
        }
        // Publish only after the same selected observation has bound the
        // persisted unspent coin's amount and script to its canonical body.
        out_amount=coin->amount;out_height=static_cast<uint64_t>(coin->height);
        std::copy(included->block_hash.begin(),included->block_hash.end(),out_block_hash_raw.begin());
        err.clear();return true;
    } catch (const std::exception& e) {err=std::string("deposit source unavailable: ")+e.what();return false;}
}

VaultWalletOutputObserver CaptureVaultWalletOutputObserver() {
    std::shared_ptr<VaultService> service;
    std::vector<uint8_t> script;
    AccountId account;
    {
        std::lock_guard<std::mutex> lock(g_runtime_mu);
        if (!g_initialized.load() || g_closing || !g_service || g_operator_script.empty()) return {};
        service=g_service;script=g_operator_script;account=g_default_account;
    }
    auto* daemon=::DaemonContext::instance();
    const auto source=daemon?daemon->chainstate:nullptr;
    return [service=std::move(service),script=std::move(script),account=std::move(account),source](
        const std::array<uint8_t,32>& txid_raw,uint32_t vout,
        const std::vector<uint8_t>& output_script,uint64_t amount,uint64_t height,
        const std::string& block_hash_hex) {
        if (output_script!=script) return;
        // Decode the complete display-order hash before any service effect.
        const auto digit=[](unsigned char c)->int {
            if(c>='0'&&c<='9')return c-'0';
            if(c>='a'&&c<='f')return c-'a'+10;
            if(c>='A'&&c<='F')return c-'A'+10;
            return -1;
        };
        if(block_hash_hex.size()!=64)
            throw std::runtime_error("vault wallet observation hash malformed");
        std::array<uint8_t,32> hash{};
        for(size_t i=0;i<32;++i) {
            const int hi=digit(block_hash_hex[2*i]),lo=digit(block_hash_hex[2*i+1]);
            if(hi<0||lo<0)throw std::runtime_error("vault wallet observation hash malformed");
            hash[31-i]=static_cast<uint8_t>((hi<<4)|lo);
        }
        if(!std::any_of(hash.begin(),hash.end(),[](uint8_t v){return v!=0;}))
            throw std::runtime_error("vault wallet observation hash missing");
        uint64_t effective_tip=height;
        if(source) {
            // Complete the selected read before service mutation. No wallet,
            // runtime or vault mutex may be held by this callback's caller.
            const auto lifetime=ChainstateService::AcquireWalletIndexUse(source);
            const auto selected=source->AcquireBlockIngressActivationLock();
            auto* db=source->GetChainDB();
            if(!db)throw std::runtime_error("vault wallet observation source unavailable");
            const auto tip=db->getTip();
            if(!tip.ok() || tip->height<0)
                throw std::runtime_error("vault wallet observation tip unavailable");
            effective_tip=std::max(height,static_cast<uint64_t>(tip->height));
        }
        service->recordDeposit(txid_raw,vout,account,amount,height,hash);
        service->tipChanged(effective_tip);
    };
}

void ObserveWalletOutput(const std::array<uint8_t,32>& txid_raw,uint32_t vout,
    const std::vector<uint8_t>& script,uint64_t amount,uint64_t height,const std::string& hash) {
    try {
        const auto observer=CaptureVaultWalletOutputObserver();
        if(observer)observer(txid_raw,vout,script,amount,height,hash);
    } catch(const std::exception& e) {
        dinero::g_logger.warn(std::string("[Vault] wallet observation failed: ")+e.what());
    }
}

namespace {

std::array<uint8_t, 32> uint256ToArray(const uint256& v) {
    std::array<uint8_t, 32> out{};
    std::memcpy(out.data(), v.begin(), 32);
    return out;
}

uint256 arrayToUint256(const std::array<uint8_t, 32>& a) {
    uint256 v;
    std::memcpy(v.begin(), a.data(), 32);
    return v;
}

}  // namespace

std::function<std::array<uint8_t, 32>(uint64_t)>
MakeChainstateBlockHashClosure(::DaemonContext& ctx) {
    return [source=ctx.chainstate](uint64_t height) -> std::array<uint8_t,32> {
        if (!source || height>uint64_t(std::numeric_limits<uint32_t>::max())) return {};
        try {
            const auto index=ChainstateService::AcquireWalletIndexUse(source);
            const auto hash=source->getCanonicalBlockHash(static_cast<uint32_t>(height));
            return hash.ok()?uint256ToArray(*hash):std::array<uint8_t,32>{};
        } catch (...) { return {}; }
    };
}

std::function<bool(const std::array<uint8_t,32>&,uint32_t,uint64_t,const std::array<uint8_t,32>&)>
MakeChainstateTxIncludedClosure(::DaemonContext& ctx) {
    return [source=ctx.chainstate](const std::array<uint8_t,32>& txid,uint32_t output,
                                  uint64_t height,const std::array<uint8_t,32>& expected_hash) -> bool {
        if (!source || height>uint64_t(std::numeric_limits<uint32_t>::max()))
            throw std::runtime_error("Vault canonical inclusion source unavailable");
        const auto index=ChainstateService::AcquireWalletIndexUse(source);
        const auto observed=source->getCanonicalOutputInclusion(arrayToUint256(txid),output,static_cast<uint32_t>(height));
        if (!observed.ok() || observed->block_hash!=arrayToUint256(expected_hash))
            throw std::runtime_error("Vault canonical inclusion observation unavailable or changed");
        return observed->included;
    };
}


VaultTipSnapshotFn MakeChainstateVaultSnapshotClosure(::DaemonContext& ctx) {
    return [source=ctx.chainstate](uint64_t height,
              const std::vector<VaultDepositQuery>& queries) -> VaultTipSnapshot {
        if (!source || height > uint64_t(std::numeric_limits<uint32_t>::max()))
            throw std::runtime_error("vault selected source unavailable");
        const auto lifetime = ChainstateService::AcquireWalletIndexUse(source);
        const auto selected = source->AcquireBlockIngressActivationLock();
        auto* db = source->GetChainDB();
        if (!db) throw std::runtime_error("vault selected database unavailable");
        const auto tip = db->getTip();
        if (!tip.ok() || tip->height < 0 || uint64_t(tip->height) != height)
            throw std::runtime_error("vault requested tip changed or unavailable");
        const auto canonical = source->getCanonicalBlockHash(static_cast<uint32_t>(height));
        if (!canonical.ok() || *canonical != tip->hash)
            throw std::runtime_error("vault selected tip is incoherent");
        VaultTipSnapshot snapshot{height, uint256ToArray(*canonical), {}};
        snapshot.deposits.reserve(queries.size());
        for (const auto& query : queries) {
            if (query.height > height) {
                // The selected tip is complete, so this height is absent.
                snapshot.deposits.push_back({query, std::nullopt, false});
                continue;
            }
            const auto output = source->getCanonicalOutputInclusion(
                arrayToUint256(query.outpoint.txid_raw), query.outpoint.vout,
                static_cast<uint32_t>(query.height));
            if (!output.ok())
                throw std::runtime_error("vault canonical deposit read unavailable");
            if (output->included && (!output->transparent_amount ||
                *output->transparent_amount != query.amount))
                throw std::runtime_error("vault canonical deposit amount mismatch");
            snapshot.deposits.push_back({query, uint256ToArray(output->block_hash), output->included});
        }
        return snapshot;
    };
}

}  // namespace dinero::vault

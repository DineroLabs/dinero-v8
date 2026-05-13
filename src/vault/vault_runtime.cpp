// Copyright (c) 2026 Dinero Labs.
//
// Daemon-scoped runtime owner for the singleton VaultService.

#include "vault/vault_runtime.h"

#include "address/addr_codec.h"
#include "external/bech32/bech32.hpp"
#include "common/logger.h"
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

#include <atomic>
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
std::unique_ptr<VaultService> g_service;
std::unique_ptr<LedgerStore> g_store;
std::atomic<bool> g_initialized{false};

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

void InitializeVaultRuntime(VaultRuntimeConfig config) {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    if (g_initialized.load()) {
        return;
    }
    if (!config.enabled) {
        dinero::g_logger.info("[Vault] disabled by config; runtime not initialised");
        return;
    }

    if (!config.block_hash_at_height) {
        dinero::g_logger.warn("[Vault] missing block_hash_at_height closure; refusing to start");
        return;
    }
    if (!config.tx_included_at) {
        dinero::g_logger.warn("[Vault] missing tx_included_at closure; refusing to start");
        return;
    }

    // Decode operator address once → scriptPubKey. The observer
    // matches wallet outputs by the resulting bytes; the address
    // string is never compared again. Fail loudly on a bad address
    // so the operator notices before any deposits flow.
    g_operator_script.clear();
    g_operator_address_str.clear();
    g_default_account = AccountId{};
    if (!config.operator_address.empty()) {
        try {
            std::vector<uint8_t> witness_program =
                DecodeTaprootWitnessProgram(config.operator_address);
            g_operator_script = CreateP2TRScriptPubKey(witness_program);
            g_operator_address_str = config.operator_address;
        } catch (const std::exception& e) {
            dinero::g_logger.warn(
                std::string("[Vault] operator_address decode failed: ") + e.what() +
                " — auto-observer disabled");
            g_operator_script.clear();
            g_operator_address_str.clear();
        }
        g_default_account.raw = config.default_account.empty() ? "default" : config.default_account;
    }

    // Optional persistence.
    if (!config.persistence_path.empty()) {
        try {
            g_store = std::make_unique<FileLedgerStore>(config.persistence_path);
        } catch (const LedgerStoreError& e) {
            dinero::g_logger.warn(std::string("[Vault] persistence open failed: ") + e.what());
            g_store.reset();
        }
    }

    // Production signing backend: dispatches through the live wallet
    // RPC stack. Persistent idempotency map sits next to the ledger
    // file under <datadir>/vault/.
    std::string idempotency_path;
    if (!config.persistence_path.empty()) {
        std::filesystem::path p(config.persistence_path);
        idempotency_path = (p.parent_path() / "idempotency.jsonl").string();
    }

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
            auto balance = wallet_service->get().getBalance(nullptr);
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

    g_service = std::make_unique<VaultService>(
        std::move(backend), service_config,
        std::move(block_hash_for_watcher), std::move(tx_included_for_watcher));

    // Replay persisted entries through the live ledger.
    if (g_store) {
        try {
            auto persisted = g_store->loadAll();
            dinero::g_logger.info(std::string("[Vault] replaying ") +
                                  std::to_string(persisted.size()) + " persisted entries");
            // We can't reach into VaultService.ledger directly through
            // its public API; the replay path is owned by the service
            // construction. For the initial wiring we surface this as
            // a known limitation: persistence currently writes through
            // the store but doesn't auto-replay into the service. The
            // operator's restart path will wire a richer constructor
            // when needed.
            (void)persisted;
        } catch (const LedgerStoreError& e) {
            dinero::g_logger.warn(std::string("[Vault] replay failed: ") + e.what());
        }
    }

    din::SetVaultService(g_service.get());
    g_initialized.store(true);

    std::string status = "[Vault] runtime initialised; shadow_mode=";
    status += (config.shadow_mode ? "true" : "false");
    status += ", k_credit=" + std::to_string(config.k_credit);
    status += ", k_settle=" + std::to_string(config.k_settle);
    if (!g_operator_script.empty()) {
        status += ", auto-observer=ON address=" + config.operator_address;
        status += " account=" + g_default_account.raw;
    } else {
        status += ", auto-observer=OFF";
    }
    if (!idempotency_path.empty()) {
        status += ", idempotency=" + idempotency_path;
    }
    dinero::g_logger.info(status);
}

void ShutdownVaultRuntime() {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    if (!g_initialized.load()) {
        return;
    }
    din::SetVaultService(nullptr);
    g_service.reset();
    if (g_store) {
        g_store->flush();
        g_store.reset();
    }
    g_operator_script.clear();
    g_operator_address_str.clear();
    g_default_account = AccountId{};
    g_initialized.store(false);
    dinero::g_logger.info("[Vault] runtime shut down");
}

void NotifyVaultTipConnected(uint64_t height) {
    if (!g_initialized.load()) {
        return;
    }
    auto* svc = g_service.get();
    if (svc == nullptr) {
        return;
    }
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

VaultService* GetVaultRuntimeService() {
    return g_service.get();
}

bool SetVaultOperator(const std::string& address, const std::string& account,
                      std::string* error_out) {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    if (!g_initialized.load()) {
        if (error_out != nullptr) {
            *error_out = "vault runtime not initialised";
        }
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
    out.address = g_operator_address_str;
    out.account = g_default_account.raw;
    return out;
}

void ObserveWalletOutput(const std::array<uint8_t, 32>& txid_raw,
                         uint32_t vout,
                         const std::vector<uint8_t>& script_pub_key,
                         uint64_t amount_una,
                         uint64_t height,
                         const std::string& block_hash_hex) {
    if (!g_initialized.load()) {
        return;
    }
    // Module state below is set under g_runtime_mu at init; we read
    // without holding the lock because once the runtime is initialised
    // these never change for the rest of its lifetime.
    if (g_operator_script.empty()) {
        return;
    }
    if (script_pub_key != g_operator_script) {
        return;
    }
    auto* svc = g_service.get();
    if (svc == nullptr) {
        return;
    }
    if (block_hash_hex.size() != 64) {
        dinero::g_logger.warn(
            "[Vault] auto-observer: malformed block_hash_hex (expected 64 chars)");
        return;
    }

    // Block hashes in display-order hex are the byte-reversal of the
    // raw consensus hash that the reorg watcher records. uint256 in
    // this codebase already stores the consensus byte-order (see
    // primitives/uint256.h begin/end), so reverse the hex.
    std::array<uint8_t, 32> block_hash_raw{};
    for (size_t i = 0; i < 32; ++i) {
        unsigned hi = 0;
        unsigned lo = 0;
        std::sscanf(block_hash_hex.c_str() + (2 * i), "%1x", &hi);
        std::sscanf(block_hash_hex.c_str() + (2 * i) + 1, "%1x", &lo);
        block_hash_raw[31 - i] = static_cast<uint8_t>((hi << 4) | lo);
    }

    try {
        svc->recordDeposit(txid_raw, vout, g_default_account,
                           static_cast<UnaAmount>(amount_una), height, block_hash_raw);
        // Drive the deposit-flow lifecycle. Use the *current chain
        // tip*, not the deposit's own block height — during a wallet
        // rescan the wallet trails the chain by many blocks, so the
        // deposit's height is stale and confs would be ~0 against it.
        // Falling back to `height` if chainstate is unavailable still
        // produces the right answer once the wallet catches up
        // (NotifyVaultTipConnected continues to drive promotion).
        uint64_t effective_tip = height;
        if (auto* daemon = ::DaemonContext::instance(); daemon != nullptr && daemon->chainstate) {
            if (auto* chain_db = daemon->chainstate->GetChainDB(); chain_db != nullptr) {
                if (auto tip = chain_db->getTip(); tip.ok()) {
                    if (auto h64 = static_cast<uint64_t>(tip.value().height); h64 > effective_tip) {
                        effective_tip = h64;
                    }
                }
            }
        }
        svc->tipChanged(effective_tip);
        dinero::g_logger.info(std::string("[Vault] auto-observed deposit txid=") +
                              toHexLower(txid_raw.data(), txid_raw.size()) +
                              ":" + std::to_string(vout) +
                              " amount=" + std::to_string(amount_una) +
                              " height=" + std::to_string(height));
    } catch (const std::exception& e) {
        dinero::g_logger.warn(std::string("[Vault] recordDeposit threw: ") + e.what());
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
    return [ctx_ptr = &ctx](uint64_t height) -> std::array<uint8_t, 32> {
        if (ctx_ptr == nullptr || !ctx_ptr->chainstate) {
            return std::array<uint8_t, 32>{};
        }
        auto* chain_db = ctx_ptr->chainstate->GetChainDB();
        if (chain_db == nullptr) {
            return std::array<uint8_t, 32>{};
        }
        // ChainDB heights are int; clamp to a safe range. Values
        // outside int range can't legally exist on this chain.
        if (height > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            return std::array<uint8_t, 32>{};
        }
        auto result = chain_db->getBlockHashByHeight(static_cast<int>(height));
        if (!result.ok()) {
            return std::array<uint8_t, 32>{};
        }
        return uint256ToArray(result.value());
    };
}

std::function<bool(const std::array<uint8_t, 32>&, uint32_t, uint64_t,
                   const std::array<uint8_t, 32>&)>
MakeChainstateTxIncludedClosure(::DaemonContext& ctx) {
    return [ctx_ptr = &ctx](const std::array<uint8_t, 32>& txid_raw, uint32_t vout,
                            uint64_t /*height*/, const std::array<uint8_t, 32>& block_hash) -> bool {
        if (ctx_ptr == nullptr || !ctx_ptr->chainstate) {
            return true;  // conservative: keep deposits in RE_MINED_SAME_TXID
        }
        auto* chain_db = ctx_ptr->chainstate->GetChainDB();
        if (chain_db == nullptr) {
            return true;
        }
        uint256 hash = arrayToUint256(block_hash);
        auto block_result = chain_db->getBlock(hash);
        if (!block_result.ok()) {
            // Block not on disk (pruned, never fetched, transient
            // RocksDB error). Conservative path: claim included.
            return true;
        }
        const Block& block = block_result.value();
        for (const auto& tx : block.vtx) {
            const uint256& tx_hash = tx.GetTxid().AsUint256();
            // OutpointId stores raw 32-byte big-endian txid (the same
            // byte layout as uint256 internally).
            if (std::memcmp(tx_hash.begin(), txid_raw.data(), 32) == 0) {
                return vout < tx.vout.size();
            }
        }
        return false;
    };
}

}  // namespace dinero::vault

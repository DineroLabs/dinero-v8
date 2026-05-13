// =============================================================================
// TEST-ONLY ISOLATION SEAM — not for production
// =============================================================================
//
// This file provides no-op definitions for symbols that dinero_core REFERENCES
// but cannot resolve in a test-scope build because the real implementations
// drag in too much daemon infrastructure (RPC, ledger state machines, fee
// estimation, mining policy, shielded validation, etc.).
//
// History and current state:
//
//   ✅ BridgeNode (7 stubs) — RETIRED. Extracted to dinero_bridge static
//      library (see CMakeLists.txt "Layer 2c"). dinero_core PUBLIC-deps it,
//      so every test target that links dinero_core gets the real BridgeNode
//      methods transitively.
//
//   ✅ Pool (5 stubs) — RETIRED. Extracted to dinero_pool static library
//      (see CMakeLists.txt "Layer 2d"). dinero_core PUBLIC-deps it.
//      Surface was small + contained (consensus/subsidy.h, ChainDB,
//      common/logger.h) — no daemon cascade.
//
//   📌 Vault (2 stubs) — INTENTIONAL isolation seam, not a missing extraction.
//      vault_runtime.cpp is already compiled into dinero_core, but pulling
//      vault_runtime.obj drags the full VaultService cascade (Ledger +
//      DepositFlow + WithdrawalQueue + SigningBackend + RPC handlers +
//      daemon_context) into the link. Extraction does NOT help — the same
//      cascade would just live in dinero_vault.lib.
//
//      The architectural improvement here is to make vault observation a
//      runtime-injectable interface (vault_observer_t = function pointer
//      or std::function set by the daemon at boot, default no-op) rather
//      than a hard link-time dep. Until that refactor lands, the stubs
//      are correct: a "consensus invariants" test should never wake up
//      the vault state machines.
//
//   📌 Mempool extras (7 stubs, plus 6 more in mempool_test_stubs.cpp) —
//      INTENTIONAL isolation seam. src/daemon/mempool.cpp is 3349 lines
//      with 17 deep includes spanning RPC, mining policy, shielded
//      validation, fee estimation, RBF policy, silent-payment scanning,
//      P2P message types, and the chain layer. Extracting it into a
//      dinero_mempool static lib would just move the cascade.
//
//      The architectural improvement is the same shape as vault: replace
//      the hard link-time dep with a narrow injectable IMempool interface.
//      Test seams here are correct in the meantime.
//
// Adding new stubs is a smell. Before adding one, check whether the
// referenced symbol's real implementation has a small contained dep set
// (extract a library, like Pool) or a deep cascade (accept the seam, like
// Vault/Mempool, and consider an injectable interface).

#include "daemon/mempool.h"
#include "vault/vault_runtime.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ─── Mempool extras — legitimate isolation seam ────────────────────────────
// 7 Mempool method stubs not already covered by mempool_test_stubs.cpp.
// See the Mempool entry in the file header for why these stay stubbed.

namespace dinero {

std::optional<MempoolEntry> Mempool::getMempoolEntry(const uint256&) const {
    return std::nullopt;
}

std::optional<uint64_t> Mempool::getTransactionFee(const uint256&) const {
    return std::nullopt;
}

size_t Mempool::size() const {
    return 0;
}

size_t Mempool::getTotalSize() const {
    return 0;
}

std::vector<Transaction> Mempool::selectTransactionsForBlock(
    size_t /*max_block_size*/,
    uint64_t /*max_block_weight*/,
    uint32_t /*current_height*/) const {
    return {};
}

void Mempool::excludeFromBlockTemplates(const uint256& /*txid*/,
                                        const std::string& /*reason*/,
                                        std::chrono::seconds /*duration*/) {
    // no-op
}

uint64_t Mempool::computeVWUForTx(const Transaction&) const {
    return 0;
}

}  // namespace dinero

// ─── Vault runtime — legitimate isolation seam ─────────────────────────────
// 2 vault observer stubs. See the Vault entry in the file header for why
// these stay stubbed even though vault_runtime.cpp lives in dinero_core.

namespace dinero::vault {

void NotifyVaultTipConnected(uint64_t /*height*/) {
    // no-op: real impl in vault_runtime.cpp drives the VaultService
}

void ObserveWalletOutput(const std::array<uint8_t, 32>& /*txid_raw*/,
                         uint32_t /*vout*/,
                         const std::vector<uint8_t>& /*script_pub_key*/,
                         uint64_t /*amount_una*/,
                         uint64_t /*height*/,
                         const std::string& /*block_hash_hex*/) {
    // no-op: real impl in vault_runtime.cpp records UTXOs for the vault ledger
}

}  // namespace dinero::vault

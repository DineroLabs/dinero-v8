// =============================================================================
// TEST-ONLY ISOLATION SEAM — vault observer no-ops
// =============================================================================
//
// vault_runtime.cpp is already compiled into dinero_core, but pulling
// vault_runtime.obj drags the full VaultService cascade (Ledger +
// DepositFlow + WithdrawalQueue + SigningBackend + RPC handlers +
// daemon_context) into every test link. This file provides no-op vault
// observer stubs for tests that link the real mempool.cpp (so they
// must NOT also pick up daemon_test_stubs.cpp's Mempool extras, which
// would cause LNK2005 multiply-defined symbols).
//
// Tests that don't link the real mempool.cpp can pick up
// daemon_test_stubs.cpp (which contains BOTH vault AND mempool extras
// stubs) instead. The reason for splitting them: composability.
//
// See tests/stubs/daemon_test_stubs.cpp header for the broader story.

#include "vault/vault_runtime.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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

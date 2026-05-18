// =============================================================================
// TEST-ONLY ISOLATION SEAM — not for production
// =============================================================================
//
// Several test executables reference Mempool methods that are DEFINED in
// src/daemon/mempool.cpp. mempool.cpp is compiled directly into dinerod,
// NOT into any linkable library — so test executables that aren't dinerod
// itself fail to link with LNK2019.
//
// This file provides no-op definitions so consensus/p2p tests that don't
// exercise mempool BEHAVIOUR can link and run. Tests that DO exercise
// mempool semantics (fee estimation, RBF, eviction, ancestor tracking)
// MUST NOT pick up these stubs — they will silently produce wrong results.
//
// Mempool is an INTENTIONAL isolation seam (verified during the architectural
// cleanup that extracted dinero_bridge and dinero_pool). src/daemon/mempool.cpp
// is 3349 lines with 17 deep includes spanning RPC, mining policy, shielded
// validation, fee estimation, RBF policy, silent-payment scanning, P2P
// message types, and the chain layer. Extracting it into a dinero_mempool
// static library would just move the cascade — the real architectural
// improvement is to replace the hard link-time dep with a narrow injectable
// IMempool interface. See tests/stubs/daemon_test_stubs.cpp file header for
// the broader story and the Vault parallel.
//
// Adding new stubs here without addressing the IMempool interface is a smell.

#include "daemon/mempool.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "consensus/outpoint.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dinero {

// Iteration primitive — referenced from compact_block.cpp (in
// dinero_core). Tests using compact_block under test fixtures simply
// iterate over an empty mempool, which is a valid mempool state.
void Mempool::forEachEntry(
    const std::function<void(const MempoolEntry&)>& /*fn*/) const {
    // empty mempool — no entries to iterate
}

// Block-connection notifications — referenced from chainstate_service.cpp.
size_t Mempool::onBlockConnected(
    const Block& /*block*/,
    uint32_t /*height*/,
    const std::vector<uint8_t>& /*block_hash*/) {
    return 0;
}

void Mempool::onBlockDisconnected(
    const Block& /*block*/,
    uint32_t /*height*/) {
    // no-op
}

std::vector<uint256> Mempool::selectStaleForRefresh(
    uint32_t /*chain_height*/,
    size_t /*max_refresh_batch*/,
    uint32_t /*max_proof_age_blocks*/,
    uint32_t /*max_refresh_attempts*/,
    size_t /*stale_overload_threshold*/) {
    return {};
}

size_t Mempool::ReconcileAfterReorg(
    const std::vector<Transaction>& /*old_chain*/,
    const std::vector<Transaction>& /*new_chain*/) {
    return 0;
}

bool Mempool::isOutputSpentInMempool(const OutPoint& /*outpoint*/) const {
    return false;
}

}  // namespace dinero

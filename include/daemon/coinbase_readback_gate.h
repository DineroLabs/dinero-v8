// Copyright (c) 2026 The Dinero Developers
//
// Maturity gate for the coinbase output-0 UTXO read-back performed by
// ChainstateService::CheckBlockDisconnectMaterialDurable.
//
// The read-back asserts that a block's coinbase output 0 is present in
// chaindb. That assertion is valid ONLY while the coinbase is still
// immature: consensus forbids spending a coinbase until
// `coinbase_maturity` confirmations, so an immature coinbase output 0
// cannot have been spent and MUST be readable. Once mature, output 0 may
// be legitimately spent (gettxout == null) — normal chain state, NOT an
// atomicity failure. Running the read-back on a mature/spent coinbase is
// what produced the false fleet-wide chainstate_recovery.marker (e.g. the
// height=35600 marker observed on LA/CN/SJ) during the 1024-block startup
// undo audit.
//
// Pure + header-only so the boundary is unit-testable without standing up
// a live ChainstateService (see tests/daemon/test_undo_audit_coinbase_gate.cpp).

#pragma once

#include <cstdint>

namespace dinero::daemon {

// Returns true iff the coinbase output-0 read-back applies for a block at
// `height`, given the chain tip the caller is reasoning from
// (`reference_tip_height`) and the network's `coinbase_maturity`.
//
// Confirmations = reference_tip_height - height + 1. A coinbase becomes
// spendable only AFTER `coinbase_maturity` confirmations, i.e. it is
// mature (and so possibly spent) once (tip - height) >= maturity. While
// immature ((tip - height) < maturity) output 0 is guaranteed unspent and
// the read-back is a valid durability assertion.
//
// `reference_tip_height < height` (tip behind the block) returns true to
// stay conservative — keep checking rather than silently skip. The
// post-commit ConnectTip caller passes reference_tip_height == height
// (depth 0, always immature), so the atomicity invariant it enforces is
// always exercised; only the deep startup-audit walk-back can skip.
inline bool CoinbaseReadbackApplies(uint32_t reference_tip_height,
                                    uint32_t height,
                                    uint32_t coinbase_maturity) {
    if (reference_tip_height < height) {
        return true;
    }
    return (reference_tip_height - height) < coinbase_maturity;
}

}  // namespace dinero::daemon

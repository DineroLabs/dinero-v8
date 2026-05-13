// =============================================================================
// TEST-ONLY ISOLATION SEAM — not for production
// =============================================================================
//
// `dinero_consensus_core` (output: dinero-consensus.lib) is the "pure"
// consensus library — no DB, no networking, no threading. It includes
// utreexo_accumulator.cpp and consensus_utxo_set.cpp, both of which pull
// in include/consensus/utreexo_canonical_roots_activation.h. That header
// has an inline IsUtreexoCanonicalRootsActive() that calls
// dinero::GetActiveChain(), causing each .obj inside the core library to
// emit a reference to GetActiveChain(). The real definition lives in
// src/consensus/chainparams_impl.cpp, which is compiled into the broader
// dinero_consensus library — not dinero_consensus_core.
//
// test_consensus_core_standalone (and any other test deliberately scoped
// to dinero_consensus_core only) links neither dinero_consensus nor
// chainparams_impl.cpp, so GetActiveChain is unresolved at link time.
//
// This stub returns Chain::MAINNET so those tests can link and run.
// Tests that actually exercise canonical-roots activation behaviour on
// testnet or regtest MUST NOT pick up this stub.
//
// Architectural cleanup options considered and deferred (June 2026 audit):
//
//   A. Add a Chain parameter to IsUtreexoCanonicalRootsActive +
//      GetUtreexoCanonicalRootsActivationHeight. Audited: cascades into
//      9+ callsite updates including ConsensusUTXOSet::Restore() (4
//      production + 2 test callers), UtreexoForest::cloneForHeight() (2
//      callers), and inline wrappers in formal_invariants.h and
//      dinero_consensus.h. Touches consensus paths — non-trivial risk
//      for the ROI of deleting one stub. Skipped for now.
//
//   B. Move chain-selector state (g_paramsSelected + g_active +
//      GetActiveChain) out of chainparams_impl.cpp into a tiny TU that
//      dinero_consensus_core compiles. Conflicts with the existing
//      single-definition of GetActiveChain in chainparams_impl.cpp and
//      complicates SelectParams flow.
//
//   C. Make GetActiveChain a function-pointer hook installed by
//      chainparams_impl.cpp at startup, defaulting to MAINNET inside
//      dinero_consensus_core. This is the same shape as the
//      vault_observer / IMempool extraction pattern documented in
//      tests/stubs/daemon_test_stubs.cpp. Cleanest long-term answer;
//      tracked as a future refactor.
//
// In the meantime, this 8-line stub is the right cost.

#include "consensus/chainparams.h"

namespace dinero {

Chain GetActiveChain() {
    return Chain::MAINNET;
}

}  // namespace dinero

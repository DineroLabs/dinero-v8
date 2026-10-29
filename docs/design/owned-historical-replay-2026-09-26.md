# Owned historical replay (2026-09-26)

## Actual production path

`AssumeUtxoReplayEngine`, used by `ChainstateService::BackgroundValidationWorker`,
now requires the exact selected genesis before connecting height one. It checks
that genesis contains the selected network's exact serialized coinbase and
header identity, then seeds the same ordinary coins as genesis initialization
without adding forest leaves. Duplicate seeding and changed network identity
refuse. Every subsequent block must have the next height, exact supplied hash,
actual parent tip, and a non-mutated transaction Merkle tree.

The existing header selector is owned by this isolated replay. Header version,
time, difficulty and PoW rules use the existing selected-network policy. The
selector has no persistent store. BlockValidator receives a construction-time
ancestry callback that resolves contextual-lock MTP from this owner, bound to
its last successfully connected parent. Missing callback results never fall
back to the process-wide block index. Ordinary validators retain their existing
lookup when no callback was supplied. Both stateful and stateless lock checks
use the supplied owner when present; the replay itself remains stateful.

Header acceptance precedes the existing full transaction/UTXO/forest and legacy
shielded validation. A header may remain in the isolated selector if its body
fails, but it does not advance the applied tip or authorize time-lock ancestry.
The production worker still discards a failed consensus replay before its
confirmation retry. Missing bodies remain an incomplete replay, not progress
certification. Genesis mismatch refuses the worker's initialization.

## Scope and qualification

The declared replay target exercises seeded digest equivalence, real root
validation failure, undo-tail retention, genesis/identity/parent/Merkle refusal,
header refusal before coin effects and contextual time-lock rejection using
owned ancestry with no global block-index entries. The time-lock fixture tests
non-final spends, not successful signed spend/broadcast. Synthetic regtest
headers use the existing regtest PoW policy; they do not qualify mainnet PoW or
an independently mined complete historical chain. An additional required CI
lane verifies the registration and actual execution of all eight replay cases.

Local qualification also ran the existing actual-daemon replay script with
`RUN_SCENARIOS=AD`: a 102-block isolated regtest source exported 307 coins;
a peerless consumer stalled across restart, fetched the complete history via
P2P, promoted it, and restarted with the strict archival audit passing and no
safe mode. The source included an actual legacy shielded transaction. This is
the A/D subset, not the complete script, Orchard activation or mainnet history.
The normal daemon build and both declared CTests passed (eight replay cases and
39 existing validation invariants). All 201 linked project C++ translation
units in the replay executable were freshly ASan/UBSan instrumented; its map
contains no remaining project C++ archive members. Four copied-source omission
controls failed their intended regressions, followed by a restored pass. Rust
and external libraries were uninstrumented and macOS leak detection was off.
The daemon A/D run used the normal build, not a fully instrumented daemon.
The separate ARM RocksDB dependency qualification remains open.

This is a prerequisite for a selected immutable historical wallet source. It
creates no wallet baseline, delivery receipt or provider. The outputs still
alias the engine and require its lifetime and exclusive ownership. Selected
branch capture/recheck, historical CT compatibility, complete wallet history,
script/key/account discovery, transparent adoption, arbitrary-length recovery,
activation history and whole-node startup/reindex remain separate obligations.
Existing header rules, including historical difficulty behavior, are reused;
this change does not redefine consensus or certify every historical profile.

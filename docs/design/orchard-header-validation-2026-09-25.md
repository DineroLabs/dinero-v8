# Staged Orchard contextual header gate

`CheckOrchardHeaderUnderChainstateLock` implements the contextual header portion
of the mixed-block connector. It is not called by production admission yet and
does not enable Orchard or choose a mainnet activation/branch identifier.

The host supplies the selected, already authenticated parent, block context,
header selector and current validation clock while holding its chain/writer
lock. Network parameters must remain fixed through application. Parent metadata,
MTP, block-one reference and the 60-second boundary anchor are copied from the
candidate's own hash-anchored branch under the selector's internal locks. No
selector-owned pointer escapes, and the best-header branch is never substituted
for the selected parent. Eviction between reads fails locally; it cannot silently
fall back to active-chain difficulty or zero/uncomputed expected bits.

Checks cover candidate/parent hashes and height, activation context, signing
network/genesis/nonzero branch, version/reserved fields, MTP, the shared two-hour
future-time bound, exact shared ASERT difficulty and actual proof of work.
Configured checkpoints at or below the candidate height must match the candidate
or its own hash-anchored parent ancestry. Missing ancestry or malformed configured
hashes are local errors. Future checkpoints do not block earlier sync prefixes;
ordinary regtest bypasses neither checkpoint nor context checks.
Time arithmetic checks the unsigned difference before conversion to signed ASERT
time. Ordinary regtest keeps its explicit existing PoW/ASERT bypass, while the
enforce-PoW qualification profile exercises both checks across the timing boundary.

`OrchardHeaderLookupError` means missing ancestry or inconsistent local
configuration, never a peer-invalid result. `TimeTooNew` is temporary: an eventual
caller must retry it as time advances, not mark that header permanently invalid.
The gate does not mutate the header selector or persistent/memory chainstate.

The selected parent remains a host trust boundary. This gate is not
historical-chain authentication, full block validity, or final branch
selection. A production adapter must run it before expensive body authorization
and apply the same locked context through the existing atomic staging operation.
Remaining obligations include consistent live checkpoint/fork-choice routing,
final resource-profile qualification,
body/transaction validation, selected state and durable commit. A nonzero branch
identifier alone is not a registered production Orchard consensus profile.

`OrchardHeader` tests real solved isolated-regtest headers, heights 1–6 across
the 60-second boundary, a competing branch with a different boundary anchor,
MTP/future-time boundaries, malformed framing/context and missing local ancestry.
The test verifies that checking a header does not advance best-header selection.
Its CTest name and Orchard label are mandatory in the root Linux CI inventory.

Checkpoint tests cover the exact height, later descendants, a competing branch
whose checkpoint differs from the best-header branch, ignored future heights,
and malformed local configuration. This does not qualify the runtime deep-reorg
path: it must invoke this gate on every candidate, including replay/reindex.

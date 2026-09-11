# Contextual transaction locks at 111,000

Mainnet activation is 111,000, selected by the operator on 2026-09-10.
Historical blocks below that height retain their existing acceptance rules.
Testnet remains dormant; regtest defaults to 111,000 and supports the strictly
regtest-only --consensus-contextual-locks-height override for boundary tests.

The shared checker enforces absolute height/time finality and version-2-or-later
relative height/time locks. It uses candidate height and branch median time,
never wall-clock time. Unconfirmed parents have candidate height; unknown coin
metadata cannot masquerade as genesis. Arithmetic widens before adding delays.

Live stateful connection, mempool admission/revalidation/template selection and
reindex use the checker. Stateless connection verifies absolute locks and relative
locks for maturity-bound v2 leaves. Mainnet v2 creation-height commitments already
activate at 60,000, so newly funded timelocks have authenticated creation ages.
Legacy v1 creation ages remain unverified in stateless mode and are explicitly
reported as deferred; fully validating nodes enforce those inputs from their
UTXO history. This is not a claim of independent legacy-age verification.

Core funding refuses timelock requests before activation. Qt and iOS require an
explicit contextual_locks_active capability from an upgraded connected node;
absence of the field disables funding. Existing proof formats are unchanged.

## Evidence

ContextualLocksTest covers historical acceptance, activation boundary, absolute
finality, sequence disable/version bits, time locks, missing ages and overflow.
Block-validation tests reject premature stateful and authenticated stateless
spends before script checking and preserve UTXOs after rejection. All 37 tests
in the block-validation invariant suite passed locally.

ContextualLocksLifecycle runs real two-node RPC funding, batch payments, premature
relative/absolute rejection, maturity acceptance, relay/mining, invalidation and
reconsideration of relative funding, reindex and restart tip identity. Registered
runs passed locally in approximately 75 seconds. CI remains required.

The read-only tools/audit/contextual_locks.py scan runs against a pinned canonical
tip and resolves input creation heights before classifying historical violations.
Its full report and fleet upgrade qualification are release gates; an in-progress
scan or an empty partial result is not evidence of a completed historical audit.

# Orchard transaction checks against a selected parent

Orchard transaction eligibility previously lived inside block-state preparation,
which also requires a real candidate block hash. The shared checks now take an
`OrchardTransactionContext` with the selected parent, candidate height,
activation height and explicit signing domain. No future block hash is needed.
The actual `PrepareOrchardStateTransition` calls these checks, then binds their
result to its required real block identity. It still rejects null and
parent-equal block hashes.

The shared implementation retains the existing rules: active profile, exact
parent height/hash/frontier/root/balance, complete authorization context,
ordered transaction/input/nullifier uniqueness, selected-history anchors,
checked nullifier lookups, per-transaction pool balance and resource limits.
Storage failures remain distinct from invalid transactions. Its immutable
result owns the projected frontier, balance, nullifiers, flows and fees. It
performs no database writes or live publication.

This is a prerequisite for typed mempool admission. It is not an admission or
reservation result: callers still need selected-chain and pool ownership,
complete ordered candidate membership, current coin resolution, policy,
shared transparent conflicts, persistence, selection, relay and readmission.
Empty sequences do not prove historical completeness. The activation-parent
history gate and production notification-provider requirement remain in force.
Mainnet activation remains unset.

Three new benign component cases compare shared checks with actual block-state
preparation, retain parent/context gates including empty sequences, and verify
conflict/lookup refusal and retry without altering an earlier result. Existing
state-transition fixtures are unchanged. Fresh backend-on full daemon and 11 declared test targets passed 15 selected
CTests. Fresh backend-off full daemon and two targets passed both selected
CTests. All nine linked project C++ translation units were freshly built with
ASan/UBSan; all three cases passed with 1,265 stable source/header/fixture inputs.
External dependencies, Rust, C/PQClean, the daemon and backend-off binaries are
outside that instrumented graph; macOS LSan is disabled. No original-source or
synchronization-removal control was run.

CI also exposed an outdated exact target count in the real-mempool consumer
lane after `RpcPoolOwner` was added. The workflow now requires the actual 20
backend-on targets; all original 19 remain. Every generated target was freshly
built and its tests executed: 20 targets/28 CTests on and 18 targets/20 CTests
off, all passing. This retains the missing-target, disabled-test and complete
execution guards. All 164 prior Orchard CTest commands remain unchanged.
The new native inventory contains 12 tests, and the root build adds a separate
required lane while preserving its existing selectors. Exact Linux
qualification remains pending.

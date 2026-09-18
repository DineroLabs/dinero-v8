# NodeCore maintenance ownership: read-only qualification stage

Status: native qualification prototype, disabled by default. This implements the
ownership part of the maintenance contract without enabling a reset plan. The
reset allowlist and preservation rules for embedded wallets are not settled.

## Scope and API

`DINERO_NODECORE_MAINTENANCE_QUALIFICATION=ON` enables exactly one plan,
`inspect-unchanged-v1`, on native Darwin/Linux builds with `BUILD_NODECORE=ON`.
Cross builds cannot enable it. Normal builds reject Begin/Resume/Finish, while
Start still refuses unresolved maintenance records. An older unguarded binary
can bypass this check; binary/datadir pairing remains a deployment prerequisite.

- Begin serializes with Start/Stop, closes the real worker and DaemonApp, hashes
  the closed directory, persists prepared intent and returns an opaque token.
- The token binds a process nonce, generation and operation ID. A competing
  Start/Begin receives busy. Ordinary Stop does not release ownership.
- Finish with outcome zero recomputes the inventory and requires the same target
  identity and prepared record. The caller cannot assert that contents match.
- Finish with outcome one retires the token but keeps prepared intent. Explicit
  Resume validates the recorded operation and returns a fresh process token.
- Wrong, stale and duplicate tokens do not change ownership. Lifecycle calls
  from the event callback return a reentry error before taking the lifecycle
  mutex, avoiding a worker-join deadlock.
- Status is diagnostic. It contains the operation ID, never a permission token.

The single process mutex is held only during synchronous core calls, never
across a Swift await. No filesystem operation, deletion callback, reset,
checkpoint removal, snapshot replacement or CF migration is provided here.
Required import anchors and verified reconstruction remain mandatory.

## Journal and verification boundary

The journal is in `.nodecore-maintenance-v1` beside the datadir. Startup reads it
before creating or opening the datadir. Existing path aliases resolve to the
same parent; prepared intent blocks other targets under that parent too.
Unknown, partial, unreadable, symlinked or malformed control files fail closed.
Records bind parent/target inode and device, canonical target, plan, phase,
operation and inventory digest. IDs and records are bounded.

Prepared/completed records use an exclusive staging file, file fsync, atomic
rename and directory fsync. Completion receipts certify a transition; they do
not demand an unchanged chain after subsequent normal operation resumes.
JSON integer representations are normalized after parsing so small positive
device/inode values compare consistently with their in-memory UInt64 values.

The inspection inventory is deliberately bounded to 64 MiB and 4,096 entries
for synthetic qualification directories. It hashes paths, file types/modes and
regular-file contents. Symlinks and special files are rejected. It is not a
filesystem snapshot, an adversarial-writer exclusion mechanism, or a scalable
inventory scheme for production chain databases.

## Qualification

The integration driver calls the production C ABI and real DaemonApp. The
ownership scenario uses offline regtest datadirs and verifies unchanged Utreexo
and shielded roots. It covers:

- another datadir cannot stop the current node;
- Begin waits at the real worker shutdown callback; a racing Start then gets
  busy, and callback Start/Stop/Begin reentry returns without deadlock;
- wrong/stale tokens and ordinary Stop preserve ownership;
- changed contents cannot be certified as unchanged;
- an actual directory permission failure blocks completion and keeps ownership;
- uncertainty and abrupt process death with an active token both survive into
  a fresh process, require explicit recovery, and reject the old token;
- malformed/partial/symlink/unknown-version records prevent startup before a
  missing datadir can be created;
- the existing lifecycle and snapshot-consumer tests continue to exercise real
  proof generation/verification and root preservation through restart.

The exact tests are `NodeCoreMaintenanceGate` (all NodeCore builds) and
`NodeCoreMaintenanceOwnership` (qualification flag only). Both have an explicit
Linux Tests lane. The normal-build test also verifies that no inspection token
can be obtained. Native qualification does not establish iOS file locking or
mobile resource behavior.

Native macOS arm64 result (2026-09-18): all four suites passed after restoring
both negative controls, 105 checks in 70.29 seconds. A separate default-OFF
rebuild passed 21 startup-gate/capability checks. Disabling token equality fails
at the wrong-token assertion; bypassing the actual Start journal gate fails at
fresh-process recovery exclusion. Both mutants compile, fail behaviorally and
are restored. Execution-map parser self-tests and the assertion ratchet pass.
Linux qualification for this prototype remains separate from #775's runtime
foundation; the latter already passed its 42 runtime checks on Linux.

### Wallet lifetime regression exposed by Linux qualification

Run 35319654885 on `329d0553d` passed lifecycle, journal-gate and ownership
checks, but the snapshot scenario ended while creating the next node's default
wallet. The artifact's last line was the successful wallet-policy write. The
original artifact did not include the controller traceback or exit status, so
it does not establish a signal or stack trace.

Source inspection found that mnemonic/descriptor imports assigned a raw global
ChainDB pointer. Completed NodeCore shutdown destroyed that database without
clearing the pointer; the next default-wallet creation dereferenced it to read
the wallet birthday. A deterministic regression observes the non-null pointer
after a real import and completed shutdown without dereferencing freed memory.

Wallet handlers now receive the owning context's ChainDB for synchronous rescan
and its height for wallet creation. They never publish the database globally.
The snapshot test also checks a wallet birthday at height 138 and a fresh node's
birthday at height 0. Restoring the old global assignment fails the lifetime
check; replacing the supplied birthday with zero fails the height check. Both
negative controls are restored before qualification. These changes establish
and fix the dangling-pointer defect; a fresh Linux run remains required to
confirm the observed CI failure is resolved.

CI artifacts now include each CTest invocation's output and any controller
traceback, including a subprocess return code when it exits before replying.

Restored follow-up qualification: all four NodeCore suites and ten wallet
readiness tests passed on macOS arm64 (14/14, 93.64 seconds). The new snapshot
checks retain the original root/proof/restart assertions. Workflow parser,
actual execution-map coverage and assertion-ratchet checks passed; CTest output
capture was exercised locally. Fresh Linux qualification remains pending.

## Unfinished gates: no destructive rollout

1. Define the reset allowlist, protected wallets/import anchors and typed outcome
   integration with Swift. Test cancellation through actual worker completion.
2. Complete the I/O fault and power-loss matrix, including every write, rename
   and sync boundary. Current write-error testing fails before publication. In
   particular, a completed rename followed by failed directory fsync has an
   uncertain durability outcome: the process keeps its lease, but a new process
   may observe the verified completed receipt. Do not equate the current test
   with proof that every possible I/O error preserves a restart barrier.
3. Qualify path replacement, real closure failures and abnormal worker exit.
   Existing inode checks do not fence raw filesystem tools or another process.
4. Decide the cross-process/mobile owner enforcement and binary rollback policy;
   qualify real Swift publication/expiry paths and iPhone memory/sync behavior.
5. Enable a destructive plan only after its own operation-specific crash-safe
   validator and permissions have been reviewed. CF separation is a later stage.

No phone reset, seed deployment, production database opening or consensus-rule
change is part of this prototype.

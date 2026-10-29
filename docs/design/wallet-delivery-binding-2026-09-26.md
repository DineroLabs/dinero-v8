# Persistent wallet binding for index recovery

## Production entry points

`RuntimeIndexDelivery::ReadForWallet` and `ApplyForWallet` acquire the actual
WalletManager database lease and check the caller's expected process session
before touching either store. They obtain a persistent database identity and
hold the lease through the existing index read or effect/progress transaction.
The caller captures the intended session, releases that initial lease, obtains
checked canonical source material, and then calls the consumer. A switch or
same-name reopen during that handoff refuses the old session. Recovery after
reopen captures a new live session and uses the retained persistent identity.
Process sessions must not be persisted as durable identity certificates.

The former caller-supplied string entry points are private; a narrowly named
test adapter exercises their lower-level consistency checks. Production callers
must use the leased wallet entry points. Two wallet databases with the same
display name receive different bindings. The active wallet cannot be replaced
while this consumer waits for the index or applies its effects.

`DatabaseLease::EnsureDeliveryIdentity` lazily adds a 32-byte random
`wallet_meta.runtime_delivery_id` under checked `BEGIN IMMEDIATE`. It requires a
selected wallet, the lease's owning thread and autocommit at entry, selects and
verifies `synchronous=FULL`, checks the existing metadata row/schema and validates
an existing nonzero binary ID. Schema creation, ID assignment and checked COMMIT
share one transaction. SQL or COMMIT failure rolls back owned work; a still-active
failed rollback terminates. Borrowed transactions remain unchanged. The returned
`DNWI01:` hexadecimal string is allocated before COMMIT, with no fallible
publication diagnostics afterwards. Reopen and metadata changes preserve it.
Both public index methods may initialize this binding, including the first read.

The identity transaction completes before the separate index operation. A failed
index operation can leave an initialized identity; it cannot thereby acknowledge
index effects. This does not make the two databases atomic. Normal wallet open
and creation do not eagerly initialize this extension. A backup that includes
the identity retains the same database binding. The ID is not authentication
against arbitrary database edits or a certificate of keys or ownership.

## Qualification

Fresh declared CMake targets and the full daemon build pass. Ten selected CTest
registrations pass, with the strengthened binding and index lanes rerun after
final changes. Two independent 30-second binding registrations test real wallets,
reopen, a fresh WalletManager instance, changed display metadata, different
wallet databases, empty selection, schema rollback, checked write and deferred
COMMIT rejection, borrowed transactions and malformed IDs. Failure assertions
require the intended error stage. The actual legacy rename API is not qualified
by these metadata tests.

The expanded `OrchardIndexDelivery` lane calls the public consumer with a real
WalletManager and real index. It checks source application/replay, same-name
reopen, stale sessions, another database with the same name, and an actual wallet
switch while the consumer waits on the index. Existing generated-store
historical/Orchard boundary and index failure tests remain. These are component
and concurrency fixtures, not a running node or independently validated history.
The workflow explicitly requires the two binding lanes and their actual internal
executions. Both root selectors retain 45 enabled Orchard registrations; not all
45 ran locally. The outbox's `Block` forward declaration now agrees with its
`struct` definition; no Windows build is claimed by that declaration correction.

All 113 linked project C++ translation units of the expanded index executable
were freshly instrumented with ASan/UBSan; the changed test translation unit
was rebuilt after strengthening the lease-acquisition probe. Four copied-source
controls omit persistence, substitute the display name, omit the session check
or release the lease early. Each fails its intended regression and the restored
source passes. The first lease control survived a test that timed full wallet
opening; the final test observes competing lease acquisition directly. Its final
map contains no project C++
archive members. Rust and external libraries are uninstrumented; macOS leak
detection is off. This does not resolve the separate open ARM dependency
sanitizer gate. The two SQLite failure registrations run normally, outside that
executable's sanitizer scope. Local inherited version labels and prebuilt
external dependencies are not release-binary provenance. No fresh-process,
physical power-loss, whole-wallet recovery or production activation is claimed.

## Remaining owner obligations

The supplied event still must come from the selected service's checked canonical
reader. The caller must establish that the watched scripts and pre-origin index
baseline belong to the intended wallet and selected chain; the persistent ID
does not certify either. The identity binding solves a store association and
lifetime obligation, not complete baseline validation or account ownership.
Existing index receipts, canonical delivery logs, whole-reorg intent and
encrypted account receipts must be reused by the complete recovery owner.
Ordinary wallet, note and account progress, ordered partial-commit replay,
late-account/rescan handling and all-consumer readiness remain unfinished.
No production RuntimeBlockNotifications provider is installed, and the first
activation boundary still requires independently validated history.

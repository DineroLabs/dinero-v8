# Catalog-owned Orchard account creation

Locally qualified at the combined final source for the component scope described below. Fresh full declared targets built ON/OFF;75 ON and70 OFF selected CTests passed, plus the enabled OrchardRuntimeReader CTest. All linked project C++ freshly ASan/UBSan:162 replay,122 wallet and35 state-machine cases, plus a separate complete DaemonApp graph with five ordinary process modes. Exact source/map/input and executed-case evidence is retained privately. Nine normal link-regression binaries and enabled reader are outside sanitizer graphs; external/Rust/C/PQClean uninstrumented and macLSanoff. No whole-node, transport, crash-durability, complete send RPC or release claim; mainnet unset.

`wallet.orchard.createaccount` accepts an object with exactly one previously
unused account number. It captures the actual checked source under source/wallet
lifetime ownership before acquiring SQLite or seed ownership. It uses the same
strict parsing, selected-session binding and backend availability boundary as
the existing receiving-address method. Missing/unset/CSN source refuses.

A checked FULL transaction authenticates the initial generated catalog under the
existing identity and seed. Recovered/legacy-unknown or missing catalogs refuse.
Only a genuinely empty authenticated catalog may initialize absent Orchard
schema; it cannot contain stray retained history. Populated catalogs require
existing schema. Every current account is fully restored at its recorded source
prefix, every reached operation archive is authenticated, and all rows must be
claimed. Catalog numbers and profiles must exactly match that inventory. Present
retained envelopes authenticate under their exact owners; recorded predecessor
links are followed with full restoration. Missing/corrupt/foreign owners refuse.

The owner creates an account from the immutable source's activation origin,
validates that initial scan against the source, issues its first external
receiver, and atomically inserts the encrypted snapshot plus a new catalog entry.
The catalog compare-and-set checks its authenticated revision and exact previous
encoding. No existing number can be reused, even when its account row is missing.
Only after checked COMMIT does the handler return the address and account revision.
It explicitly reports synchronization required. No cursor, event, selected tip,
spend readiness, or transparent baseline is fabricated.

## Scope and qualification

Seven written ON cases/two OFF cover strict registered RPC refusal, actual
nonconsecutive creation/reopen/issuance, first-schema/catalog/snapshot/COMMIT
rollback, borrowed/stale/domain/read refusal, deleted/foreign/corrupt retained
owners, recovered unknown and pre-catalog rows, and actual real-proof shield
replay and canonical undo under the newly created owner. Source and fixtures
were prepared together; no original-red, build or pass claim exists yet. Existing
fixture/assertion bodies remain unchanged. Extracted present-account enumeration
keeps its prior semantics for existing callers.

This creation boundary reconciles its catalog and current/reached owners. Other
existing read/replay/issuance entrypoints have not yet been universally converted
to require catalog completeness; legacy inventory reconciliation remains open.
Authentication cannot detect rollback of an entire valid backup or deletion of
unreferenced historical retained snapshots. Missing recorded parents do refuse.
The 1024-account/65536-row/predecessor limits are operational refusal bounds,
not load/resident-memory qualification. No nonempty pending-operation archive
fixture or new authenticated network transport is claimed by these cases.

Fresh full ON/OFF builds, actual component/RPC-handler tests, complete linked
project sanitizer graphs and independent private evidence verification remain
required. Full release, all-consumer recovery, pending/proving/relay wallet host,
whole-node restart/reindex/reorg and remaining platform/load gates are separate.
Mainnet stays unset. No production/client/Swift/deployment or unsafe race controls.

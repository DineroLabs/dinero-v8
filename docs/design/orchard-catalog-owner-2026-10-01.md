# First-seed Orchard catalog ownership

Locally qualified at the combined final source for the component scope described below. Fresh full declared targets built ON/OFF;75 ON and70 OFF selected CTests passed, plus the enabled OrchardRuntimeReader CTest. All linked project C++ freshly ASan/UBSan:162 replay,122 wallet and35 state-machine cases, plus a separate complete DaemonApp graph with five ordinary process modes. Exact source/map/input and executed-case evidence is retained privately. Nine normal link-regression binaries and enabled reader are outside sanitizer graphs; external/Rust/C/PQClean uninstrumented and macLSanoff. No whole-node, transport, crash-durability, complete send RPC or release claim; mainnet unset.

Genuine generated and recovered wallet creation now includes an account catalog
in the same checked FULL SQLite transaction as the first seed and sealed initial
owner. Generated creation starts with an authenticated empty account list;
mnemonic recovery explicitly records inventory unknown. A recovered identity,
missing legacy catalog or empty database never permits historical recreation.

The canonical DNOAC01 record authenticates the persistent wallet identity, mode,
revision and bounded account/profile entries using domain-separated HMAC-SHA256
under the exact 64-byte wallet seed. It contains no seed or private key. Reads
require an existing caller transaction, exact SQL TEXT/byte lengths, canonical
hex, a valid tag, identity, bounded fields and terminal SQLITE_DONE. No read
writes a record or commits a borrowed transaction.

The first-seed sealed owner uses DNI02 to require the new catalog at unlock.
DNI01 remains accepted with its original master and legacy absence semantics.
Every present catalog is authenticated before staged unlock publication, even
when a legacy initial-owner record is absent. Ordinary seed rewrites cannot
orphan an existing catalog. Same-seed persistence leaves catalog bytes intact.
No PQ master regeneration, KDF migration, live cache or source cursor is added.

## Qualification scope and remaining account lifecycle

Seven written cases cover generated/recovered creation and encrypted reopen,
corrupt/missing/type/seed failures with locked and already-unlocked preservation,
identity and authenticated mode binding, actual first seed/catalog/owner write
and COMMIT rollback, checked read/EOF and caller transaction preservation, and
DNI01/same-seed compatibility, and authenticated canonical payload bounds. Initial source and cases were prepared together;
there is no original-red or execution claim. Existing test bodies and deadlines
remain unchanged. The new earlier seed-orphan guard may reject a changed-seed
request before older injected write faults; the new first-seed cases exercise
actual required writes and observe actual COMMIT refusal independently.

This batch establishes initial catalog ownership only. It does not yet allocate
accounts, reconcile all encrypted account/archive rows, certify legacy inventory,
or implement the account-creation RPC. Existing test-only account enrollment is
not catalog-backed. Those actual catalog transitions and owner checks are next;
no caller may advertise global account completeness from this record alone.
Deleting both a catalog and its new sealed owner can leave legacy-unknown state;
this must never authorize recreation. A complete valid older wallet backup can
also authenticate: local MACs cannot certify freshness or prevent backup rollback.

Fresh full ON/OFF builds, actual component tests, complete linked project
sanitizer graphs and independent private evidence verification remain required.
Mainnet stays unset. No production/client/Swift/deployment changes, unsafe race
controls, release readiness, or whole-node qualification are included.

# Checked master seed persistence

`WalletManager::storeMasterSeed` now holds the existing database lease and a
checked FULL transaction across its seed write, encryption metadata, and the
existing replacement path's address-state and recovery-setting writes. Required
prepare, bind, step, affected-row, and commit results are checked. The two tables
that were optional in older schemas are skipped only after a checked schema read
confirms absence. Caller transactions are refused without committing or rolling
back their work.

The incoming and prior live seeds remain in scoped, cleansed buffers. Loading the
prior seed for the existing same-seed comparison does not publish it. The live
seed is swapped only after commit succeeds; failed required writes leave the
previous live seed and durable rows intact. The KDF key buffers and cipher context
are released on exceptions as well as normal returns. The established v2
PBKDF2/AES-GCM seed envelope is unchanged.

## Qualification

Three `WalletSeedWrite` component cases exercise required seed/metadata failure,
commit refusal and a caller-owned transaction, and rollback of the existing
replacement path followed by same-seed reopen. They use real WalletManager,
SQLite, encryption, and address issuance. All three initial cases failed the
predecessor writer before the production edit. Additional deletion and recovery
setting refusal assertions were added afterward; those individual assertions do
not have an original-source-before-edit claim. The independent Orchard workflow
requires all three exact passing case markers and retains inventory and logs.

## Limits

This is a persistence prerequisite, not an initialization-versus-recovery
certificate. The existing replacement decision and successful replacement policy
remain; this change does not authorize replacing an established identity or
certify complete key, script, account, or backup discovery. Creation's later
mnemonic and registry writes and `storeUnencryptedWallet`'s subsequent metadata
writes are outside this transaction. No whole-create/whole-restore atomicity is
claimed. Unlock still needs staged publication and explicit initialization
ownership. Missing P2MR storage cannot authorize master regeneration.

No PQ master generation or recovery, historical-address relabeling, new account,
receipt, journal, readiness reset, production activation, or notification provider
is introduced. Existing historical import and Orchard archive qualifications
retain their prior scope. Process-crash, power-loss, full dependency sanitizer,
whole-node Orchard lifecycle, and release gates remain open.

The additional existing restore-over-encrypted-wallet case remains an open
qualification gate. The same case also fails with the predecessor wallet manager
in the unchanged fixture. Its policy check has not been removed or weakened;
this persistence change does not repair the outer restore owner's policy and
key transition. Raw diagnostics and provenance remain in the private ledger.

Fresh local ON/OFF daemon and declared wallet component builds passed. The
selected wallet CTests passed (8 ON, 6 OFF), including all three seed-write cases.
Three additional existing mnemonic/restore CTests were executed in each
configuration: two passed and the restore-over-encrypted case failed as described
above. The existing restore fixture uses mempool stubs; these are component
checks, not transport, admission, broadcast, or full-process restore validation.

All 89 linked project C++ translation units were freshly rebuilt with ASan/UBSan;
43 cases across 12 wallet suites passed with 1,307 source/header hashes unchanged.
The normal discovery link map includes project archives to identify the graph;
the instrumented map contains no project C++ archive members. External, Rust,
C/PQClean code is uninstrumented, and macOS leak detection is off. The daemon,
OFF binaries, and separate restore component are outside this instrumented graph.
The full ARM RocksDB instrumentation gate remains open.

Copied predecessor-writer, unchecked-metadata-step, and early-publication controls
failed their intended named assertions without sanitizer diagnostics. Restoring
the production implementation passed all three new cases. An initial control
that omitted metadata entirely also broke fixture setup; it is retained privately
and is not counted as a successful control. Production assertions were unchanged.

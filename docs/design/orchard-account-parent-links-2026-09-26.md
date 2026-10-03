# Authenticated account parent links

The bound account consumer now records the prior snapshot revision inside the
same encrypted account payload as its scan and delivery receipt. A typed connect
uses the actual expected revision whose envelope is retained by
`StageReplaceRetaining`. Disconnect obtains its locator from that authenticated
payload, authenticates the retained parent and checks its immediate-parent scan.
It then carries the parent's link forward while preserving the current issued
addresses, pending signed bytes and operation archive. Callers no longer choose
a parent revision.

Revision order is checked at both reads: the current link is strictly less than
the current snapshot revision, and a retained parent's link is strictly less than
that parent's revision. Non-chain account changes can advance the revision while
preserving this link; subtracting one from the current revision is incorrect.
No chain-height-to-revision guess or independent mapping journal is used.

## Encoding and compatibility

An account with a nonzero link encodes DNORAC06: the DNORAC05 layout followed by
an eight-byte little-endian revision. Version 06 requires a nonzero link, an
existing delivery receipt and an activated scan. Versions 01–05 still decode with
no invented link. A zero-link account retains the existing version 05 encoding.
The locator is protected by the existing snapshot AEAD, including its wallet,
network, genesis, account and storage-revision binding.

Old snapshots can still be read. Automatic typed disconnect refuses an absent
link rather than adopting a caller-provided revision. Missing retained history
also refuses. Explicit baseline reconciliation is still required for those
accounts. Raw scan advancement/rewind and rescan reset clear storage links;
address and pending-operation updates preserve them. Historical delivery below
activation has no typed scan parent link.

## Qualification and limits

The actual bound consumer regression uses real WalletManager key/identity
ownership and generated Orchard note/spend bodies. It verifies a non-chain
intervening revision, checked SQL failure, reopen, two consecutive typed undos,
legacy missing-link refusal, authenticated self-link refusal and preservation of
Ready bytes/addresses. These are wallet-effect fixtures, not independently
validated historical consensus or a running node.

This change supplies automatic parent locators for the bound account consumer.
It does not install the consumer in the recovery coordinator or production
notification provider. Immutable selected-chain replay views, multi-store
account reconciliation, baseline/late-account recovery, other consumers and
activation/startup qualification remain required. Retained revisions are unpruned;
no storage-growth/load or physical power-loss qualification is claimed.

## Linux link correction

The preceding account-owner run 36245636079 failed before root tests: GNU ld saw
`dinero_chainstate` before the newly used WalletManager object and could not
resolve its ChainDB/BlockStorage calls. The component dependency list now places
`dinero_wallet` before the chainstate provider. This corrects archive order
without test stubs, disabled checks or altered production behavior. Local macOS
link success alone does not qualify GNU ld; a fresh Linux run is required.

Local qualification used a fresh declared CMake build of the full daemon and
account/archive targets. Three distinct CTests passed; the strengthened bound
lane was rebuilt and rerun after the link change. The runtime-reader-off service
translation unit compiled. Both unchanged workflow selectors retain exactly 46
enabled root registrations; all 46 were not run locally.

All 86 project C++ translation units in the bound executable were freshly
ASan/UBSan instrumented, with the final test revision rebuilt in the same run.
The final map contains no project C++ archive members. The unchanged previously
instrumented RocksDB histogram object was reused; other external libraries and
Rust were uninstrumented, and macOS leak detection was off. The independent ARM
RocksDB checksum alignment qualification remains open. Four copied-source
controls (missing connect link, guessing revision minus one, dropping the parent's
link on undo, and allowing a self/future revision) failed their intended checks;
the restored executable passed. No original-source test-first claim is made.

The initial build was interrupted to schedule Rust and RocksDB separately before
the four-job C++ build. The initial control verifier expected an account-level
error for the guessed revision; the actual rejection was the earlier scan
checkpoint mismatch. The verifier was corrected to that specific rejection;
production checks and test behavior were unchanged. Version labels are inherited
from aea and the external OpenSSL dependency is prebuilt; these are not release
binaries or release-provenance qualification.

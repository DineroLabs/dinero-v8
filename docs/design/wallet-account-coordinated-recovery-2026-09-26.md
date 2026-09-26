# Coordinated Orchard account recovery

## Behavior

`RuntimeWalletRecovery::ResumeWalletStores` obtains a service-owned immutable
`RuntimeAccountReplay` before acquiring wallet ownership. It coordinates the
existing index, ordinary-wallet and encrypted Orchard-account receipts. It
creates no additional journal, identity or progress record.

All three stores must already be enrolled. Missing or invalidated transparent
progress and an account without applied source progress refuse. Initial
baseline validation, late accounts and rescan reconciliation remain separate
obligations. The public adapter is compiled into the daemon dependency graph;
it is not yet installed in notification or startup routing.

The source capture starts at the actual retained activation-boundary connection.
Older records without replay frames and an origin after that boundary refuse.
It consumes every checked page, including histories longer than 128 records,
and validates the captured head at EOF. The current operational ceiling is
2,048 records and 64 MiB of charged source material. Exceeding either fails
before wallet effects; it never truncates a backlog or reports readiness.
These are not consensus limits or resident-memory bounds. General long-chain
recovery and load qualification remain open.

The service releases its activation lock after copying checked source material.
Expensive authorization verification then uses only those owned copies. The
result describes that captured prefix even if the node advances during proof
verification; later recovery source checks still precede wallet effects.

## Immutable branch views

Each typed connection reconstructs ordered inputs from the retained canonical
coin undo and earlier outputs in the exact block. External undo identities
must be consumed exactly; duplicate spends, creation conflicts, unsupported
confidential inputs/outputs and mismatched net-created identities refuse.
Orchard transparent signatures, bundle authorization, maturity and locks are
reverified. Timestamp lookups use retained answers from that actual canonical
validation; a missing answer is not guessed.

The view reconstructs anchor and nullifier membership on the specific parent
branch and prepares the sealed Orchard transition again. Its resulting
checkpoint must equal the retained next checkpoint. Repeated connections and
disconnects must agree on body, checkpoint, undo and captured timing answers.
Historical records below activation derive only an empty wallet scan tip.

Restore callbacks own immutable source data. Note origins and pending-operation
observations must be exact ancestors of the requested cursor's branch. An
off-branch block or missing earlier history refuses. Nullifier lookup walks
that branch, not the daemon's later active tip. No callback acquires chain
locks while a wallet lease is held.

This reconstructs wallet replay material from an already selected source. It
does not independently validate pre-origin UTXO completeness, historical
unspentness, ordinary scripts, header work, snapshot provenance or complete
block consensus. Local source checksums are not adversarial database
authentication. Independent activation-history qualification remains required.

## Account ownership and commit order

`OrchardAccountDelivery::ReadForReplay` authenticates the existing encrypted
snapshot under the pinned actual wallet identity and recovery seed. It uses
the authenticated receipt only to choose an immutable source position, then
fully restores the account against that position before returning it. There
is no metadata-only spendable account or cursor setter.

The coordinator validates both transparent cursors and the account cursor,
including an ahead store and EOF. It rereads all three stores under one wallet
lease before each event and refuses concurrent receipt or account revision
changes. Only lagging stores apply the event, in index, ordinary-wallet,
Orchard-account order. Each commits actual effects with its existing receipt.
The account uses the authenticated retained-parent revision for typed undo.
Issued addresses and pending/Ready operation data retain their existing
rollback semantics.

An exception leaves earlier committed prefixes available for retry. Reopen
requires the new live wallet session while retaining persistent identity.
The final result reports the applied captured prefix, account revision and
observed current source head. It does not publish wallet height, imply lasting
readiness or acknowledge vault/mempool/proof-cache/relay/oracle consumers.

## Qualification scope

Tests use actual checked indexed source records, real WalletManager and SQLite
stores, and a freshly proven 5,000-unit note owned by the integration wallet.
They cover typed rollback/reconnect, historical transitions across more than
one page, note balance restoration, retained parent selection after intervening
revisions, address preservation, immutable old views, source cursor mismatch,
concurrent account changes, stale session and caller-lease refusal.

An injected account write failure occurs after both transparent stores commit.
The account receipt remains unchanged; reopen and ordered retry recover only
its missing work. Generated source bodies and explicitly seeded wallet
baselines are not independently validated historical consensus or a running
node. Full provider, startup, baseline and platform qualification remain open.

Local qualification builds the declared integration/service targets and full
daemon. Seven selected CTests pass. The actual service source API is exercised;
the three-store coordinator regression uses a private seam over the actual
checked reader, not a running daemon or the public coordinator adapter.
All 121 linked project C++ translation units of the integration executable were
freshly instrumented with ASan/UBSan; four affected units were refreshed after
the final source/lock-scope refinement. Rust and external libraries remain
uninstrumented, macOS leak detection is off, and the service adapter is outside
that sanitizer executable. Copied omission controls cover branch nullifiers,
branch identity, account revision recheck and account effects; restored code
must pass. Default runtime-reader-off service compilation is checked separately.
Inherited version labels and prebuilt dependencies are not release provenance.
The generated timing fixture currently exercises empty MTP answers; delayed
timestamp-lock replay, physical power loss, running-node recovery and loaded
long histories require further qualification.

# Wallet recovery failure handling and confirmed activity

The phone incident exposed a distinction between a selected snapshot chain and the durable archival tip read by the legacy wallet scanner. These changes do not implement stateless snapshot wallet recovery or authorize a local wallet takeover.

## Changes

- A requested legacy restore/rescan checks source availability before changing the active wallet or seed. A snapshot source returns `recovery_required`; explicit `rescan=false` remains key import, not a recovery receipt.
- Snapshot recovery checks the selected/durable source identity before accepting an old numeric completion marker or importing base coins. A mismatch preserves the existing wallet inventory instead of reviving base coins before an unavailable tail scan.
- A scan beginning above its available tip fails. It cannot certify an empty tail as recovered.
- An available legacy/owned historical scan writes confirmed activity in the same checked transaction as coins and progress. It identifies mined receipts, records owned spends net of change, preserves originating transaction metadata and labels, and keeps records outside the scan. Hidden-value output history is refused rather than invented. This does not establish complete key discovery or recover missing historical sources.

## Evidence

The added confirmed-history check fails on the previous scanner (zero history despite a restored coin). The source-range check fails with the previous successful-empty-range return. The restore preflight test fails without its check (active wallet changes before rescan failure). The old numeric-marker test fails without the service check (recovery reports success against the wrong source).

Both backend configurations build. Backend-off: six nearby CTest groups pass, including snapshot import, shielded startup, block connect/disconnect, replay and wallet rescan. A real-daemon WalletRestoreRescan test passes separately. Backend-on: AssumeUtxoReplay passes 12 cases; WalletCanonicalRecovery passes six cases. Receipt/spend/change amounts, mined classification, labels, reopen/retry and history-write rollback are exercised by the wallet rescan executable. These are local macOS results, not hosted CI or device recovery qualification.

## Remaining phone recovery

The selected stateless snapshot is not an archival historical source. Native recovery still needs source-bound snapshot ownership plus authenticated post-base history, durable atomic publication tied to wallet identity/watch scripts and the selected chain, restart/reorg handling, and separate transparent/legacy-shielded/Orchard completeness. Neither a bridge balance, equal heights, a numeric marker nor discovery candidates grant those receipts.

The older `codex/wallet-snapshot-lifecycle-commit-guard` lane contains source-certificate ideas, but still uses the legacy archival-tip scanner, older wallet ownership APIs and no v6 source support. It was reviewed selectively and not merged. The current iPhone continues using the app safeguard: local validation can run while RPC serves the working wallet. No native recovery reset or new SDK installation was performed in this step.

## Explicit pre-Orchard transparent recovery candidate

`wallet.recovertransparentfromsnapshot` repairs an existing named wallet, with
both its expected name and first receive address required. It does not create a
wallet, restore a phrase, change keys or grant automatic wallet authority.

The source verifies the retained v3/v4/v5 snapshot's transparent records against
all live leaves of its rebuilt forest and the selected base header commitment.
Owned pre-maturity-fork leaves additionally need their canonical creation
transaction: their old leaf hash does not bind creation height or coinbase
status. Hash-linked selected post-base bodies must all be available and match
their Merkle commitments. Missing data defers recovery before wallet effects.

The final source is checked again under snapshot/selected/lifecycle ownership.
The writer checks wallet session and watched scripts again, then replaces the
transparent inventory and rebuilds post-base activity in one wallet transaction.
Pending outgoing activity refuses reconciliation. Wallet index publication uses
the existing index-first / wallet-COMMIT ordering, not a cross-store transaction;
a failed wallet COMMIT leaves no wallet completion and a retry reconciles both
stores. Local block hints prevent queued covered connects from lowering the
recovered height or resurrecting immature-only presentation. Disconnect removes
hints from the fork height onward. These hints are idempotency data, not recovery
or chain-verification certificates.

This candidate is explicitly limited to transparent current coins and post-base
history before Orchard activation. It refuses v6 and an Orchard-active selected
source; it does not qualify older spent history, complete key discovery, shielded
note recovery or deep-reorg recovery. The explicit path does not run the legacy
shielded rescan consumer. The app must retain the working RPC authority until a
fresh native inventory comparison and its independent chain/quorum gates pass.

Bounds are operational refusal limits: 256 MiB snapshot input, one million coins
and forest positions, 128 MiB retained post-base body material, and 20,000 tail
blocks. Device memory and end-to-end recovery remain separate qualification.

Local tests cover complete source inclusion, corrupt/omitted records and bodies,
legacy metadata substitution, wrong wallet identity/session, pending outgoing
payments, required-history and deferred-COMMIT failures, retry/reopen, mined
classification, covered connects/disconnects, and an empty post-base tail. The
queued-connect regression fails without its guard: the old notification lowers
wallet height from 130 to 110 and mature balance from 100 DIN to zero.

Phone startup also suppresses the legacy height rewind/catch-up/index validation
when a retained snapshot recovery base exists. An uninitialized selected source
at genesis is not proof of a reorg. Its regression fails before the fix: startup
changes a recovered height of 130 to zero and removes the mature presentation.
With the fix, the same wallet height and mature balance survive startup while
explicit source recovery remains deferred.

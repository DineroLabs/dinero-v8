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

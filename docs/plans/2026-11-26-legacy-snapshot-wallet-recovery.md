# Older snapshot wallet recovery for the next desktop release

## Incident and boundary

The Windows Juliett node was imported from the H52,287 pre-Orchard snapshot. It
later validated and backfilled the chain, but its wallet index remained a v2
snapshot index. A wallet created on that source could not adopt a complete
ownership history and showed zero despite 127 confirmed mining payouts. The
v8.2.6 `NativeWalletServable` guard now refuses another new wallet rather than
creating one with a misleading zero balance. An upgrade install retains the
old `%APPDATA%\Dinero` data, so installing a newer binary does not change this
source class.

The current v3 offline index upgrade is for an existing **full archive**. It
rejects imported snapshot state. `--reindex-chainstate` rebuilds ChainDB but
does not rebuild the v2 wallet index, so neither command alone is a recovery.
The full-v6 bundled snapshot authenticates network state, not a prior wallet's
keys or ownership history. Moving a funded wallet to a new snapshot without a
complete scan can hide its earlier money.

## Two recovery routes

1. **No wallet material:** An offline daemon preflight checks the legacy
   snapshot marker and the wallet registry, wallet files and HD-key directory.
   The desktop offers a separate current-snapshot data directory on a proven
   empty result. It keeps the original directory intact. It stores the
   selected path and refuses to silently fall back if that path later goes
   missing. If the probe is unavailable or uncertain, it does not offer this
   route.
2. **Existing wallets or key files:** The desktop offers a *separate new
   wallet* only with an explicit acknowledgement that no old wallet or coins
   move to it. The original directory stays in place. A separate help button
   directs people with existing funds toward recovery, not a misleading empty
   balance. Actual recovery requires a full archive from genesis, *all* wallet
   material restored on a separate copy, a rescan of each wallet, and canonical
   balance and restart verification before any cutover. The manual Juliett
   recovery proved this route on one Windows machine. It is **not** yet a
   shipped automatic migration. The release must either provide and qualify a
   guided implementation or keep this case blocked with a recovery instruction;
   it must never present a fresh-snapshot switch as a funded-wallet repair.
   Settings provides a folder switch for reopening either the separate new
   wallet or preserved older data after quitting the app; the switch never
   moves wallet files.

## Release gates

- On Windows, macOS and Linux, a former H52,287 v2 index is recognized even
  after background validation has cleared `assumeutxo_active`. A current v6
  source and a true full archive are not offered the old-snapshot reset.
- An empty former-snapshot installation starts with the latest bundled v6
  snapshot from a distinct directory; the old chain and wallet data remain
  intact, including after a failed startup or reinstall. An app restart selects
  the same new directory.
- A registry row, an orphaned wallet DB, HD seed material, unknown registry
  schema, linked path, unreadable source or a running daemon all prevent the
  implicit empty classification. No wallet file is deleted or moved by the
  probe. Before adopting a running node after a folder switch, the desktop
  checks the authenticated RPC-reported data folder and refuses a mismatch.
- Funded-wallet recovery must be tested with a mined pre-base payout, multiple
  named wallets, locked restart, a moving tip, and interruption/retry. A
  successful balance display alone is not a complete recovery receipt.
- Exact-source CI on the release candidate, plus a Windows installer upgrade
  from an older snapshot installation. The new helper is not release-qualified
  by a local macOS build alone.

## Current implementation scope

The first branch adds read-only detection and a separate fresh-profile route
for empty installs and for users who explicitly choose to create a *new*
wallet while preserving older wallet files. It also adds the retained
`assumeutxo_lc_base_height` to the v3-upgrade refusal. It deliberately does not
mark funded old-snapshot nodes as converted.

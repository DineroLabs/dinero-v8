# Dinero v8.1.10

Dinero v8.1.10 is the mandatory consensus-readiness release for the coupled
CTV/CCV covenant profile scheduled at mainnet block 100,000. It also packages
the shielded, wallet recovery, liquidity-vault, covenant, QR, consolidation,
and cross-platform reliability work completed after v8.1.9.

## Consensus and covenant activation

- Activates CTV and CCV atomically at block 100,000.
- Keeps CSFS and TXHASH dormant.
- Adds exact 99,999/100,000 boundary, legacy-node, mempool, restart, rollback,
  and reactivation coverage.
- Allows covenant descriptor construction before activation while spend
  construction remains activation-gated.
- Adds a read-only fleet safety checker and a canonical CI readiness gate.

## Wallet and client reliability

- Restores persistent shielded anchor history across restart and reorg.
- Strengthens mnemonic import/rebinding and zero-watch-script recovery.
- Aligns shielded and covenant core behavior across Dinero-Qt, iOS, and Android.
- Adds consolidation single-flight/review protections and clears stale transient
  errors after recovery.
- Improves QR URI interoperability and centered round-logo rendering.
- Hardens liquidity-vault and contract signing paths, including removal of
  production placeholder authorization behavior.

## Release evidence

The activation candidate passed:

- 15/15 covenant readiness lanes on Apple Silicon and Linux x86_64.
- 16/16 mutation kills.
- funded CTV and CCV lifecycle, restart, reorg, and reconfirmation tests.
- a physical Mac/Dell/DineroTX funded relay and competing-fork rehearsal.
- Android ARM64/x86_64, iOS device/simulator, and Dinero-Qt builds.

Operators must upgrade before block 100,000. Verify signed artifact hashes and
the expected consensus checksum from the release readiness evidence before
starting the daemon.

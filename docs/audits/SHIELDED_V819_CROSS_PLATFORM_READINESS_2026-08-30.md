# Shielded v8.1.9 cross-platform readiness — 2026-08-30

## Outcome

The repeated-mnemonic/watch-script recovery and embedded-daemon lock fixes are
built and exercised across Qt, native iOS, and native Android. No wallet or
blockchain directory was deleted during validation.

## Proven builds

| Platform | Embedded core | App commit | Physical result |
|---|---|---|---|
| dinero-qt macOS | `ce1439708` plus diagnostics `9531bb443` | same repository | forced exit, helper cleanup, immediate daemon/RPC relaunch passed |
| DineroDPI iOS | XCFramework from `ce1439708` | `285ddf3` | iPhone kill/relaunch passed (PID 1772 → 1773) |
| DineroDPI Android | `bc453703` (Android lineage with equivalent fixes) | `4dc4fab` | Pixel force-stop/relaunch passed (PID 15649 → 15948) |

## Test evidence

- `DatadirGuard`: passed, including daemon→exec-helper→daemon-relaunch topology.
- `WalletRestoreRescan`: passed in 46.78 seconds.
- iOS: 594 tests in 89 suites passed with the rebuilt XCFramework.
- Android: arm64-v8a and x86_64 NodeCore builds and `assembleDebug` passed.
- Qt packaging: deep signature verification passed and macdeployqt no longer
  emits the transient nested-Brotli signature failure.
- Qt failure details now include `dinerod.lock`, PID-file state, and the current
  macOS lock owner PID/command. Unrelated `wallet.log` history is excluded.

## Physical funded-note gate

The connected Pixel was synced through block 98,856 and showed a transparent
balance, but its send screen reported **0 DIN / 0 shielded notes**. Consequently
the following tests remain `NOT RUN`, rather than being inferred from idle or
transparent-wallet behavior:

1. output/spend proving peak RSS and wall time;
2. an actual shielded spend and multi-note selection on each phone;
3. before/at/after activation-boundary sender/recipient behavior;
4. funded-note discovery after restart and a real reorg;
5. a complete camera QR shielded transfer between physical devices.

These require at least one confirmed shielded note plus a specified transfer
amount/destination. The current Android baseline after restart was 479,336 KiB
PSS / 618,472 KiB RSS; it is not a proof-memory measurement.

## Release assessment

The code-level recovery and process-lifecycle defects are fixed and locally
proven. Build, unit, restart, provenance, and signing gates pass. A production
shielded release decision must remain conditional until the five funded-note
device gates above have real evidence.

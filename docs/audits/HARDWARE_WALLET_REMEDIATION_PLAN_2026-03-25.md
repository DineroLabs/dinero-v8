# Hardware Wallet Remediation Plan

Date: 2026-03-25

Scope:
- `dinero` backend hardware wallet support
- `dinero-qt` hardware wallet frontend

## Audit Summary

The current hardware wallet stack is only partially production-ready:

- File-based PSBT export/import works and is the most complete flow.
- QR export works, but signed-QR re-import is still disabled in the Qt frontend.
- USB device enumeration exists, but the Qt tab overstates capability.
- The backend compile gating for USB support is inconsistent.
- Ledger and Trezor implementation depth is uneven, and the full GUI/RPC flow is not wired.

## P0: Truthfulness and Build Consistency

Goals:
- Make the build flags and runtime behavior consistent.
- Stop the GUI from claiming support that is not actually available.

Tasks:
1. Define `HAVE_USB_HWALLET` whenever `ENABLE_HARDWARE_WALLETS=ON` and the target actually builds USB HID support.
2. Replace the stale runtime error text referencing `ENABLE_USB_HWALLET`.
3. Rewrite the Qt USB tab copy:
   - no "fully supported" claims
   - no unconditional "USB support enabled" log lines
   - clearly state that the current Qt USB tab is detection-oriented and that file/QR PSBT workflows remain the reliable signing paths
4. Keep the disabled `Connect` button visibly unavailable until an actual connect/sign flow exists.

Acceptance:
- A build with `ENABLE_HARDWARE_WALLETS=ON` no longer falls into the "USB support not compiled" path because of a missing compile definition.
- A build without USB support does not present misleading "enabled" status in the GUI.

## P1: Ledger-First USB RPC and Qt Wiring

Goals:
- Make one real USB hardware wallet path work end-to-end.
- Prefer Ledger first because the codebase already has the deepest implementation.

Tasks:
1. Add RPC methods for:
   - `hwallet.connectdevice`
   - `hwallet.disconnectdevice`
   - `hwallet.getdeviceinfo`
   - `hwallet.getmasterfingerprint`
   - `hwallet.displayaddress`
   - `hwallet.signpsbt`
2. Expose only capabilities that are actually implemented for the selected device.
3. Wire Qt to:
   - enable connect after enumeration
   - show connected device info
   - sign a PSBT over USB
   - surface device-side rejection and firmware/app mismatch clearly
4. Route successful signed PSBT import back into the existing send/broadcast flow.

Acceptance:
- Ledger can be enumerated, connected, used to display an address, sign a PSBT, and return a signed PSBT through Qt.

## P2: Trezor Completion

Goals:
- Bring Trezor to the same baseline as Ledger.

Tasks:
1. Implement missing production paths:
   - address retrieval
   - on-device address verification
   - master fingerprint
   - PSBT protobuf signing call
2. Make Trezor capability reporting runtime-accurate.
3. Add Qt and RPC handling for Trezor-specific errors where needed.

Acceptance:
- Trezor path supports the same minimum flow as Ledger.

## P3: Remove Duplicate and Dead Hardware Wallet Stack

Goals:
- Eliminate parallel implementations that drift independently.

Tasks:
1. Remove or quarantine the old `src/wallet/hardware_wallet.cpp` stack if it is not part of the shipped build.
2. Remove or fix `src/wallet/hardware_wallet_manager.cpp` if it is intended to be live code.
3. Consolidate around one hardware wallet abstraction:
   - PSBT-first
   - transport-aware
   - RPC-exposed

Acceptance:
- There is one authoritative hardware wallet stack in-tree.

## P4: Test and Release Discipline

Goals:
- Replace "infrastructure ready" claims with meaningful coverage.

Tasks:
1. Add unit/regression coverage for:
   - compile gating
   - enumeration results by device type
   - master fingerprint extraction
   - multi-APDU/chunked PSBT signing
   - GUI state for unsupported/disabled USB builds
2. Add a physical-device smoke checklist for release candidates.
3. Stop labeling simulated file-copy tests as "production ready".

Acceptance:
- Release notes and docs match the verified feature set.

## Recommended Execution Order

1. P0 truthfulness and compile gating
2. P1 Ledger-first USB flow
3. P2 Trezor completion
4. P3 dead-stack cleanup
5. P4 test hardening and release cleanup

# iOS/macOS Sync Profile + Mining Full Implementation Checklist

Status: In Progress
Owner: Core + DineroDPI
Last Updated: 2026-02-22

## Objective
Implement one shared NodeCore codebase/XCFramework with strict platform behavior:
- iOS profile: Utreexo stateless/compact by default, no full-block sync path, no mining.
- macOS profile: full blocks/stateful by default, mining enabled.

This checklist is execution-grade and must be completed in order. Do not skip gates.

## Scope
- Core repo: `/Users/haydarevich/src/dinero`
- App repo: `/Users/haydarevich/src/apps/DineroDPI/DineroDPI`

## Non-Negotiable Invariants
- iOS profile must never emit `getdata(MSG_BLOCK)`.
- macOS profile must use full-block sync (`MSG_BLOCK`).
- Mining APIs must be hard-disabled for iOS profile in core.
- UI feature visibility must be driven by core capabilities/status, not assumptions.

---

## Phase 1: FFI Sync Profile Contract
- [x] Add `sync_profile` to NodeCore start config parser in `src/nodecore/nodecore_ffi.cpp`.
- [ ] Supported values:
  - [x] `ios_utreexo`
  - [x] `mac_fullblock`
- [x] Keep backward compatibility for `utreexo_stateless`.
- [x] Conflict policy implemented and documented (`sync_profile` wins over raw flag).
- [ ] Platform fallback when missing:
  - [x] iOS default `ios_utreexo`
  - [x] macOS default `mac_fullblock`
- [x] Expose resolved profile in `nodecore_status`.

Acceptance:
- [x] Unit test: missing profile resolves correctly by platform target.
- [x] Unit test: invalid profile returns deterministic error.
- [x] Unit test: profile/flag conflict resolves with profile precedence.

---

## Phase 2: Core Policy Enforcement at Startup
- [x] Add a single startup policy function in daemon startup flow (`ApplySyncProfilePolicy`).
- [ ] Enforce for `ios_utreexo`:
  - [x] `utreexo_stateless = true`
  - [x] full-block request mode disabled
  - [x] reduced peer/caches policy applied
- [ ] Enforce for `mac_fullblock`:
  - [x] `utreexo_stateless = false`
  - [x] full-block request mode enabled
  - [x] normal peer/caches policy applied
- [x] Add startup proof log line with: profile, stateless/stateful, request type, peer cap.

Acceptance:
- [ ] Startup logs show exactly one canonical proof line.
- [ ] Status endpoint returns same resolved values as log line.

---

## Phase 3: Remove Block Request-Type Leaks
- [x] Implement one helper for block request inventory selection in network layer.
- [x] Replace direct request-type conditionals with helper in all block-request call paths.
- [x] Replace hardcoded `MSG_BLOCK` in timeout/retry path (`src/daemon/network_manager.cpp:1685`).
- [x] Replace any direct request path leakage in `requestBlock` and scheduler callbacks.
- [x] Audit daemon for remaining direct `MSG_BLOCK` getdata requests outside allowed profile policy.

Acceptance:
- [ ] Test (iOS profile): zero `getdata(MSG_BLOCK)` emitted during initial sync + retry + recovery.
- [ ] Test (macOS profile): `getdata(MSG_BLOCK)` emitted as expected.
- [ ] Grep check: no unmanaged hardcoded `MSG_BLOCK` request path remains.

---

## Phase 4: Core Capabilities + Mining Hard Gates
- [x] Add `nodecore_capabilities` FFI endpoint.
- [ ] Capabilities include:
  - [x] `supports_local_mining`
  - [x] `supports_pool_mining`
- [x] Enforce mining rejection for iOS profile at core/RPC boundary.
- [x] Return explicit structured error code/message for unsupported mining calls.
- [x] Keep mining allowed for macOS full-block profile.

Acceptance:
- [x] iOS profile: mining RPC calls return unsupported error every time.
- [ ] macOS profile: mining RPC path functional.
- [ ] Capabilities endpoint reflects true policy and is stable across restarts.

---

## Phase 5: DineroDPI Runtime Wiring
- [x] Pass explicit `sync_profile` from app runtime config (`NodeKit`).
- [x] Read and use `nodecore_status` + `nodecore_capabilities` to drive UI feature state.
- [x] Remove/avoid UI-only assumptions for validation mode/mining availability.
- [x] Ensure Sync Cockpit labels reflect real runtime mode from core state.

Acceptance:
- [ ] iOS UI never advertises unsupported mining.
- [ ] Validation mode label matches actual profile/status values.

---

## Phase 6: Platform-Specific Mining Safety Policy
- [ ] Keep iOS mobile safety gates for any future allowed contribute mode.
- [ ] Add macOS-specific safety gate policy (no phone-only battery/charging hard requirement).
- [ ] Ensure gate policy selected by platform/profile and logged.

Acceptance:
- [ ] macOS mining not blocked by mobile-only checks.
- [ ] iOS remains conservative.

---

## Phase 7: XCFramework Packaging (Single Artifact, Multi-Platform)
- [ ] Build `NodeCore.xcframework` with slices:
  - [x] iOS arm64
  - [x] iOS simulator arm64
  - [ ] macOS arm64
  - [ ] macOS x86_64 (if supported target matrix requires it)
- [x] Replace app embedded framework artifact.
- [ ] Verify xcframework `Info.plist` includes both iOS + macOS libraries.

Acceptance:
- [ ] Xcode links correctly for iOS and macOS targets.
- [x] No missing symbol errors from profile/capability additions.

---

## Phase 8: DineroDPI macOS Target Enablement
- [ ] Add/verify native macOS target in DineroDPI project.
- [ ] Apply platform-specific feature visibility:
  - [ ] iOS: no mining controls
  - [ ] macOS: mining controls enabled
- [ ] Ensure shared codebase remains single-source with conditional UI only.

Acceptance:
- [ ] macOS app runs as native target (not iOS-on-Mac only fallback).
- [ ] Runtime status/profile correctly reports mac full-block mode.

---

## Phase 9: Automated Tests (Required)
- [ ] Core unit tests:
  - [x] profile parsing/defaulting/conflict behavior
  - [x] capabilities policy
- [ ] Network tests:
  - [ ] request type selection for initial, retry, direct request paths
- [ ] RPC tests:
  - [ ] mining rejection on iOS profile
  - [ ] mining allowed on mac profile
- [ ] App tests:
  - [ ] status/capability-driven UI gates

Acceptance:
- [ ] CI green for new tests.
- [ ] No regressions in existing wallet/sync tests.

---

## Phase 10: E2E Release Gate Matrix

### iOS (stateless)
- [ ] Fresh install: create/restore/send/receive flow passes.
- [ ] Sync/retry/reconnect shows zero `MSG_BLOCK` requests.
- [ ] Mining APIs/features unavailable.
- [ ] Wallet balances/history/address derivation remain correct.

### macOS (full-block)
- [ ] Full sync uses `MSG_BLOCK`.
- [ ] Mining start/stop works and status updates correctly.
- [ ] Coinbase maturity and wallet accounting correct.
- [ ] Reorg handling remains stable under full-block mode.

### Cross-device parity
- [ ] Same mnemonic => same derivation/address set and spendability.
- [ ] Send/receive interoperability across iOS/macOS/Qt passes.

Final Acceptance:
- [ ] All phases completed with evidence links (commits/tests/log excerpts).
- [ ] Release note includes profile enforcement and mining capability split.

---

## Evidence Log (fill during execution)
- Core commits:
  - [x] `7a082fe54` core: enforce sync profiles and capability-gated mining
  - [x] `c580dbd6a` core: hard-gate mining context RPCs by sync profile
  - [x] `d5d898f5b` core: add sync profile policy module and contract tests
- App commits:
  - [x] `60e7fa1` ios: consume sync profile + capabilities and refresh NodeCore xcframework
  - [x] `72a74f4` ios: refresh NodeCore.xcframework with profile mining gates
  - [x] `ced10e7` ios: refresh NodeCore.xcframework after sync profile policy update
- Test runs:
  - [x] `cmake --build /Users/haydarevich/src/dinero/build --target dinerod -j8` (pass)
  - [x] `./build_nodecore_xcframework.sh` (pass, iOS device + simulator slices)
  - [x] `xcodebuild -project /Users/haydarevich/src/apps/DineroDPI/DineroDPI/DineroDPI.xcodeproj -scheme DineroDPI -configuration Debug -destination 'generic/platform=iOS Simulator' build` (pass)
  - [x] `ctest -R "SyncProfilePolicy|MiningPolicyTripwires|MiningRestartPolicyTripwires|MiningCompletenessPolicyTripwires" --output-on-failure` (pass)
  - [x] `ctest -R "MiningPolicyTripwires|MiningRestartPolicyTripwires|MiningCompletenessPolicyTripwires" --output-on-failure` (pass)
  - [ ] Runtime smoke (ephemeral local daemon) currently flaky due RPC readiness race; deterministic policy tripwire tests pass.
- E2E captures:
  - [ ]

## Risks / Watch Items
- [ ] Any leftover hardcoded request path bypassing profile helper.
- [ ] UI mode labels drifting from core runtime truth.
- [ ] iOS-on-Mac runtime confusion vs true native mac target.
- [x] Mining RPC legacy handlers bypassing capability checks.

# Trustless Light-Client Shielded Scanning — M1 (Phase 1 receive) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Per `feedback_subagent_verify_commit`: parent must `git status` + `git log` after each subagent task to verify the commit actually landed; do NOT trust the subagent's "done" report alone.

**Goal:** Ship the iOS receive-side surface for trustless light-client shielded scanning per the spec at `docs/superpowers/specs/2026-05-26-trustless-light-client-shielded-design.md` (PR #159, merged `b288f5bc`). M1 = M2/M3 are out of scope (M2 = daemon shielded compact filter; M3 = spend with witness fetch).

**Scope of M1:**
- iOS-only changes (no daemon changes — M1 works against existing RPCs)
- Trial-decrypt shielded outputs in blocks-of-interest against local IVK
- Maintain a verified incremental commitment-tree frontier with snapshot lineage
- Local-only `ShieldedNoteStore` exposed to `WalletShieldedView` in thin-client mode
- Active wallet only (per locked spec decision — not all wallets in app)
- NO spend path (M3)

**Design decisions locked from spikes (2026-05-26):**

| Concern | Decision | Source |
|---|---|---|
| AEAD + HKDF port | Direct CryptoKit (`ChaChaPoly`, `HKDF<SHA256>`). NO pure-Swift fallback. | Spike #1 — byte parity verified |
| Poseidon-2 implementation | Pure Swift round structure delegating field ops to libsecp256k1's PUBLIC `secp256k1_ec_seckey_tweak_*` via existing bundled `secp256k1.xcframework`. NO daemon `.xcframework` wrap. NO libsecp256k1 fork. | Spike #2 — daemon's own `Scalar` uses the same public API; measured 24.77 µs/eval at -O3 |
| Performance budget on iPhone | ~30-50 µs per Poseidon eval projected; 220k hashes × ~40 µs ≈ 13 sec, well under spec's 22s first-sync budget | Spike #2 measured + Swift→C overhead estimate |
| `.xcframework` of daemon code | Deferred to M3 (Spartan prover needs it; M1 does not) | Spike #2 + spec line 88 |
| Snapshot retention invariant | `kSnapshotInterval = 256` blocks; `kSnapshotRetention = 8` entries. **Load-bearing for M3 spend.** Don't tune. | Spec lines 201-212 |
| Active-wallet-only scan | M1 only scans the wallet the user has open. Multi-wallet background scan is M3+. | Spec — locked M1 decision |

**Architecture:** All M1 code lives under `DineroDPI/DineroDPI/Core/Shielded/` (new subdirectory). Two new methods added to existing `Core/Crypto/Secp256k1.swift` (no new wrapper file). Integrates into existing `Core/Filters/FilterChainSync.swift` (transparent UTXO discovery path). Updates the guardrail at `Views/WalletView.swift:4890` to allow shielded **display** in bridge-RPC mode (spend still blocked — that's M3).

**Tech Stack:** Swift 5.9+, CryptoKit, SQLite (via existing wrapper used by `UTXOStore.swift`), the bundled `secp256k1.xcframework`. No SwiftPM dependencies added.

**Branch:** `feature/m1-shielded-lc-receive` off `DineroDPI` default. Worktree: `/private/tmp/dineroDPI-m1-shielded`.

**Signing:** All commits SSH-signed as `Dinero Labs <team@dinerolabs.org>`. The DineroDPI repo is not currently in the 6-repo signed list (`dinero.md`); first commit in this branch MUST configure per-repo signing per the established pattern.

**Daemon ground-truth pins (from spike artifacts):**
- `Poseidon(0, 0) = cb1411ec5c71cc7ece4da4b866599aaf8f50e0cbbd45f7f65c3c24d2130ef0db`
- `Poseidon(1, 0) = 7c6187d299f78c9cfe391869da5da9735f3280430be53eee8d26ca4d7fa31e87`
- 611-byte `EncryptedNote` container (vector 2 from `src/test/shielded_derivation_tests.cpp:413`): `94d6195348f85b3dfca0caeb9f0a3a398345542793088f5b960ba3a134494c7f…14b14a`
- HKDF info string: `DIN/v7/shielded/note`
- AEAD nonce: 12 zero bytes; AAD: 32-byte `epk`; output: `[ct:563][tag:16]`

For any vector the daemon doesn't yet pin in source (e.g. `IVK(seed)`, `ShieldedAddress(seed,j)`), generate one using `/tmp/poseidon_daemon_harness.cpp` shape by linking against `/Users/haydarevich/src/dinero/build/libdinero_zk.a` and pinning into `Core/Shielded/ShieldedTestVectors.swift`. Document generator commit per pin.

---

## File map

New iOS files (all under `DineroDPI/DineroDPI/`):

- Create: `Core/Crypto/Poseidon.swift` (~180 lines)
- Create: `Core/Shielded/ShieldedAEAD.swift` (~80 lines)
- Create: `Core/Shielded/ShieldedKeys.swift` (~150 lines)
- Create: `Core/Shielded/ShieldedAddress.swift` (~140 lines)
- Create: `Core/Shielded/ShieldedNote.swift` (~80 lines, plaintext + ciphertext value types)
- Create: `Core/Shielded/ShieldedNoteStore.swift` (~250 lines, SQLite-backed)
- Create: `Core/Shielded/CommitmentTree.swift` (~300 lines, incremental frontier + snapshot/reorg)
- Create: `Core/Shielded/AnchorVerifier.swift` (~70 lines)
- Create: `Core/Shielded/BlockParserShieldedExt.swift` (~100 lines, extension on existing BlockParser)
- Create: `Core/Shielded/ShieldedScanner.swift` (~280 lines, orchestrator)
- Create: `Core/Shielded/ShieldedTestVectors.swift` (~100 lines, KAT constants)

Existing files modified:

- Modify: `Core/Crypto/Secp256k1.swift` — add `seckeyTweakAdd` + `seckeyTweakMul` raw scalar wrappers (~30 LOC)
- Modify: `Core/Filters/FilterChainSync.swift` — call into `ShieldedScanner` per matched block; route reorg events
- Modify: `Views/WalletView.swift` line 4890 — update guardrail text + allow display in bridge-RPC mode (spend still blocked)

New test files (XCTest, under `DineroDPI/DineroDPITests/Shielded/`):

- Create: `Shielded/PoseidonTests.swift` (~90 lines, KAT + `measure {}` bench)
- Create: `Shielded/ShieldedAEADTests.swift` (~80 lines, against daemon 611-byte vector)
- Create: `Shielded/ShieldedKeysTests.swift` (~100 lines, against pinned IVK vectors)
- Create: `Shielded/ShieldedAddressTests.swift` (~70 lines, encode/decode round-trip + HRP enforcement)
- Create: `Shielded/CommitmentTreeTests.swift` (~180 lines, including snapshot+reorg invariants)
- Create: `Shielded/ShieldedScannerIntegrationTests.swift` (~150 lines, end-to-end scan vs scripted fixture blocks)

Total new + modified: ~2000 LOC iOS + ~1100 LOC tests = ~3100 LOC. Larger than the relay-hints plan (~450 LOC) — this is a full subsystem, not an incremental RPC.

---

## Pre-flight (before Task 1)

- [ ] **Worktree + branch setup**

```bash
cd /Users/haydarevich/src/apps/DineroDPI
git fetch origin
git worktree add /private/tmp/dineroDPI-m1-shielded -b feature/m1-shielded-lc-receive origin/<default-branch>
cd /private/tmp/dineroDPI-m1-shielded
```

(Use the actual default-branch name — verify via `git remote show origin | grep "HEAD branch"`.)

- [ ] **Per-repo signing config** (this repo is NOT in the 6 already-configured)

```bash
git config --local user.name "Dinero Labs"
git config --local user.email "team@dinerolabs.org"
git config --local user.signingkey ~/.ssh/id_ed25519_dinero_signing.pub
git config --local commit.gpgsign true
git config --local gpg.format ssh
git config --local --list | grep -E "^(user|commit|gpg)\."
```

After first commit, verify with `gh api repos/<owner>/DineroDPI/commits/<sha> --jq .commit.verification` that GitHub shows `verified: true, reason: "valid"`. If `unsigned`, abort and fix per `project_commit_signing` recipe.

- [ ] **Xcode project sanity**

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj -list 2>&1 | head -20
xcodebuild -project DineroDPI/DineroDPI.xcodeproj -scheme DineroDPI -showdestinations 2>&1 | head -10
```

Confirm the scheme builds at HEAD before adding any files.

---

## Task 1: secp256k1 scalar wrappers

**Files:**
- Modify: `Core/Crypto/Secp256k1.swift`

This is the foundation Poseidon needs. Adds two wrapper methods to the existing class; no new file.

- [ ] **Step 1: Read existing Secp256k1.swift** to confirm the class shape, error enum, and context-pointer pattern.

```bash
grep -nE "class Secp256k1|public func|throws|secp256k1_context|tweakFailed" \
  DineroDPI/DineroDPI/Core/Crypto/Secp256k1.swift
```

- [ ] **Step 2: Add the two wrapper methods** in the public section, matching the existing call style (Data in, Data out, throws Secp256k1Error).

```swift
/// Modular addition mod n (secp256k1 scalar field).
/// Mirrors daemon's Scalar::operator+ in src/zk/zkvm/scalar.cpp:65.
/// Returns Data(repeating: 0, count: 32) iff the sum is zero mod n
/// (matches daemon's explicit zero handling).
func seckeyTweakAdd(_ a: Data, _ b: Data) throws -> Data {
    guard a.count == 32, b.count == 32 else { throw Secp256k1Error.invalidInputLength }
    if a.allSatisfy({ $0 == 0 }) { return b }
    if b.allSatisfy({ $0 == 0 }) { return a }
    var result = a
    let ok = result.withUnsafeMutableBytes { rPtr -> Int32 in
        let rBase = rPtr.baseAddress!.assumingMemoryBound(to: UInt8.self)
        return b.withUnsafeBytes { bPtr -> Int32 in
            let bBase = bPtr.baseAddress!.assumingMemoryBound(to: UInt8.self)
            return secp256k1_ec_seckey_tweak_add(self.context, rBase, bBase)
        }
    }
    if ok != 1 { return Data(repeating: 0, count: 32) }
    return result
}

/// Modular multiplication mod n.
/// Mirrors daemon's Scalar::operator* in src/zk/zkvm/scalar.cpp:85.
/// Returns Data(repeating: 0, count: 32) iff either operand is zero.
func seckeyTweakMul(_ a: Data, _ b: Data) throws -> Data {
    guard a.count == 32, b.count == 32 else { throw Secp256k1Error.invalidInputLength }
    if a.allSatisfy({ $0 == 0 }) || b.allSatisfy({ $0 == 0 }) {
        return Data(repeating: 0, count: 32)
    }
    var result = a
    let ok = result.withUnsafeMutableBytes { rPtr -> Int32 in
        let rBase = rPtr.baseAddress!.assumingMemoryBound(to: UInt8.self)
        return b.withUnsafeBytes { bPtr -> Int32 in
            let bBase = bPtr.baseAddress!.assumingMemoryBound(to: UInt8.self)
            return secp256k1_ec_seckey_tweak_mul(self.context, rBase, bBase)
        }
    }
    if ok != 1 { return Data(repeating: 0, count: 32) }
    return result
}
```

(If the existing class spells `self.context` differently — e.g. `Secp256k1.sharedContext` — match the existing pattern.)

- [ ] **Step 3: Build to verify**

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj -scheme DineroDPI \
  -destination 'platform=macOS,arch=arm64' build 2>&1 | tail -15
```

Expected: clean build, no new warnings.

- [ ] **Step 4: Commit**

```bash
git add DineroDPI/DineroDPI/Core/Crypto/Secp256k1.swift
git commit -S -m "$(cat <<'EOF'
feat(crypto): expose seckeyTweakAdd/Mul scalar wrappers for Poseidon

Wraps the public secp256k1_ec_seckey_tweak_{add,mul} API already bundled
in DineroDPI/Libraries/secp256k1.xcframework. These are the same calls
the daemon's src/zk/zkvm/scalar.cpp uses for its Scalar arithmetic
(operator+ at line 65, operator* at line 85), so anything we build on
top — starting with Poseidon-2 in the next commit — is byte-equivalent
to the daemon by construction.

Zero handling mirrors the daemon: tweak_add/mul reject zero operands,
so we short-circuit those paths to return canonical zero/passthrough
before calling into libsecp256k1.

No new dependencies; uses the existing bundled xcframework.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Poseidon-2 + KAT vectors + bench

**Files:**
- Create: `Core/Crypto/Poseidon.swift`
- Create: `Core/Shielded/ShieldedTestVectors.swift`
- Create: `DineroDPITests/Shielded/PoseidonTests.swift`

This is the perf-gating task. If the on-device bench in Step 4 misses the per-eval target by >2×, halt and revisit the .xcframework escape hatch BEFORE writing more tasks on this foundation.

- [ ] **Step 1: Write `ShieldedTestVectors.swift` first** so tests have constants to import.

```swift
// Core/Shielded/ShieldedTestVectors.swift
//
// Daemon-generated known-answer vectors. Each pin documents:
//   - Source file/line in dinero-v8 that produces it
//   - Generator command (run against /Users/haydarevich/src/dinero/build/libdinero_zk.a)
//
// DO NOT regenerate without re-pinning here AND in the daemon's test
// file (or the cross-impl byte-parity contract breaks silently).

import Foundation

enum ShieldedTestVectors {
    // src/zk/zkvm/poseidon_gadget.cpp::poseidon2_bytes(zero, zero)
    // Generated 2026-05-26 via /tmp/poseidon_daemon_harness.cpp.
    static let poseidonZeroZero = Data(hex: "cb1411ec5c71cc7ece4da4b866599aaf8f50e0cbbd45f7f65c3c24d2130ef0db")

    // src/zk/zkvm/poseidon_gadget.cpp::poseidon2_bytes(BE(1), zero)
    static let poseidonOneZero = Data(hex: "7c6187d299f78c9cfe391869da5da9735f3280430be53eee8d26ca4d7fa31e87")

    // src/test/shielded_derivation_tests.cpp::PinnedHexVector2EncryptedNote
    // Inputs: deterministic recipient_d/pk_d/esk/rcm/value/memo (see test).
    // Output: 611-byte container [epk:32][ct:563][tag:16].
    static let encryptedNoteVector2 = Data(hex:
        "94d6195348f85b3dfca0caeb9f0a3a398345542793088f5b960ba3a134494c7f" +
        "bf48bb6d359922610ff31ceadd6872092a20e35c9aa1e82e666ef880af7450e8" +
        // … (paste the full 1222-hex-char string from
        //    src/test/shielded_derivation_tests.cpp:455-475)
        "14b14a"
    )
}

private extension Data {
    init(hex: String) {
        var bytes = [UInt8]()
        bytes.reserveCapacity(hex.count / 2)
        var idx = hex.startIndex
        while idx < hex.endIndex {
            let next = hex.index(idx, offsetBy: 2)
            bytes.append(UInt8(hex[idx..<next], radix: 16)!)
            idx = next
        }
        self.init(bytes)
    }
}
```

- [ ] **Step 2: Write `PoseidonTests.swift`** (failing test first per TDD)

```swift
// DineroDPITests/Shielded/PoseidonTests.swift
import XCTest
@testable import DineroDPI

final class PoseidonTests: XCTestCase {
    func test_poseidonZeroZero_matchesDaemon() throws {
        let zero = Data(repeating: 0, count: 32)
        let out = try Poseidon.hash(zero, zero)
        XCTAssertEqual(out, ShieldedTestVectors.poseidonZeroZero)
    }

    func test_poseidonOneZero_matchesDaemon() throws {
        var one = Data(repeating: 0, count: 32); one[31] = 1
        let zero = Data(repeating: 0, count: 32)
        let out = try Poseidon.hash(one, zero)
        XCTAssertEqual(out, ShieldedTestVectors.poseidonOneZero)
    }

    func test_perf_singleEvalUnder100us_onMac() throws {
        // Loose ceiling: we want < 100 µs/eval on Mac M-series. iPhone
        // bench in Task 9 is the binding gate; this is an early canary.
        let zero = Data(repeating: 0, count: 32)
        var acc = zero
        let iters = 1000
        let t0 = Date().timeIntervalSince1970
        for i in 0..<iters {
            var b = zero; b[31] = UInt8(i & 0xFF)
            acc = try Poseidon.hash(acc, b)
        }
        let elapsed = Date().timeIntervalSince1970 - t0
        let perEvalUs = elapsed * 1_000_000.0 / Double(iters)
        print("Poseidon perf: \(perEvalUs) µs/eval")
        XCTAssertLessThan(perEvalUs, 100.0,
            "Mac perf regression — expect ~30-50 µs/eval via libsecp256k1 wrappers")
    }
}
```

- [ ] **Step 3: Run to verify failure** (no Poseidon.swift yet)

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj -scheme DineroDPI \
  -destination 'platform=macOS,arch=arm64' \
  test -only-testing:DineroDPITests/PoseidonTests 2>&1 | tail -20
```

Expected: compile error — `Poseidon` undefined.

- [ ] **Step 4: Write `Poseidon.swift`**

```swift
// Core/Crypto/Poseidon.swift
//
// Poseidon-2 over the secp256k1 scalar field. Byte-equivalent to the
// daemon's poseidon2_bytes at src/zk/zkvm/poseidon_gadget.cpp.
//
// Parameters (locked by daemon ground truth; do NOT change):
//   State width t = 3 (capacity[0]=0, rate[1]=a, rate[2]=b)
//   S-box: x^5
//   Full rounds Rf = 8 (4 before partial, 4 after)
//   Partial rounds Rp = 56 (only state[0])
//   Total: 64 rounds
//   MDS: [[2,1,1],[1,2,1],[1,1,2]]
//   Round constants: SHA256("PoseidonC_secp256k1" || BE32(r) || BE32(e))
//                    with rejection-resampling if 0 or >= n
//   Output: state[1]
//
// Performance: ~30-50 µs/eval expected on iPhone (Spike #2 measured).

import CryptoKit
import Foundation

enum PoseidonError: Error {
    case wrongInputLength
}

enum Poseidon {
    private static let rf = 8
    private static let rp = 56
    private static let t = 3
    private static let totalRounds = rf + rp  // 64

    /// 32-byte big-endian representation of the secp256k1 group order
    /// (used only for rejection sampling in round-constant derivation).
    private static let n: Data = Data([
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
        0xBA,0xAE,0xDC,0xE6,0xAF,0x48,0xA0,0x3B,
        0xBF,0xD2,0x5E,0x8C,0xD0,0x36,0x41,0x41,
    ])

    /// Lazy-init round constants table.
    private static let rc: [[Data]] = {
        (0..<totalRounds).map { r in
            (0..<t).map { e in deriveRC(UInt32(r), UInt32(e)) }
        }
    }()

    /// Public API: hash two 32-byte BE scalars, return one 32-byte BE.
    static func hash(_ a: Data, _ b: Data) throws -> Data {
        guard a.count == 32, b.count == 32 else { throw PoseidonError.wrongInputLength }
        var state: [Data] = [Data(repeating: 0, count: 32), a, b]
        let secp = Secp256k1.shared  // adjust to whatever the existing singleton is

        // First 4 full rounds
        for r in 0..<(rf / 2) {
            try addRoundConstants(&state, r: r, secp: secp)
            try sboxAll(&state, secp: secp)
            try mds(&state, secp: secp)
        }
        // 56 partial rounds (S-box on state[0] only)
        let partStart = rf / 2
        for r in 0..<rp {
            try addRoundConstants(&state, r: partStart + r, secp: secp)
            state[0] = try sbox5(state[0], secp: secp)
            try mds(&state, secp: secp)
        }
        // Last 4 full rounds
        let full2Start = partStart + rp
        for r in 0..<(rf / 2) {
            try addRoundConstants(&state, r: full2Start + r, secp: secp)
            try sboxAll(&state, secp: secp)
            try mds(&state, secp: secp)
        }
        return state[1]
    }

    // MARK: – round helpers

    private static func sbox5(_ x: Data, secp: Secp256k1) throws -> Data {
        let x2 = try secp.seckeyTweakMul(x, x)
        let x4 = try secp.seckeyTweakMul(x2, x2)
        return try secp.seckeyTweakMul(x4, x)
    }

    private static func sboxAll(_ state: inout [Data], secp: Secp256k1) throws {
        for i in 0..<t { state[i] = try sbox5(state[i], secp: secp) }
    }

    /// MDS: M = [[2,1,1],[1,2,1],[1,1,2]] over Fp.
    /// 2·x is x+x (one tweak_add); avoids needing a mul-by-constant.
    private static func mds(_ state: inout [Data], secp: Secp256k1) throws {
        let s0 = state[0], s1 = state[1], s2 = state[2]
        let twoS0 = try secp.seckeyTweakAdd(s0, s0)
        let twoS1 = try secp.seckeyTweakAdd(s1, s1)
        let twoS2 = try secp.seckeyTweakAdd(s2, s2)
        let new0 = try secp.seckeyTweakAdd(try secp.seckeyTweakAdd(twoS0, s1), s2)
        let new1 = try secp.seckeyTweakAdd(try secp.seckeyTweakAdd(s0, twoS1), s2)
        let new2 = try secp.seckeyTweakAdd(try secp.seckeyTweakAdd(s0, s1), twoS2)
        state[0] = new0; state[1] = new1; state[2] = new2
    }

    private static func addRoundConstants(_ state: inout [Data], r: Int, secp: Secp256k1) throws {
        for e in 0..<t {
            state[e] = try secp.seckeyTweakAdd(state[e], rc[r][e])
        }
    }

    // MARK: – round constant derivation

    private static func deriveRC(_ r: UInt32, _ e: UInt32) -> Data {
        let tag = Data("PoseidonC_secp256k1".utf8)
        var msg = Data()
        msg.append(tag)
        msg.append(contentsOf: rBE(r))
        msg.append(contentsOf: rBE(e))
        var h = Data(SHA256.hash(data: msg))
        // Rejection sampling per daemon's Scalar::from_hash
        while !inRange(h) {
            h = Data(SHA256.hash(data: h))
        }
        return h
    }

    private static func rBE(_ v: UInt32) -> [UInt8] {
        return [UInt8((v >> 24) & 0xFF), UInt8((v >> 16) & 0xFF),
                UInt8((v >>  8) & 0xFF), UInt8( v        & 0xFF)]
    }

    /// True iff 1 ≤ value(h) < n.
    private static func inRange(_ h: Data) -> Bool {
        // Reject zero
        if h.allSatisfy({ $0 == 0 }) { return false }
        // Reject >= n via big-endian byte compare
        for i in 0..<32 {
            if h[i] < n[i] { return true }
            if h[i] > n[i] { return false }
        }
        return false  // equal to n
    }
}
```

- [ ] **Step 5: Run tests + bench output**

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj -scheme DineroDPI \
  -destination 'platform=macOS,arch=arm64' \
  test -only-testing:DineroDPITests/PoseidonTests 2>&1 | tail -25
```

Expected:
- `test_poseidonZeroZero_matchesDaemon` PASS
- `test_poseidonOneZero_matchesDaemon` PASS
- `test_perf_singleEvalUnder100us_onMac` PASS with logged time ~30-50 µs

**If KAT fails:** halt. Bug is almost certainly in `addRoundConstants` (sign of rejection sampling) or MDS `2·x` decomposition. Re-derive against `/tmp/poseidon_ref.py` (the Python reference is the trusted byte-level mirror).

**If perf fails (>100 µs):** halt. Switch to .xcframework escape hatch (track new task). Don't proceed to Task 3 on shaky foundation.

- [ ] **Step 6: Commit**

```bash
git add DineroDPI/DineroDPI/Core/Crypto/Poseidon.swift \
        DineroDPI/DineroDPI/Core/Shielded/ShieldedTestVectors.swift \
        DineroDPI/DineroDPITests/Shielded/PoseidonTests.swift
git commit -S -m "$(cat <<'EOF'
feat(crypto): Swift Poseidon-2 over secp256k1 scalar field (byte parity)

Pure-Swift round structure (Rf=8 full, Rp=56 partial, t=3, x^5 S-box,
MDS [[2,1,1],[1,2,1],[1,1,2]]) delegating field arithmetic to the
secp256k1.xcframework wrappers added in the previous commit. Output is
byte-equivalent to the daemon's poseidon2_bytes (src/zk/zkvm/
poseidon_gadget.cpp), verified by KAT against pinned vectors:

  Poseidon(0, 0) = cb1411ec…0ef0db
  Poseidon(1, 0) = 7c6187d2…7fa31e87

Round constants are derived deterministically via
SHA256("PoseidonC_secp256k1" || BE32(r) || BE32(e)) with rejection
resampling for results == 0 or >= n (mirrors Scalar::from_hash in
src/zk/zkvm/scalar.cpp:48-59).

Mac perf canary in PoseidonTests fires at 100 µs/eval; expected ~30-50.
Real iPhone perf gate is in Task 9 (XCTest measure block on device).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: AEAD primitives + 611-byte note KAT

**Files:**
- Create: `Core/Shielded/ShieldedAEAD.swift`
- Create: `DineroDPITests/Shielded/ShieldedAEADTests.swift`

Direct CryptoKit per Spike #1 — no pure-Swift fallback.

- [ ] **Step 1: Failing test**

```swift
// DineroDPITests/Shielded/ShieldedAEADTests.swift
import XCTest
import CryptoKit
@testable import DineroDPI

final class ShieldedAEADTests: XCTestCase {
    /// Verifies HKDF + ChaChaPoly produce the daemon's exact 611-byte
    /// container [epk:32 | ct:563 | tag:16] for the deterministic
    /// inputs in src/test/shielded_derivation_tests.cpp:413 vector 2.
    func test_encryptedNote_vector2_byteParity() throws {
        // (Implementer: extract sharedSecret + plaintext + epk from
        //  the vector 2 fixture in shielded_derivation_tests.cpp.
        //  These aren't currently exposed as hex pins in the daemon
        //  test file — generate via a one-off harness that prints
        //  the values, then pin into ShieldedTestVectors.swift.)
        let shared    = ShieldedTestVectors.vector2EcdhShared           // 32 bytes
        let epk       = ShieldedTestVectors.vector2Epk                  // 32 bytes
        let plaintext = ShieldedTestVectors.vector2NotePlaintext        // 563 bytes
        let expected  = ShieldedTestVectors.encryptedNoteVector2        // 611 bytes (epk || ct || tag)

        let key = ShieldedAEAD.hkdfDeriveKey(ikm: shared, salt: epk,
                                              info: "DIN/v7/shielded/note")
        let (ct, tag) = try ShieldedAEAD.seal(key: key, aad: epk, plaintext: plaintext)
        XCTAssertEqual(ct.count, 563)
        XCTAssertEqual(tag.count, 16)
        XCTAssertEqual(epk + ct + tag, expected)
    }

    func test_open_roundTrip() throws {
        let key       = SymmetricKey(size: .bits256)
        let aad       = Data(repeating: 0xAB, count: 32)
        let plaintext = Data(repeating: 0xCD, count: 563)
        let keyData   = key.withUnsafeBytes { Data($0) }
        let (ct, tag) = try ShieldedAEAD.seal(key: keyData, aad: aad, plaintext: plaintext)
        let opened    = try ShieldedAEAD.open(key: keyData, aad: aad, ciphertext: ct, tag: tag)
        XCTAssertEqual(opened, plaintext)
    }

    func test_open_failsOnTamper() throws {
        let key       = Data(repeating: 0x55, count: 32)
        let aad       = Data(repeating: 0xAB, count: 32)
        let plaintext = Data(repeating: 0xCD, count: 563)
        var (ct, tag) = try ShieldedAEAD.seal(key: key, aad: aad, plaintext: plaintext)
        ct[0] ^= 0x01
        XCTAssertThrowsError(try ShieldedAEAD.open(key: key, aad: aad, ciphertext: ct, tag: tag))
    }
}
```

- [ ] **Step 2: Implementation**

```swift
// Core/Shielded/ShieldedAEAD.swift
import CryptoKit
import Foundation

enum ShieldedAEADError: Error {
    case openFailed
}

enum ShieldedAEAD {
    /// HKDF-SHA256 (single 32-byte block). Mirrors daemon
    /// HkdfExtractAndExpand at src/wallet/shielded_derivation.cpp:325.
    static func hkdfDeriveKey(ikm: Data, salt: Data, info: String) -> Data {
        let key = HKDF<SHA256>.deriveKey(
            inputKeyMaterial: SymmetricKey(data: ikm),
            salt: salt,
            info: Data(info.utf8),
            outputByteCount: 32
        )
        return key.withUnsafeBytes { Data($0) }
    }

    /// ChaCha20-Poly1305 IETF, 12-byte zero nonce, AAD-authenticated.
    /// Mirrors AeadEncrypt at src/wallet/shielded_derivation.cpp:447.
    /// Returns (ciphertext, 16-byte tag).
    static func seal(key: Data, aad: Data, plaintext: Data) throws -> (ciphertext: Data, tag: Data) {
        let sym = SymmetricKey(data: key)
        let nonce = try ChaChaPoly.Nonce(data: Data(repeating: 0, count: 12))
        let sealed = try ChaChaPoly.seal(plaintext, using: sym, nonce: nonce, authenticating: aad)
        return (Data(sealed.ciphertext), Data(sealed.tag))
    }

    /// Mirrors AeadDecrypt at src/wallet/shielded_derivation.cpp:476.
    static func open(key: Data, aad: Data, ciphertext: Data, tag: Data) throws -> Data {
        let sym = SymmetricKey(data: key)
        let nonce = try ChaChaPoly.Nonce(data: Data(repeating: 0, count: 12))
        let sealed = try ChaChaPoly.SealedBox(nonce: nonce, ciphertext: ciphertext, tag: tag)
        do {
            return try ChaChaPoly.open(sealed, using: sym, authenticating: aad)
        } catch {
            throw ShieldedAEADError.openFailed
        }
    }
}
```

- [ ] **Step 3: Generate vector 2 input pins**

The daemon's existing PinnedHexVector2EncryptedNote only pins the OUTPUT. For Swift's AEAD-only test to be meaningful, we need the OPAQUE inputs (shared, epk, plaintext) pinned too. Generate them by extending `/tmp/poseidon_daemon_harness.cpp` to also call `EncryptNoteForRecipient` with the same fixture and print `shared`, `epk`, `plaintext` to hex. Paste into ShieldedTestVectors.swift.

```bash
# (Generate vectors; update ShieldedTestVectors.swift)
cat >> /tmp/poseidon_daemon_harness.cpp <<'CPPEOF'
// (Add code here that mirrors PinnedHexVector2EncryptedNote setup and
//  prints sharedSecret, epk, note.Serialize() to hex.)
CPPEOF
```

Then pin into the Swift test vectors file.

- [ ] **Step 4: Run tests**

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj -scheme DineroDPI \
  -destination 'platform=macOS,arch=arm64' \
  test -only-testing:DineroDPITests/ShieldedAEADTests 2>&1 | tail -20
```

Expected: 3/3 PASS.

- [ ] **Step 5: Commit**

```bash
git add DineroDPI/DineroDPI/Core/Shielded/ShieldedAEAD.swift \
        DineroDPI/DineroDPI/Core/Shielded/ShieldedTestVectors.swift \
        DineroDPI/DineroDPITests/Shielded/ShieldedAEADTests.swift
git commit -S -m "$(cat <<'EOF'
feat(shielded): ShieldedAEAD — HKDF-SHA256 + ChaChaPoly IETF, byte parity

Thin facade over CryptoKit HKDF<SHA256> + ChaChaPoly. Verified
byte-equivalent to the daemon's HkdfExtractAndExpand + AeadEncrypt
(src/wallet/shielded_derivation.cpp:325, :447). KAT reproduces the full
611-byte EncryptedNote container from the daemon's vector 2
(PinnedHexVector2EncryptedNote in src/test/shielded_derivation_tests.cpp).

Spike #1 (2026-05-26) verified CryptoKit matches daemon's OpenSSL
EVP_chacha20_poly1305 byte-for-byte; no pure-Swift AEAD fallback is
needed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Shielded key derivation (ivk / ovk / dk)

**Files:**
- Create: `Core/Shielded/ShieldedKeys.swift`
- Create: `DineroDPITests/Shielded/ShieldedKeysTests.swift`

Per spec lines 100-121: derive the spending key tree from the existing BIP39 seed; cache (ivk, ovk, dk) in `KeychainManager`.

- [ ] **Step 1: Read daemon key derivation** at `src/wallet/shielded_derivation.cpp` (around line 100, the `DeriveShieldedKeys` function — verify exact name). Capture the exact BLAKE2s tagged-hash personals and the order of derivation steps.

- [ ] **Step 2: Failing test, then implementation**

(Implementer writes `ShieldedKeysTests.swift` against a generated KAT pin — same generator-harness pattern as Task 3 — then implements `ShieldedKeys.swift`. Output: `ivk` (Data 32), `ovk` (Data 32), `dk` (Data 32) given a BIP39 seed.)

Notes:
- BLAKE2s is not in CryptoKit. Implementer choice: pull in a small BLAKE2s Swift impl, OR add a wrapper around the C BLAKE2s in libsecp256k1's tagged-hash helpers, OR rewrite using `HMAC<SHA256>` IFF the daemon uses HMAC (it does NOT; the daemon uses `blake2s_personal` per Spike #1 inspection). Document the choice in the commit; the spec assumes BLAKE2s.
- The wallet's seed already lives in `KeychainManager`. Add a derived-key cache key `shielded.v7.<accountIndex>.{ivk,ovk,dk}`. Migrate-on-first-launch.

- [ ] **Step 3: Verify byte parity against pinned vector + commit**

(Pin a (seed → ivk) vector from the daemon; commit ShieldedKeys.swift + tests + vector pin in one signed commit.)

---

## Task 5: ShieldedAddress Bech32m codec

**Files:**
- Create: `Core/Shielded/ShieldedAddress.swift`
- Create: `DineroDPITests/Shielded/ShieldedAddressTests.swift`

Spec lines 378-414: 43-byte payload `[d:11 || pk_d:32]` encoded as Bech32m with HRP `dins` (mainnet) / `tdins` (testnet) / `rdins` (regtest). §7.1 enforces HRP rejection.

- [ ] **Step 1: Inspect existing Bech32 code in DineroDPI**

```bash
grep -rn "bech32\|Bech32\|convertbits" DineroDPI/DineroDPI/Core/ 2>/dev/null | head -10
```

Existing Bech32 code (for Taproot `din1p…` and P2MR `din1r…` addresses) likely handles encode/decode. Reuse — don't duplicate. ShieldedAddress = HRP `dins`/`tdins`/`rdins` over a 43-byte payload, Bech32m variant.

- [ ] **Step 2: Implement + test (encode/decode round-trip + 3 invalid-HRP tests + 1 invalid-checksum test)**

- [ ] **Step 3: Commit**

(Standard commit message format; reference spec §7.1 for HRP enforcement rationale.)

---

## Task 6: ShieldedNoteStore (SQLite-backed)

**Files:**
- Create: `Core/Shielded/ShieldedNote.swift`
- Create: `Core/Shielded/ShieldedNoteStore.swift`

Spec lines 234-261: schema is

```sql
CREATE TABLE shielded_notes (
    note_id INTEGER PRIMARY KEY,
    block_hash BLOB(32),
    block_height INTEGER,
    leaf_index INTEGER,
    value_una INTEGER,
    rcm BLOB(32),
    memo BLOB(512),
    nullifier BLOB(32),
    spent_in_block_hash BLOB(32),  -- NULL until M3 spend
    received_at INTEGER
);
CREATE INDEX shielded_notes_by_block ON shielded_notes(block_height);
CREATE INDEX shielded_notes_by_nullifier ON shielded_notes(nullifier);
```

- [ ] **Step 1: Reuse the SQLite wrapper that `UTXOStore.swift` already uses.** Find it via `grep -n "class UTXOStore" DineroDPI/DineroDPI/Core/Filters/UTXOStore.swift`.

- [ ] **Step 2: Implementation** — open/close lifecycle, insert, query by block range, mark-spent (will be exercised in M3), reorg-truncate `WHERE block_height > new_tip`.

- [ ] **Step 3: Tests** — schema migration on fresh DB; insert N notes; query by block range; reorg-truncate clears notes above new tip but preserves below.

- [ ] **Step 4: Commit**

---

## Task 7: CommitmentTree + snapshot/reorg invariant

**Files:**
- Create: `Core/Shielded/CommitmentTree.swift`
- Create: `Core/Shielded/AnchorVerifier.swift`
- Create: `DineroDPITests/Shielded/CommitmentTreeTests.swift`

**This is the M1/M3 contract task.** The snapshot retention rule (256-block interval × 8-snapshot window) is load-bearing for M3 spend witness verification. Implementer MUST NOT treat `kSnapshotInterval` / `kSnapshotRetention` as tuning knobs. Spec lines 201-212 lay this out explicitly; this task implements it.

- [ ] **Step 1: Implementation**

Sapling-shape incremental frontier (logarithmic in tree size). Append leaves in canonical block order. Snapshot the full frontier every 256 leaves into `<datadir>/shielded_tree_snapshots/<height>.bin` (atomic via `.tmp` + rename + best-effort fsync). Prune to last 8 entries.

On reorg, AnchorVerifier picks the most recent snapshot at height ≤ `new_tip - 100`, loads it as the active frontier, re-applies blocks from snapshot height to new tip.

Repeated snapshot-write failure surfaces as a one-time alert (per spec line 210). Silent degradation breaks M3 spend, which is forbidden.

- [ ] **Step 2: Tests** — at minimum:
  - Empty tree → root matches daemon's `empty_root` (generate KAT via daemon harness)
  - Append 1 leaf → root differs from empty
  - Append 256 leaves → snapshot file exists, prune left exactly 1 snapshot
  - Append 9 × 256 leaves → snapshot dir has exactly 8 files, oldest pruned
  - Reorg test: append 300 leaves (1 snapshot at h=256), revert tip from 300 → 256, re-apply different 50 leaves → root matches independently-rebuilt tree

- [ ] **Step 3: Commit**

(Reference spec lines 201-212 in commit message — this is the load-bearing invariant. Future implementers tempted to shrink retention "for storage" must hit a hard wall in code review.)

---

## Task 8: BlockParser shielded extension + ShieldedScanner + integration

**Files:**
- Create: `Core/Shielded/BlockParserShieldedExt.swift`
- Create: `Core/Shielded/ShieldedScanner.swift`
- Create: `DineroDPITests/Shielded/ShieldedScannerIntegrationTests.swift`
- Modify: `Core/Filters/FilterChainSync.swift`
- Modify: `Views/WalletView.swift` (line 4890 guardrail)

This is the orchestration task. Brings everything together.

- [ ] **Step 1: Extend BlockParser** to enumerate shielded outputs per block. Decode each `[epk:32][ct:563][tag:16]` container. Surface as `ShieldedOutput` value type.

- [ ] **Step 2: Implement ShieldedScanner**

For each block-of-interest range from `FilterChainSync`:
1. Fetch full block (existing path; M2 will optimize via daemon filter)
2. Extension enumerates shielded outputs
3. For each output:
   - Compute `shared = ecdh(ivk, epk)` (use the existing libsecp256k1 wrapper; tweak_mul on the scalar API)
   - Derive AEAD key via `ShieldedAEAD.hkdfDeriveKey`
   - Try `ShieldedAEAD.open` — failure means not for us, silently discard
   - On success: deserialize 563-byte plaintext, compute note commitment, verify against the block's commitment-tree append (via `CommitmentTree.appendAndExpectedRoot`)
4. On match: insert into `ShieldedNoteStore` with `leaf_index` and `block_height`
5. Scan spent-nullifier set from block to mark `spent_in_block_hash` on any owned-notes (NULL until then)

- [ ] **Step 3: Wire into FilterChainSync**

The existing transparent scan path knows when blocks-of-interest arrive. Add a parallel `shieldedScanner.scan(block:)` call. On reorg, call `commitmentTree.handleReorg(newTip:)` and `noteStore.truncateAbove(newTip)`.

- [ ] **Step 4: Update WalletShieldedView guardrail**

The current text at `Views/WalletView.swift:4890` says *"Shielded keys and note state live in the active local wallet. Shared bridge RPC mode is intentionally blocked from deriving or spending shielded wallet funds."*

Update to: *"Shielded balance and history are now visible in thin-client mode (M1). Spending shielded funds still requires Embedded Node mode or a future witness-fetch update."*

Bridge-RPC mode now SHOWS `ShieldedNoteStore` data. Spend buttons remain disabled (M3).

- [ ] **Step 5: Integration test**

Scripted fixture: 3 blocks of regtest data, 2 of which contain a shielded output for the wallet's ivk. Assert:
- After scan: ShieldedNoteStore has 2 entries
- Commitment-tree root matches independently-rebuilt
- WalletShieldedView fetch returns 2 notes with correct value_una sums

- [ ] **Step 6: Commit**

```bash
# (Single commit — the integration is the deliverable, splitting these
#  pieces would land non-working intermediate states.)
git add DineroDPI/DineroDPI/Core/Shielded/BlockParserShieldedExt.swift \
        DineroDPI/DineroDPI/Core/Shielded/ShieldedScanner.swift \
        DineroDPI/DineroDPI/Core/Filters/FilterChainSync.swift \
        DineroDPI/DineroDPI/Views/WalletView.swift \
        DineroDPI/DineroDPITests/Shielded/ShieldedScannerIntegrationTests.swift
git commit -S -m "$(cat <<'EOF'
feat(shielded): ShieldedScanner wires receive-side M1 end-to-end

For each block-of-interest from FilterChainSync, enumerate shielded
outputs, trial-decrypt against local IVK, verify against commitment
tree, persist matches to ShieldedNoteStore. Reorg path truncates above
new tip and re-applies via CommitmentTree's snapshot lineage.

WalletShieldedView guardrail (Views/WalletView.swift:4890) updated:
bridge-RPC mode now SHOWS shielded balance/history (read-only). Spend
buttons remain disabled — that's M3 (separate spec/plan).

This is the "iOS shielded UX in thin-client mode" delivery the
trustless-LC-shielded spec was built for.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: On-device bench + sanity log + draft PR

**Files:**
- Create: `docs/superpowers/plans/2026-05-26-trustless-light-client-shielded-m1-sanity.md` (in dinero-v8 repo, NOT DineroDPI — co-locates with spec)

- [ ] **Step 1: Run the perf bench on a real iPhone**

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj -scheme DineroDPI \
  -destination 'platform=iOS,name=<your iPhone>' \
  test -only-testing:DineroDPITests/PoseidonTests 2>&1 | tail -25
```

Capture the logged "Poseidon perf: X µs/eval" number on iPhone (NOT simulator — simulator runs on Mac silicon and isn't representative).

**Hard gate:** iPhone per-eval ≤ 67 µs. If ≥ 100 µs, halt and open follow-up to wrap daemon Poseidon as .xcframework.

- [ ] **Step 2: Run full Shielded test suite**

```bash
xcodebuild -project DineroDPI/DineroDPI.xcodeproj -scheme DineroDPI \
  -destination 'platform=iOS,name=<your iPhone>' \
  test -only-testing:DineroDPITests/PoseidonTests \
  -only-testing:DineroDPITests/ShieldedAEADTests \
  -only-testing:DineroDPITests/ShieldedKeysTests \
  -only-testing:DineroDPITests/ShieldedAddressTests \
  -only-testing:DineroDPITests/CommitmentTreeTests \
  -only-testing:DineroDPITests/ShieldedScannerIntegrationTests 2>&1 | tail -20
```

All targets must PASS on real device.

- [ ] **Step 3: Write sanity log in dinero-v8 (alongside spec)**

```bash
cd /Users/haydarevich/src/dinero-v8
git checkout dinero-main && git pull
cat > docs/superpowers/plans/2026-05-26-trustless-light-client-shielded-m1-sanity.md <<EOF
# Trustless LC Shielded M1 — Sanity Log

**Date:** $(date -u +%FT%TZ)
**Branch (iOS):** feature/m1-shielded-lc-receive
**iPhone tested:** <model + iOS version>

| Check | Result | Notes |
|---|---|---|
| Poseidon KAT (zero,zero) byte parity | PASS | |
| Poseidon KAT (one,zero) byte parity | PASS | |
| iPhone Poseidon per-eval | <X µs> | Gate: ≤ 67 µs |
| AEAD vector 2 byte parity | PASS | 611-byte container |
| ShieldedKeys ivk pin | PASS | |
| ShieldedAddress encode/decode round-trip | PASS | |
| CommitmentTree empty-root vs daemon | PASS | |
| Snapshot retention 8 enforced | PASS | |
| Reorg replay produces matching root | PASS | |
| End-to-end scanner integration | PASS | 2-of-3 fixture blocks |
| WalletShieldedView shows notes in bridge-RPC mode | PASS | spend buttons disabled |
EOF
git add docs/superpowers/plans/2026-05-26-trustless-light-client-shielded-m1-sanity.md
git commit -S -m "docs: M1 trustless-LC-shielded sanity log

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
git push origin dinero-main
```

- [ ] **Step 4: Push DineroDPI branch + draft PR**

```bash
cd /private/tmp/dineroDPI-m1-shielded
git push -u origin feature/m1-shielded-lc-receive
gh pr create --draft --title "feat(shielded): M1 trustless light-client receive-side scanning" --body "$(cat <<'EOF'
## Summary

Implements **Milestone 1** of the trustless light-client shielded scanning spec
([design doc](https://github.com/DineroLabs/dinero-v8/blob/dinero-main/docs/superpowers/specs/2026-05-26-trustless-light-client-shielded-design.md), PR #159).

Receive-side only. M3 (spend) is a separate plan.

### What this delivers

iOS users in **thin-client mode** can now see their shielded balance and
history without trusting any remote RPC and without running an embedded
node. Server sees which blocks you fetch (same as transparent
light-client); does NOT see which outputs you decrypted successfully.

### Crypto path verified against daemon byte-for-byte

- Poseidon-2 (KAT against `poseidon2_bytes` from `src/zk/zkvm/`)
- AEAD encrypt/decrypt (KAT against vector 2 / 611-byte `EncryptedNote` container)
- HKDF + ChaChaPoly via CryptoKit (Spike #1 verified parity)
- Scalar arithmetic via existing `secp256k1.xcframework`'s public
  `secp256k1_ec_seckey_tweak_*` API (same calls the daemon's `Scalar`
  uses internally)

### Performance

iPhone bench: **<X µs/eval** for Poseidon (gate: ≤ 67 µs). First-sync
projection: ~<Y> sec for the current chain post-shielded-activation —
well under spec's 22s budget.

### Architectural invariants honored

- `kSnapshotInterval = 256` blocks × `kSnapshotRetention = 8` entries
  (load-bearing for M3 spend; do NOT tune)
- Active wallet only (multi-wallet background scan is M3+)
- Reorg max depth = 100 blocks (consensus bound)

### Out of scope (future work)

- **M2:** daemon-side shielded compact filter (reduces per-block
  trial-decrypt cost by ~100×)
- **M3:** spend path — needs witness-by-leaf-index RPC + Spartan prover
  `.xcframework`

### Test plan

- [x] KAT for Poseidon, AEAD, ShieldedKeys, ShieldedAddress against pinned daemon vectors
- [x] CommitmentTree snapshot + reorg invariants
- [x] End-to-end ShieldedScanner integration (3-block fixture, 2 owned outputs)
- [x] On-device iPhone perf bench (logged in sanity)
- [ ] CI: full DineroDPI test suite

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Then poll CI and mark ready when green.

---

## Coverage map (self-review)

| Spec requirement | Task |
|---|---|
| secp256k1 scalar arithmetic wrappers | 1 |
| Poseidon-2 byte-equivalent to daemon | 2 |
| HKDF-SHA256 + ChaChaPoly IETF, AEAD layout | 3 |
| ivk / ovk / dk derivation from BIP39 seed | 4 |
| ShieldedAddress Bech32m codec (HRP enforced) | 5 |
| ShieldedNoteStore (SQLite, reorg-safe truncate) | 6 |
| Incremental commitment-tree frontier | 7 |
| Snapshot retention invariant (256 × 8) | 7 |
| AnchorVerifier (anchored at PoW-checked block hash) | 7 |
| BlockParser shielded-output enumeration | 8 |
| ShieldedScanner orchestration | 8 |
| FilterChainSync integration | 8 |
| WalletShieldedView surface (display in bridge-RPC mode) | 8 |
| iPhone perf bench + sanity + draft PR | 9 |

---

## Notes for the implementer

- **The spec is authoritative.** When this plan and the spec disagree, the spec wins — `docs/superpowers/specs/2026-05-26-trustless-light-client-shielded-design.md`.
- **Test vectors come from the daemon, not from this plan.** Generate via `/tmp/poseidon_daemon_harness.cpp` (extend as needed) and pin into `ShieldedTestVectors.swift`. Commit each pin alongside the test that uses it.
- **Per-task verify-commit-landed:** parent must `git status` + `git log -1` after every subagent commit (see `feedback_subagent_verify_commit`). Subagents have shipped phantom-completed tasks before.
- **If the iPhone Poseidon bench misses the 67 µs gate:** stop. Don't ship M1 with a perf cliff. Open a follow-up task: *"Wrap daemon poseidon2_bytes as .xcframework, replace `Poseidon.hash` body with FFI call, re-run gate."* M3 needs `.xcframework` infrastructure anyway — this is just pulling it forward.
- **Don't touch `feedback_dinero_project_first`:** if a spec ambiguity could compromise the chain or a wallet, raise it BEFORE building on it. Better one paused day than a shipped privacy bug.

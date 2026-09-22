# Shielded v2 Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One Spartan+Hyrax proof per shielded transaction (transaction version 7, envelope `DZV2` id `0x01`) over a hash-only bundle statement, coexisting with today's v1 proofs behind activation and sunset heights that stay at `UINT32_MAX`.

**Architecture:** Everything new lives in `include/consensus/shielded/v2/` and `src/consensus/shielded/v2/` inside the existing `dinero_shielded` library. The existing v1 code is entered through five narrow seams only: the transaction-version predicates, a version-aware bundle-codec front door, a two-line dispatch at the top of `ValidateShieldedBundle`, a version-7 branch in the auth resource limits, and a third sighash encoding. The wallet gains a hash-derived key scheme, a v2 bundle builder, and a migration RPC. Block acceptance pre-verifies all v2 proofs of a block in parallel before the ingress lock and feeds the proof cache, so the locked path only hits the cache.

**Tech Stack:** C++20; existing `dinero_zk` (R1CS, Poseidon-2 gadget, Spartan sum-check + Hyrax); gtest/ctest; CMake/Ninja; GitHub Actions; libFuzzer.

**Spec:** `docs/superpowers/specs/2026-09-22-shielded-v2-design.md`. Section 10 (Amendment A, hash-derived ownership key) is **pending owner approval**; Tasks 0–4 and 6–8 are safe to build before that approval because nothing activates, but Task 5 (wallet key scheme) and any height selection must wait for it. Spike report: `docs/superpowers/spike/2026-09-22-shielded-v2-spike-report.md` (with addendum).

## Global Constraints

- Transaction versions 5 and 6 behave byte-for-byte as today at every height below `shielded_v1_sunset_height`. Every existing shielded ctest stays green; none is edited except to extend it.
- New consensus heights: `shielded_v2_activation_height = UINT32_MAX`, `shielded_v1_sunset_height = UINT32_MAX` on every network. Overrides are REGTEST-only CLI flags, refused elsewhere, exactly like `--consensus-shielded-spend-auth-height`.
- Envelope: `'D' 'Z' 'V' '2' | proof_system_id (u8) | CompactSize len | proof bytes`. Phase 1 accepts `proof_system_id = 0x01` only. Any other id, tag or length fails closed (`ProofInvalid`).
- Caps, enforced before any proof work: `kV2MaxSpends = 4`, `kV2MaxOutputs = 2`, `kV2MaxProofBytes = 32768`, `kV2MaxEnvelopeBytes = 4 + 1 + 5 + 32768`.
- Transcript domain: `dinero.shielded.bundle.v2`. Sighash domain for version 7: `DIN/v7/shielded/tx-sighash/bundle-v2`. Cache-key domain: `dinero.shielded.verified.bundle.v2`. Hash spend key tag: `DIN/v7/shielded/v2/hk`. These strings are consensus-critical; they appear once each in code and are pinned by vectors.
- Public-input order (prover and verifier identical): `sighash, vb_pos, vb_neg, anchor[0], nullifier[0], …, anchor[n-1], nullifier[n-1], commitment[0], …, commitment[m-1]`.
- Statement (spec §3.1 as amended by §10.2): per spend `hk = P(ask, HK_TAG)`, `pk_d = P(hk, d)`, `nfk_c = P(nfk, NFK_TAG)`, `pk = P(pk_d, nfk_c)`, `cm = P(P(P(ADDR_TAG, P(d, pk)), value), rcm)`, Merkle path depth 32 to `anchor_i`, `nullifier_i = P(nfk, leaf_index)`, `value_i` 64-bit; per output `cm_j` as above with `pk` a witness, `value_j` 64-bit; `vb_pos + Σ value_i = vb_neg + Σ value_j`, `vb_pos`, `vb_neg` 64-bit; `sighash` bound by one constraint and by the transcript. `P` is Poseidon-2 over the secp256k1 scalar field (`poseidon2_gadget`).
- Performance gates (Release only, asserted by ctest on the CI runner class; local M4 Max numbers recorded in JSON, not asserted): proof envelope ≤ 20,480 bytes; single 2-in-2-out verify ≤ 140 ms; batched 50 × 2-in-2-out ≤ 25 ms per proof on 8 threads; prove 2-in-2-out ≤ 1,000 ms. M4 Max reference from the spike: 18,792 B / 65 ms / 11.7 ms / 486 ms.
- Repo rule: every rule or fix ships with a test that fails without it. `assert()` is not a gate; use gtest assertions or exit-nonzero.
- Commit after every task with the attribution line `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Nothing merges to `dinero-main` without owner approval.

## Blast radius (every existing file this plan touches)

| file | change |
|---|---|
| `include/primitives/transaction.h:167-184` | add `TX_VERSION_SHIELDED_BUNDLE_V2 = 7`, `IsShieldedBundleV2Version`, include it in `IsShieldedAuthVersion` |
| `include/consensus/chainparams.h:272` | add `shielded_v2_activation_height`, `shielded_v1_sunset_height` |
| `src/consensus/chainparams_impl.cpp:589` | `ValidateChainParams` also checks `V2ActivationConfigurationValid` |
| `src/daemon/main.cpp:900-909` | two REGTEST-only overrides next to the compact override |
| `include/consensus/shielded/shielded_tx.h:99-121` | add `std::vector<uint8_t> v2_proof;` to `ShieldedBundle` |
| `include/consensus/shielded/binding_sig.h:100`, `src/consensus/shielded/binding_sig.cpp:43-60` | third `ShieldedProofEncoding::BundleV2`, its domain, version-7 auto-detect |
| `include/consensus/shielded/shielded_validation.h:59-70,90-139,209-228`, `src/consensus/shielded/shielded_validation.cpp:258,278` | new error `ProofVersionSunset = 10`; `ValidationContext::v2_rules`; 13th `BuildShieldedValidationContext` parameter; two-line dispatch |
| `src/consensus/block_validation.cpp:101-112,149,167-179,683` | error string; codec front door; pass `V2RulesFor(Params())` |
| `src/daemon/mempool.cpp:2461,2526-2538,3331,3345` | codec front door; pass `V2RulesFor` |
| `src/consensus/reindexer.cpp:2431,2471-2483` | codec front door; pass `V2RulesFor` |
| `include/consensus/shielded/resource_limits.h:73-120` | codec front door; version-7 branch (`proofs = 1`, v2 caps, envelope cap) |
| `src/consensus/shielded/CMakeLists.txt:10-44` | add the `v2/*.cpp` sources |
| `src/daemon/block_acceptor.cpp:721` | one call to `PrewarmBundleProofCacheV2` at the top of `AcceptBlockFromPeer` |
| `include/wallet/shielded_note_store.h:41-49` | `NoteKeyScheme::HashKeyV2 = 3` |
| `include/wallet/shielded_derivation.h`, `src/wallet/shielded_derivation.cpp` | `ShieldedAccountKeys::hk`; `DeriveDiversifiedAddressV2`; v2 HRPs in `DecodeShieldedAddress` |
| `src/wallet/shielded_wallet_ops.cpp`, `include/wallet/shielded_wallet_ops.h` | scan recognises scheme 3; change outputs to v2 addresses when the wallet has `hk` |
| `src/rpc/shielded_rpc_json.cpp:1764-1776` | register `wallet.shieldedaddressv2`, `wallet.shieldedmigratev2`, `wallet.transferv2` |
| `tests/CMakeLists.txt` | new test targets and Release-only performance test |
| `.github/workflows/` | `shielded-v2-vectors.yml`, `shielded-v2-sanitizers.yml`, `shielded-e2e.yml` |
| `scripts/ci/unexecuted_tests_baseline.txt` | remove the 22 shielded e2e tests once their lane exists |

Everything else is a new file under `include/consensus/shielded/v2/`, `src/consensus/shielded/v2/`, `src/wallet/shielded_v2_*.cpp`, `tests/consensus/test_shielded_v2_*.cpp`, `tests/vectors/shielded_v2_v1/`, `tests/fuzz/`.

---

### Task 0: Measure the final statement and pin the gates

**Files:**
- Modify: `tests/spike/bundle_circuit_v2.h:34-49,84-131` (spike, throwaway)
- Create: `docs/benchmarks/shielded-v2-phase1-statement-20260922.json`

**Interfaces:**
- Produces: the measured constraint count, proof bytes, prove and verify times of the exact §10.2 statement, referenced by Task 7's gates.

- [ ] **Step 1: Add the hash-key legs to the spike circuit**

In `tests/spike/bundle_circuit_v2.h` replace the `SpendLeg` struct and the spend loop so the statement matches spec §10.2:

```cpp
struct SpendLeg {
    Scalar ask, nullifier_key, value, randomness, diversifier;
    uint32_t leaf_index = 0;
    std::array<Hash, TREE_DEPTH> siblings{};
    Scalar anchor, nullifier;  // public
};
```

and in `BuildBundleCircuitV2`, replace the block from `Variable sk = cs.alloc(s.secret_key);` through `gadgets::assert_equal(cs, nf, nullifiers[i], p + "_nullifier");` with:

```cpp
        Variable ask = cs.alloc(s.ask);
        Variable nfk = cs.alloc(s.nullifier_key);
        Variable val = cs.alloc(s.value);
        Variable rnd = cs.alloc(s.randomness);
        Variable div = cs.alloc(s.diversifier);
        Variable idx = cs.alloc(Scalar(static_cast<uint64_t>(s.leaf_index)));
        Variable hk_tag = gadgets::constant(cs, HashToScalarV2(HkTagV2()), p + "_hktag");
        Variable nfk_tag = gadgets::constant(cs, HashToScalarV2(dinero::consensus::shielded::NullifierKeyTag()), p + "_nfktag");
        Variable hk = poseidon2_gadget(cs, ask, hk_tag, p + "_hk");
        Variable pk_d = poseidon2_gadget(cs, hk, div, p + "_pkd");
        Variable nfk_c = poseidon2_gadget(cs, nfk, nfk_tag, p + "_nfkc");
        Variable pk = poseidon2_gadget(cs, pk_d, nfk_c, p + "_pk");
        Variable cm = NoteCommitmentV2(cs, div, pk, val, rnd, p);
        Variable root = MerklePathV2(cs, cm, idx, s.siblings, p);
        gadgets::assert_equal(cs, root, anchors[i], p + "_anchor");
        Variable nf = poseidon2_gadget(cs, nfk, idx, p + "_nf");
        gadgets::assert_equal(cs, nf, nullifiers[i], p + "_nullifier");
```

Add above `BuildBundleCircuitV2`:

```cpp
inline Hash HkTagV2() {
    static const char tag[] = "DIN/v7/shielded/v2/hk";
    Hash h{};
    std::memcpy(h.data(), tag, sizeof(tag) - 1);
    return h;
}
```

Update `MakeHonestBundle` accordingly: set `s.ask = Scalar(uint64_t{1000 + i}); s.nullifier_key = Scalar(uint64_t{7000 + i});`, compute `hk = poseidon2_native(s.ask, HashToScalarV2(HkTagV2()))`, `pk_d = poseidon2_native(hk, s.diversifier)`, `nfk_c = poseidon2_native(s.nullifier_key, HashToScalarV2(NullifierKeyTag()))`, `pk = poseidon2_native(pk_d, nfk_c)`, and `s.nullifier = poseidon2_native(s.nullifier_key, Scalar(uint64_t(s.leaf_index)))`. Replace the `pub_only` zeroing lines in `tests/spike/shielded_v2_bundle_bench.cpp` (`sp.secret_key = ...`) with `sp.ask = sp.nullifier_key = sp.value = sp.randomness = sp.diversifier = Scalar::zero();`.

- [ ] **Step 2: Build and run the bench**

Run: `cmake --build build-spike --target spike_shielded_v2_bench && ./build-spike/spike_shielded_v2_bench --selftest && ./build-spike/spike_shielded_v2_bench --no-baseline`
Expected: self-test exit 0; JSON with three `bundle_*` rows. Constraints for `bundle_2in2out` between 55,000 and 62,000 (spike had 53,038 with two fewer Poseidon evaluations per spend).

- [ ] **Step 3: Record**

Write the JSON output verbatim to `docs/benchmarks/shielded-v2-phase1-statement-20260922.json` with a top-level `"host": "M4 Max, Release, build-spike"` field added. If `bundle_2in2out.verify_ms` exceeds 70 or `proof_bytes` exceeds 20,480, stop and report before Task 7: the report's tuning items (drop the `E` opening, Hyrax column shaping) move ahead of the performance ctest.

- [ ] **Step 4: Commit**

```bash
git add tests/spike/bundle_circuit_v2.h tests/spike/shielded_v2_bundle_bench.cpp docs/benchmarks/shielded-v2-phase1-statement-20260922.json
git commit -m "spike(shielded-v2): measure the amended hash-key bundle statement

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 1: Activation and sunset heights

**Files:**
- Create: `include/consensus/shielded/v2/rules.h`
- Modify: `include/consensus/chainparams.h:272` (after `shielded_compact_activation_height`)
- Modify: `src/consensus/chainparams_impl.cpp:589-595`
- Modify: `src/daemon/main.cpp:900-909` (next to the compact override; flag parsing follows `compact_regtest_override`, found with `grep -n compact_regtest_override src/daemon/main.cpp`)
- Test: `tests/consensus/test_shielded_v2_activation.cpp`
- Modify: `tests/CMakeLists.txt` (new target next to `test_compact_activation`, found with `grep -n test_compact_activation tests/CMakeLists.txt`)

**Interfaces:**
- Produces: `struct ShieldedV2Rules { bool enabled; uint32_t activation_height; uint32_t v1_sunset_height; constexpr bool Active(uint64_t) const; constexpr bool V1Allowed(uint64_t) const; }`, `bool V2ActivationConfigurationValid(const ChainParams&)`, `ShieldedV2Rules V2RulesFor(const ChainParams&)`; `ChainParams::shielded_v2_activation_height`, `ChainParams::shielded_v1_sunset_height`.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/consensus/test_shielded_v2_activation.cpp
#include "consensus/chainparams.h"
#include "consensus/shielded/v2/rules.h"
#include <gtest/gtest.h>
#include <limits>

namespace {
namespace sh = dinero::consensus::shielded;
using dinero::ChainParams;

ChainParams Scheduled() {
    ChainParams p{};
    p.name = "regtest";
    p.shielded_activation_height = 1;
    p.shielded_input_binding_activation_height = 2;
    p.shielded_cv_binding_activation_height = 3;
    p.shielded_epoch_reset_height = 3;
    p.shielded_spend_auth_activation_height = 4;
    p.shielded_spend_auth_epoch_reset_height = 4;
    p.shielded_v2_activation_height = 20;
    p.shielded_v1_sunset_height = 30;
    return p;
}

TEST(ShieldedV2Activation, DefaultsAreDormantOnEveryNetwork) {
    ChainParams p{};
    EXPECT_EQ(p.shielded_v2_activation_height, UINT32_MAX);
    EXPECT_EQ(p.shielded_v1_sunset_height, UINT32_MAX);
    EXPECT_TRUE(sh::V2ActivationConfigurationValid(p));
    const auto rules = sh::V2RulesFor(p);
    EXPECT_FALSE(rules.Active(0));
    EXPECT_FALSE(rules.Active(UINT32_MAX));
    EXPECT_TRUE(rules.V1Allowed(0));
    EXPECT_TRUE(rules.V1Allowed(UINT32_MAX));
}

TEST(ShieldedV2Activation, ScheduledBoundaries) {
    const auto rules = sh::V2RulesFor(Scheduled());
    ASSERT_TRUE(rules.enabled);
    EXPECT_FALSE(rules.Active(19));
    EXPECT_TRUE(rules.Active(20));
    EXPECT_TRUE(rules.Active(21));
    EXPECT_TRUE(rules.V1Allowed(29));
    EXPECT_FALSE(rules.V1Allowed(30));
    EXPECT_FALSE(rules.V1Allowed(31));
}

TEST(ShieldedV2Activation, InvalidSchedulesFailClosed) {
    auto p = Scheduled();
    p.shielded_v2_activation_height = 4;  // not after the Auth reset
    EXPECT_FALSE(sh::V2ActivationConfigurationValid(p));
    EXPECT_FALSE(sh::V2RulesFor(p).Active(100));

    p = Scheduled();
    p.shielded_v1_sunset_height = 20;  // sunset not after activation
    EXPECT_FALSE(sh::V2ActivationConfigurationValid(p));

    p = Scheduled();
    p.shielded_v2_activation_height = UINT32_MAX;  // sunset without activation
    EXPECT_FALSE(sh::V2ActivationConfigurationValid(p));
    EXPECT_TRUE(sh::V2RulesFor(p).V1Allowed(1000));

    p = Scheduled();
    p.shielded_spend_auth_activation_height = UINT32_MAX;  // v2 requires Auth scheduled
    EXPECT_FALSE(sh::V2ActivationConfigurationValid(p));
}
}  // namespace
```

- [ ] **Step 2: Run it to verify it fails**

Add the target to `tests/CMakeLists.txt` right after the `test_compact_activation` block, same link set and include directories as that block, with `add_test(NAME ShieldedV2Activation COMMAND test_shielded_v2_activation)` and `LABELS "shielded;consensus;mandatory"`. Then:

Run: `cmake --build build-spike --target test_shielded_v2_activation`
Expected: compile error, `consensus/shielded/v2/rules.h` not found.

- [ ] **Step 3: Add the heights**

In `include/consensus/chainparams.h` directly after `uint32_t shielded_compact_activation_height = UINT32_MAX;`:

```cpp
    // Shielded v2: one bundle proof per transaction (tx version 7, DZV2 envelope).
    // Requires the Auth profile to be scheduled and lies strictly after it.
    // UINT32_MAX is dormant on every network; regtest override only.
    uint32_t shielded_v2_activation_height = UINT32_MAX;
    // From this height new blocks may not carry v1 shielded proofs (tx versions
    // 5/6 with a non-empty bundle). Must lie strictly after v2 activation and is
    // chosen only after migration telemetry. UINT32_MAX = never.
    uint32_t shielded_v1_sunset_height = UINT32_MAX;
```

- [ ] **Step 4: Write the rules header**

```cpp
// include/consensus/shielded/v2/rules.h
#pragma once
#include "consensus/chainparams.h"
#include <cstdint>

namespace dinero::consensus::shielded {

struct ShieldedV2Rules {
    bool enabled = false;
    uint32_t activation_height = UINT32_MAX;
    uint32_t v1_sunset_height = UINT32_MAX;

    constexpr bool Active(uint64_t height) const {
        return enabled && activation_height != UINT32_MAX && height >= activation_height;
    }
    // v1 proofs stay valid until the sunset. A sunset can only bite once v2 is active.
    constexpr bool V1Allowed(uint64_t height) const {
        return !(Active(height) && v1_sunset_height != UINT32_MAX && height >= v1_sunset_height);
    }
};

// Fail closed for any schedule that is not (Auth < v2 < sunset-or-never).
inline bool V2ActivationConfigurationValid(const ChainParams& p) {
    const uint32_t v2 = p.shielded_v2_activation_height;
    const uint32_t sunset = p.shielded_v1_sunset_height;
    if (v2 == UINT32_MAX) return sunset == UINT32_MAX;
    return p.shielded_spend_auth_activation_height != UINT32_MAX &&
           p.shielded_spend_auth_activation_height < v2 &&
           (sunset == UINT32_MAX || sunset > v2);
}

inline ShieldedV2Rules V2RulesFor(const ChainParams& p) {
    return {V2ActivationConfigurationValid(p), p.shielded_v2_activation_height,
            p.shielded_v1_sunset_height};
}

}  // namespace dinero::consensus::shielded
```

- [ ] **Step 5: Wire chainparams validation and the regtest override**

In `src/consensus/chainparams_impl.cpp` inside `ValidateChainParams`, after the compact check add:

```cpp
    if (!consensus::shielded::V2ActivationConfigurationValid(params)) {
        throw std::runtime_error("chainparams: shielded v2 schedule invalid (need Auth < v2 < sunset)");
    }
```

(`#include "consensus/shielded/v2/rules.h"` at the top.) Match the throw/abort style the compact check uses two lines above.

In `src/daemon/main.cpp`, declare `long long shielded_v2_override = -1, shielded_v1_sunset_override = -1;` where `compact_regtest_override` is declared, parse `--consensus-shielded-v2-height=` and `--consensus-shielded-v1-sunset-height=` where `--consensus-shielded-compact-height=` is parsed (same pattern), and after the compact override block add:

```cpp
    if (shielded_v2_override >= 0 || shielded_v1_sunset_override >= 0) {
        auto& mp = dinero::MutableParams();
        if (chain != dinero::Chain::REGTEST || mp.shielded_spend_auth_activation_height == UINT32_MAX ||
            shielded_v2_override <= static_cast<long long>(mp.shielded_spend_auth_activation_height)) {
            std::cerr << "Shielded v2 override requires REGTEST and a height after the Auth reset\n";
            return 1;
        }
        mp.shielded_v2_activation_height = static_cast<uint32_t>(shielded_v2_override);
        if (shielded_v1_sunset_override >= 0) {
            if (shielded_v1_sunset_override <= shielded_v2_override) {
                std::cerr << "Shielded v1 sunset must be after v2 activation\n";
                return 1;
            }
            mp.shielded_v1_sunset_height = static_cast<uint32_t>(shielded_v1_sunset_override);
        }
        std::cout << "[Network] REGTEST shielded v2 bundle proofs at height " << shielded_v2_override
                  << (shielded_v1_sunset_override >= 0 ? ", v1 sunset at " + std::to_string(shielded_v1_sunset_override) : std::string())
                  << " (test-only)\n";
    }
```

- [ ] **Step 6: Run the test**

Run: `cmake --build build-spike --target test_shielded_v2_activation && ./build-spike/test_shielded_v2_activation`
Expected: 3 tests PASS. Also run `./build-spike/test_compact_activation` (must still pass) and `cmake --build build-spike --target dinerod` (must compile).

- [ ] **Step 7: Commit**

```bash
git add include/consensus/shielded/v2/rules.h include/consensus/chainparams.h src/consensus/chainparams_impl.cpp src/daemon/main.cpp tests/consensus/test_shielded_v2_activation.cpp tests/CMakeLists.txt
git commit -m "shielded-v2: activation and v1 sunset heights (dormant), rules, regtest override

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Transaction version 7, DZV2 envelope, v2 bundle codec, sighash encoding

**Files:**
- Modify: `include/primitives/transaction.h:167-184`
- Modify: `include/consensus/shielded/shielded_tx.h:99-121`
- Create: `include/consensus/shielded/v2/envelope.h`, `src/consensus/shielded/v2/envelope.cpp`
- Create: `include/consensus/shielded/v2/serialization.h`, `src/consensus/shielded/v2/serialization.cpp`
- Modify: `include/consensus/shielded/binding_sig.h:100`, `src/consensus/shielded/binding_sig.cpp:43-60`
- Modify: the eight decode call sites listed in the blast radius (`block_validation.cpp:149,683`, `mempool.cpp:2461,3331,3345`, `reindexer.cpp:2431`, `resource_limits.h:89,104`)
- Modify: `src/consensus/shielded/CMakeLists.txt` (add `v2/envelope.cpp`, `v2/serialization.cpp`)
- Test: `tests/consensus/test_shielded_v2_codec.cpp` (+ target `test_shielded_v2_codec`, ctest `ShieldedV2Codec`)

**Interfaces:**
- Produces:
  - `Transaction::TX_VERSION_SHIELDED_BUNDLE_V2 = 7`, `static constexpr bool Transaction::IsShieldedBundleV2Version(int32_t)`.
  - `ShieldedBundle::v2_proof` (`std::vector<uint8_t>`).
  - `namespace dinero::consensus::shielded::v2`: `constexpr std::array<uint8_t,4> kV2Tag{'D','Z','V','2'}`, `enum class ProofSystemId : uint8_t { SpartanHyrax = 0x01, SpartanHashPcs = 0x02 }`, `struct V2Envelope { ProofSystemId id; std::vector<uint8_t> proof; }`, `enum class EnvelopeDecodeError { Ok, Truncated, BadTag, UnknownId, LengthMismatch, TooLarge }`, `std::vector<uint8_t> EncodeV2Envelope(const V2Envelope&)`, `EnvelopeDecodeError DecodeV2Envelope(const std::vector<uint8_t>&, V2Envelope*)`, `constexpr size_t kV2MaxSpends = 4, kV2MaxOutputs = 2, kV2MaxProofBytes = 32768, kV2MaxEnvelopeBytes = 4 + 1 + 5 + kV2MaxProofBytes`.
  - `std::vector<uint8_t> SerializeShieldedBundleV2(const ShieldedBundle&)`, `BundleDecodeError DeserializeShieldedBundleV2(const uint8_t*, size_t, ShieldedBundle*)`, vector overload, `BundleDecodeError DeserializeShieldedBundleForVersion(int32_t tx_version, const std::vector<uint8_t>&, ShieldedBundle*)`, `std::vector<uint8_t> SerializeShieldedBundleForVersion(int32_t tx_version, const ShieldedBundle&)` (all in `namespace dinero::consensus::shielded`, declared in `v2/serialization.h`).
  - `ShieldedProofEncoding::BundleV2` with domain `DIN/v7/shielded/tx-sighash/bundle-v2`; `ComputeShieldedTxSighash(tx)` selects it for version 7.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/consensus/test_shielded_v2_codec.cpp
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/v2/envelope.h"
#include "consensus/shielded/v2/serialization.h"
#include "primitives/transaction.h"
#include <gtest/gtest.h>

namespace {
using namespace dinero::consensus::shielded;
using dinero::Transaction;

Hash H(uint8_t b) { Hash h{}; h.fill(b); return h; }

ShieldedBundle SampleBundle() {
    ShieldedBundle b;
    b.value_balance = -1500;
    b.spends.push_back({H(0x11), H(0xA1), {}, {}});
    b.spends.push_back({H(0x22), H(0xA1), {}, {}});
    b.outputs.push_back({H(0x33), {}, std::vector<uint8_t>(611, 0xEE), {}});
    b.outputs.push_back({H(0x44), {}, std::vector<uint8_t>(611, 0xEF), {}});
    b.v2_proof = v2::EncodeV2Envelope({v2::ProofSystemId::SpartanHyrax, std::vector<uint8_t>(100, 0x5A)});
    return b;
}

TEST(ShieldedV2Codec, VersionSevenPredicates) {
    EXPECT_EQ(Transaction::TX_VERSION_SHIELDED_BUNDLE_V2, 7);
    EXPECT_TRUE(Transaction::IsShieldedBundleV2Version(7));
    EXPECT_FALSE(Transaction::IsShieldedBundleV2Version(6));
    EXPECT_TRUE(Transaction::IsShieldedVersion(7));
    EXPECT_TRUE(Transaction::IsShieldedAuthVersion(7));  // txid commits to the bundle; auth resource envelope applies
    EXPECT_FALSE(Transaction::IsShieldedBundleV2Version(5));
}

TEST(ShieldedV2Codec, EnvelopeRoundTripAndRejections) {
    v2::V2Envelope env{v2::ProofSystemId::SpartanHyrax, std::vector<uint8_t>(300, 0xAB)};
    auto bytes = v2::EncodeV2Envelope(env);
    ASSERT_GE(bytes.size(), 4u + 1u + 2u + 300u);
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "DZV2");
    v2::V2Envelope out;
    EXPECT_EQ(v2::DecodeV2Envelope(bytes, &out), v2::EnvelopeDecodeError::Ok);
    EXPECT_EQ(out.id, v2::ProofSystemId::SpartanHyrax);
    EXPECT_EQ(out.proof, env.proof);

    auto bad_tag = bytes; bad_tag[3] = '1';
    EXPECT_EQ(v2::DecodeV2Envelope(bad_tag, &out), v2::EnvelopeDecodeError::BadTag);
    auto bad_id = bytes; bad_id[4] = 0x02;  // phase 2 id is not accepted in phase 1 decoding? It decodes; validation rejects. Here: unknown id 0x7F.
    bad_id[4] = 0x7F;
    EXPECT_EQ(v2::DecodeV2Envelope(bad_id, &out), v2::EnvelopeDecodeError::UnknownId);
    auto truncated = bytes; truncated.pop_back();
    EXPECT_EQ(v2::DecodeV2Envelope(truncated, &out), v2::EnvelopeDecodeError::LengthMismatch);
    auto trailing = bytes; trailing.push_back(0);
    EXPECT_EQ(v2::DecodeV2Envelope(trailing, &out), v2::EnvelopeDecodeError::LengthMismatch);
    EXPECT_EQ(v2::DecodeV2Envelope(std::vector<uint8_t>(3, 'D'), &out), v2::EnvelopeDecodeError::Truncated);
    v2::V2Envelope huge{v2::ProofSystemId::SpartanHyrax, std::vector<uint8_t>(v2::kV2MaxProofBytes + 1, 0)};
    EXPECT_EQ(v2::DecodeV2Envelope(v2::EncodeV2Envelope(huge), &out), v2::EnvelopeDecodeError::TooLarge);
}

TEST(ShieldedV2Codec, BundleRoundTripIsCanonical) {
    const auto b = SampleBundle();
    const auto bytes = SerializeShieldedBundleV2(b);
    ShieldedBundle out;
    ASSERT_EQ(DeserializeShieldedBundleV2(bytes, &out), BundleDecodeError::Ok);
    EXPECT_EQ(out.value_balance, b.value_balance);
    ASSERT_EQ(out.spends.size(), 2u);
    EXPECT_EQ(out.spends[1].nullifier, H(0x22));
    EXPECT_EQ(out.spends[1].anchor, H(0xA1));
    EXPECT_TRUE(out.spends[1].zk_proof.empty());
    EXPECT_EQ(out.spends[1].cv, ValueCommitment{});
    ASSERT_EQ(out.outputs.size(), 2u);
    EXPECT_EQ(out.outputs[0].encrypted_note.size(), 611u);
    EXPECT_TRUE(out.aggregated_range_proof.empty());
    EXPECT_EQ(out.binding_sig, BindingSignature{});
    EXPECT_EQ(out.v2_proof, b.v2_proof);
    EXPECT_EQ(SerializeShieldedBundleV2(out), bytes);
}

TEST(ShieldedV2Codec, BundleRejectsOrderDuplicatesTrailingAndCaps) {
    auto b = SampleBundle();
    std::swap(b.spends[0], b.spends[1]);  // descending nullifiers
    ShieldedBundle out;
    EXPECT_EQ(DeserializeShieldedBundleV2(SerializeShieldedBundleV2(b), &out), BundleDecodeError::OrderViolation);

    b = SampleBundle();
    b.spends[1].nullifier = b.spends[0].nullifier;  // duplicate
    EXPECT_EQ(DeserializeShieldedBundleV2(SerializeShieldedBundleV2(b), &out), BundleDecodeError::OrderViolation);

    b = SampleBundle();
    auto bytes = SerializeShieldedBundleV2(b);
    bytes.push_back(0);
    EXPECT_EQ(DeserializeShieldedBundleV2(bytes, &out), BundleDecodeError::TrailingBytes);
    bytes = SerializeShieldedBundleV2(b);
    bytes.pop_back();
    EXPECT_EQ(DeserializeShieldedBundleV2(bytes, &out), BundleDecodeError::Truncated);

    b = SampleBundle();
    for (int i = 0; i < 3; ++i) b.spends.push_back({H(0x50 + i), H(0xA1), {}, {}});  // 5 spends > kV2MaxSpends
    EXPECT_EQ(DeserializeShieldedBundleV2(SerializeShieldedBundleV2(b), &out), BundleDecodeError::NotCanonical);

    // A count claiming 2^32 spends must fail before allocating.
    std::vector<uint8_t> hostile(8, 0);
    hostile.push_back(0xFE); hostile.insert(hostile.end(), {0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_NE(DeserializeShieldedBundleV2(hostile, &out), BundleDecodeError::Ok);
}

TEST(ShieldedV2Codec, FrontDoorDispatchesOnVersion) {
    const auto b = SampleBundle();
    const auto v2_bytes = SerializeShieldedBundleForVersion(7, b);
    EXPECT_EQ(v2_bytes, SerializeShieldedBundleV2(b));
    ShieldedBundle out;
    EXPECT_EQ(DeserializeShieldedBundleForVersion(7, v2_bytes, &out), BundleDecodeError::Ok);
    EXPECT_NE(DeserializeShieldedBundleForVersion(6, v2_bytes, &out), BundleDecodeError::Ok);  // v1 codec must not accept v2 bytes
    ShieldedBundle v1; v1.value_balance = 5;
    const auto v1_bytes = SerializeShieldedBundleForVersion(6, v1);
    EXPECT_EQ(v1_bytes, SerializeShieldedBundle(v1));
    EXPECT_NE(DeserializeShieldedBundleForVersion(7, v1_bytes, &out), BundleDecodeError::Ok);
}

TEST(ShieldedV2Codec, VersionSevenSighashHasItsOwnDomain) {
    Transaction tx;
    tx.version = 7;
    tx.shielded_bundle_bytes = SerializeShieldedBundleV2(SampleBundle());
    const auto v7 = ComputeShieldedTxSighash(tx);
    EXPECT_EQ(v7, ComputeShieldedTxSighash(tx, ShieldedProofEncoding::BundleV2));
    EXPECT_NE(v7, ComputeShieldedTxSighash(tx, ShieldedProofEncoding::Full));
    EXPECT_NE(v7, ComputeShieldedTxSighash(tx, ShieldedProofEncoding::CompactV1));
    Transaction six = tx; six.version = 6;
    EXPECT_NE(v7, ComputeShieldedTxSighash(six));
}
}  // namespace
```

Add the target in `tests/CMakeLists.txt` with the same link set as `test_shielded_v2_activation` plus `dinero_tx_primitives dinero_crypto` (copy the link list of `test_compact_activation`, which already links what transaction.h needs), `add_test(NAME ShieldedV2Codec ...)`, labels `shielded;consensus;mandatory`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build-spike --target test_shielded_v2_codec`
Expected: compile errors (`TX_VERSION_SHIELDED_BUNDLE_V2`, `v2/envelope.h` missing).

- [ ] **Step 3: Transaction version 7**

In `include/primitives/transaction.h` after `static constexpr int32_t TX_VERSION_SHIELDED_V2 = 6;`:

```cpp
    // v7 is shielded v2: one bundle proof per transaction in a DZV2 envelope.
    // The bundle uses the v2 wire layout (consensus/shielded/v2/serialization.h)
    // and, like v6, commits into txid. Dormant until shielded_v2_activation_height.
    static constexpr int32_t TX_VERSION_SHIELDED_BUNDLE_V2 = 7;
    static constexpr bool IsShieldedBundleV2Version(int32_t tx_version) {
        return tx_version == TX_VERSION_SHIELDED_BUNDLE_V2;
    }
```

and change `IsShieldedAuthVersion` to:

```cpp
    static constexpr bool IsShieldedAuthVersion(int32_t tx_version) {
        return tx_version == TX_VERSION_SHIELDED_V2 || IsShieldedBundleV2Version(tx_version) ||
               IsCompactRegtestVersion(tx_version);
    }
```

Run: `grep -n -E 'version *== *[56]\b|TX_VERSION_SHIELDED(_V2)?\b' src/primitives/transaction_serializer.cpp src/primitives/transaction.cpp`
Expected: only predicate uses (`IsShieldedVersion`, `IsShieldedAuthVersion`, `ShieldedBundleCommitsToTxid`). If a literal `5`/`6` comparison governs bundle-slot serialization or txid inclusion, extend it with `|| IsShieldedBundleV2Version(version)` and add a test in this file asserting a version-7 transaction with bundle bytes round-trips through `TransactionSerializer` and that changing one bundle byte changes `GetTxid()`.

- [ ] **Step 4: Bundle field**

In `include/consensus/shielded/shielded_tx.h` after `BindingSignature binding_sig{};`:

```cpp
    /// Shielded v2 (tx version 7): the DZV2 envelope carrying the single
    /// bundle proof. Empty for v1 bundles; never part of the v1 wire layout.
    std::vector<uint8_t>             v2_proof;
```

- [ ] **Step 5: Envelope**

```cpp
// include/consensus/shielded/v2/envelope.h
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded::v2 {

constexpr std::array<uint8_t, 4> kV2Tag{'D', 'Z', 'V', '2'};
constexpr size_t kV2MaxSpends = 4;
constexpr size_t kV2MaxOutputs = 2;
constexpr size_t kV2MaxProofBytes = 32768;
constexpr size_t kV2MaxEnvelopeBytes = 4 + 1 + 5 + kV2MaxProofBytes;

enum class ProofSystemId : uint8_t {
    SpartanHyrax = 0x01,   // phase 1
    SpartanHashPcs = 0x02, // phase 2; decodes, never verifies in phase 1
};

struct V2Envelope {
    ProofSystemId id = ProofSystemId::SpartanHyrax;
    std::vector<uint8_t> proof;
};

enum class EnvelopeDecodeError { Ok, Truncated, BadTag, UnknownId, LengthMismatch, TooLarge };

std::vector<uint8_t> EncodeV2Envelope(const V2Envelope& env);
EnvelopeDecodeError DecodeV2Envelope(const std::vector<uint8_t>& bytes, V2Envelope* out);

// Bitcoin CompactSize, minimal encoding required on decode.
void WriteCompactSize(std::vector<uint8_t>& out, uint64_t n);
// Returns false on truncation or non-minimal encoding.
bool ReadCompactSize(const uint8_t* data, size_t len, size_t& pos, uint64_t& out);

}  // namespace dinero::consensus::shielded::v2
```

```cpp
// src/consensus/shielded/v2/envelope.cpp
#include "consensus/shielded/v2/envelope.h"

namespace dinero::consensus::shielded::v2 {

void WriteCompactSize(std::vector<uint8_t>& out, uint64_t n) {
    if (n < 253) { out.push_back(static_cast<uint8_t>(n)); return; }
    if (n <= 0xFFFF) { out.push_back(253); for (int i = 0; i < 2; ++i) out.push_back(static_cast<uint8_t>(n >> (8 * i))); return; }
    if (n <= 0xFFFFFFFFull) { out.push_back(254); for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(n >> (8 * i))); return; }
    out.push_back(255); for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(n >> (8 * i)));
}

bool ReadCompactSize(const uint8_t* data, size_t len, size_t& pos, uint64_t& out) {
    if (pos >= len) return false;
    const uint8_t first = data[pos++];
    size_t width = 0; uint64_t min = 0;
    if (first < 253) { out = first; return true; }
    if (first == 253) { width = 2; min = 253; }
    else if (first == 254) { width = 4; min = 0x10000; }
    else { width = 8; min = 0x100000000ull; }
    if (len - pos < width) return false;
    out = 0;
    for (size_t i = 0; i < width; ++i) out |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
    pos += width;
    return out >= min;  // non-minimal encodings are rejected
}

std::vector<uint8_t> EncodeV2Envelope(const V2Envelope& env) {
    std::vector<uint8_t> out(kV2Tag.begin(), kV2Tag.end());
    out.push_back(static_cast<uint8_t>(env.id));
    WriteCompactSize(out, env.proof.size());
    out.insert(out.end(), env.proof.begin(), env.proof.end());
    return out;
}

EnvelopeDecodeError DecodeV2Envelope(const std::vector<uint8_t>& bytes, V2Envelope* out) {
    if (bytes.size() < 4 + 1 + 1) return EnvelopeDecodeError::Truncated;
    if (!std::equal(kV2Tag.begin(), kV2Tag.end(), bytes.begin())) return EnvelopeDecodeError::BadTag;
    const uint8_t id = bytes[4];
    if (id != static_cast<uint8_t>(ProofSystemId::SpartanHyrax) &&
        id != static_cast<uint8_t>(ProofSystemId::SpartanHashPcs)) return EnvelopeDecodeError::UnknownId;
    size_t pos = 5; uint64_t len = 0;
    if (!ReadCompactSize(bytes.data(), bytes.size(), pos, len)) return EnvelopeDecodeError::Truncated;
    if (len > kV2MaxProofBytes) return EnvelopeDecodeError::TooLarge;
    if (bytes.size() - pos != len) return EnvelopeDecodeError::LengthMismatch;
    out->id = static_cast<ProofSystemId>(id);
    out->proof.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.end());
    return EnvelopeDecodeError::Ok;
}

}  // namespace dinero::consensus::shielded::v2
```

- [ ] **Step 6: v2 bundle codec and front door**

```cpp
// include/consensus/shielded/v2/serialization.h
#pragma once
// Shielded v2 bundle wire layout (transaction version 7):
//   LE64  value_balance
//   CompactSize n_spends   (<= kV2MaxSpends)     then per spend:  nullifier[32] anchor[32]
//   CompactSize n_outputs  (<= kV2MaxOutputs)    then per output: commitment[32] CompactSize note_len note[note_len]
//   CompactSize env_len    (<= kV2MaxEnvelopeBytes) then env[env_len]   (DZV2 envelope, see envelope.h)
// Spends strictly ascending by nullifier, outputs strictly ascending by commitment,
// no trailing bytes, and re-encoding must reproduce the input (NotCanonical otherwise).
// Decoded bundles carry zero cv / bvk / binding_sig and empty zk_proof / aggregated_range_proof.
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_tx.h"
#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded {

std::vector<uint8_t> SerializeShieldedBundleV2(const ShieldedBundle& bundle);
BundleDecodeError DeserializeShieldedBundleV2(const uint8_t* data, size_t len, ShieldedBundle* out);
BundleDecodeError DeserializeShieldedBundleV2(const std::vector<uint8_t>& bytes, ShieldedBundle* out);

// Version-aware front door. Version 7 selects the v2 layout; every other
// version selects the unchanged v1 codec. Callers that hold a Transaction use these.
BundleDecodeError DeserializeShieldedBundleForVersion(int32_t tx_version,
                                                      const std::vector<uint8_t>& bytes,
                                                      ShieldedBundle* out);
std::vector<uint8_t> SerializeShieldedBundleForVersion(int32_t tx_version, const ShieldedBundle& bundle);

}  // namespace dinero::consensus::shielded
```

```cpp
// src/consensus/shielded/v2/serialization.cpp
#include "consensus/shielded/v2/serialization.h"
#include "consensus/shielded/v2/envelope.h"
#include "primitives/transaction.h"
#include <algorithm>
#include <cstring>

namespace dinero::consensus::shielded {
namespace {
void PutU64LE(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void PutHash(std::vector<uint8_t>& out, const Hash& h) { out.insert(out.end(), h.begin(), h.end()); }
bool GetHash(const uint8_t* d, size_t len, size_t& pos, Hash& h) {
    if (len - pos < 32) return false;
    std::memcpy(h.data(), d + pos, 32); pos += 32; return true;
}
}  // namespace

std::vector<uint8_t> SerializeShieldedBundleV2(const ShieldedBundle& b) {
    std::vector<uint8_t> out;
    PutU64LE(out, static_cast<uint64_t>(b.value_balance));
    v2::WriteCompactSize(out, b.spends.size());
    for (const auto& s : b.spends) { PutHash(out, s.nullifier); PutHash(out, s.anchor); }
    v2::WriteCompactSize(out, b.outputs.size());
    for (const auto& o : b.outputs) {
        PutHash(out, o.commitment);
        v2::WriteCompactSize(out, o.encrypted_note.size());
        out.insert(out.end(), o.encrypted_note.begin(), o.encrypted_note.end());
    }
    v2::WriteCompactSize(out, b.v2_proof.size());
    out.insert(out.end(), b.v2_proof.begin(), b.v2_proof.end());
    return out;
}

BundleDecodeError DeserializeShieldedBundleV2(const uint8_t* d, size_t len, ShieldedBundle* out) {
    ShieldedBundle b;
    size_t pos = 0;
    if (len < 8) return BundleDecodeError::Truncated;
    uint64_t vb = 0;
    for (int i = 0; i < 8; ++i) vb |= static_cast<uint64_t>(d[i]) << (8 * i);
    pos = 8;
    b.value_balance = static_cast<int64_t>(vb);

    uint64_t n = 0;
    if (!v2::ReadCompactSize(d, len, pos, n)) return BundleDecodeError::VarintOverflow;
    if (n > v2::kV2MaxSpends) return BundleDecodeError::NotCanonical;
    for (uint64_t i = 0; i < n; ++i) {
        ShieldedSpend s{};
        if (!GetHash(d, len, pos, s.nullifier) || !GetHash(d, len, pos, s.anchor)) return BundleDecodeError::Truncated;
        if (i > 0 && !(b.spends.back().nullifier < s.nullifier)) return BundleDecodeError::OrderViolation;
        b.spends.push_back(std::move(s));
    }
    if (!v2::ReadCompactSize(d, len, pos, n)) return BundleDecodeError::VarintOverflow;
    if (n > v2::kV2MaxOutputs) return BundleDecodeError::NotCanonical;
    for (uint64_t i = 0; i < n; ++i) {
        ShieldedOutput o{};
        if (!GetHash(d, len, pos, o.commitment)) return BundleDecodeError::Truncated;
        uint64_t note_len = 0;
        if (!v2::ReadCompactSize(d, len, pos, note_len)) return BundleDecodeError::VarintOverflow;
        if (note_len > 4096 || len - pos < note_len) return BundleDecodeError::Truncated;
        o.encrypted_note.assign(d + pos, d + pos + note_len); pos += note_len;
        if (i > 0 && !(b.outputs.back().commitment < o.commitment)) return BundleDecodeError::OrderViolation;
        b.outputs.push_back(std::move(o));
    }
    uint64_t env_len = 0;
    if (!v2::ReadCompactSize(d, len, pos, env_len)) return BundleDecodeError::VarintOverflow;
    if (env_len > v2::kV2MaxEnvelopeBytes) return BundleDecodeError::NotCanonical;
    if (len - pos < env_len) return BundleDecodeError::Truncated;
    b.v2_proof.assign(d + pos, d + pos + env_len); pos += env_len;
    if (pos != len) return BundleDecodeError::TrailingBytes;
    const auto re = SerializeShieldedBundleV2(b);
    if (re.size() != len || std::memcmp(re.data(), d, len) != 0) return BundleDecodeError::NotCanonical;
    *out = std::move(b);
    return BundleDecodeError::Ok;
}

BundleDecodeError DeserializeShieldedBundleV2(const std::vector<uint8_t>& bytes, ShieldedBundle* out) {
    return DeserializeShieldedBundleV2(bytes.data(), bytes.size(), out);
}

BundleDecodeError DeserializeShieldedBundleForVersion(int32_t v, const std::vector<uint8_t>& bytes, ShieldedBundle* out) {
    return dinero::Transaction::IsShieldedBundleV2Version(v) ? DeserializeShieldedBundleV2(bytes, out)
                                                             : DeserializeShieldedBundle(bytes, out);
}

std::vector<uint8_t> SerializeShieldedBundleForVersion(int32_t v, const ShieldedBundle& b) {
    return dinero::Transaction::IsShieldedBundleV2Version(v) ? SerializeShieldedBundleV2(b)
                                                             : SerializeShieldedBundle(b);
}
}  // namespace dinero::consensus::shielded
```

`Hash` is `std::array<uint8_t, 32>`, so `operator<` is lexicographic; the test's "duplicate" case fails the strict `<` and reports `OrderViolation`. Add both `.cpp` files to `src/consensus/shielded/CMakeLists.txt` after `compact_spartan_codec.cpp` as `v2/envelope.cpp` and `v2/serialization.cpp`.

- [ ] **Step 7: Sighash encoding**

`include/consensus/shielded/binding_sig.h:100`: `enum class ShieldedProofEncoding { Full, CompactV1, BundleV2 };`

`src/consensus/shielded/binding_sig.cpp`, first function body becomes:

```cpp
Hash ComputeShieldedTxSighash(const ::dinero::Transaction& tx) {
    if (Transaction::IsShieldedBundleV2Version(tx.version))
        return ComputeShieldedTxSighash(tx, ShieldedProofEncoding::BundleV2);
    ShieldedBundle bundle;
    const bool compact = tx.version == Transaction::TX_VERSION_SHIELDED_V2 &&
        DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle) == BundleDecodeError::Ok &&
        HasCompactProofs(bundle);
    return ComputeShieldedTxSighash(tx, compact ? ShieldedProofEncoding::CompactV1
                                              : ShieldedProofEncoding::Full);
}
```

and in the two-argument function add `constexpr const char kBundleV2[] = "DIN/v7/shielded/tx-sighash/bundle-v2";` and select it: `if (encoding == ShieldedProofEncoding::BundleV2) b.Add(kBundleV2, sizeof(kBundleV2) - 1); else if (encoding == ShieldedProofEncoding::CompactV1) ... else ...`. Nothing else in the preimage changes.

- [ ] **Step 8: Switch the eight decode call sites to the front door**

At each site replace `DeserializeShieldedBundle(X.shielded_bundle_bytes, &bundle)` with `DeserializeShieldedBundleForVersion(X.version, X.shielded_bundle_bytes, &bundle)` where `X` is the transaction in scope (`tx`, or `entry.tx` at `mempool.cpp:3345`), adding `#include "consensus/shielded/v2/serialization.h"`. Sites: `src/consensus/block_validation.cpp:149`, `:683`; `src/daemon/mempool.cpp:2461`, `:3331`, `:3345`; `src/consensus/reindexer.cpp:2431`; `include/consensus/shielded/resource_limits.h:89`, `:104`. The `binding_sig.cpp:46` site stays on the v1 codec by design (it is inside the `version == 6` compact check).

Run: `grep -rn 'DeserializeShieldedBundle(' src include --include='*.cpp' --include='*.h' | grep -v -E 'v2/serialization|shielded_serialization|binding_sig.cpp|ForVersion|DeserializeShieldedBundleV2'`
Expected: no output.

- [ ] **Step 9: Run the tests**

Run: `cmake --build build-spike --target test_shielded_v2_codec test_shielded_validation test_shielded_serialization_oom dinerod && ./build-spike/test_shielded_v2_codec && ctest --test-dir build-spike -R 'ShieldedValidation$|ShieldedSerializationOom|CompactProductionV6Vectors' --output-on-failure`
Expected: all PASS. The v1 vector test proves v6 bytes still decode and validate identically.

- [ ] **Step 10: Commit**

```bash
git add include/primitives/transaction.h include/consensus/shielded/shielded_tx.h include/consensus/shielded/v2 src/consensus/shielded/v2 src/consensus/shielded/CMakeLists.txt include/consensus/shielded/binding_sig.h src/consensus/shielded/binding_sig.cpp src/consensus/block_validation.cpp src/daemon/mempool.cpp src/consensus/reindexer.cpp include/consensus/shielded/resource_limits.h tests/consensus/test_shielded_v2_codec.cpp tests/CMakeLists.txt
git commit -m "shielded-v2: tx version 7, DZV2 envelope, v2 bundle codec, bundle-v2 sighash domain

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: v2 keys, bundle circuit, prover and verifier

**Files:**
- Create: `include/consensus/shielded/v2/keys.h`, `src/consensus/shielded/v2/keys.cpp`
- Create: `include/consensus/shielded/v2/bundle_circuit.h`, `src/consensus/shielded/v2/bundle_circuit.cpp`
- Modify: `src/consensus/shielded/CMakeLists.txt` (add `v2/keys.cpp`, `v2/bundle_circuit.cpp`)
- Test: `tests/consensus/test_shielded_v2_circuit.cpp` (target `test_shielded_v2_circuit`, ctest `ShieldedV2Circuit`, label `shielded;consensus;mandatory`, TIMEOUT 600)

**Interfaces:**
- Consumes: `v2::EncodeV2Envelope/DecodeV2Envelope`, `kV2Max*` (Task 2); `PoseidonHash2`, `NoteCommitment`, `ComputeNullifier`, `AddrBindTag`, `NullifierKeyTag`, `AuthRecipientCommitmentKey`, `CommitmentTree`, `TREE_DEPTH` (`consensus/shielded/commitment_tree.h`); `poseidon2_gadget`, `gadgets::*`, `R1CS`, `Transcript`, `r1cs_spartan_prove/verify`, `spartan_hash_r1cs_structure`, `GeneratorSet::cached`, `HyraxParams::from_n` (`src/zk/zkvm/*.h`); `VerifiedProofCache<1024>` (`src/consensus/shielded/proof_verification_cache.h`).
- Produces (`namespace dinero::consensus::shielded::v2`):
  - `const Hash& HashSpendKeyTagV2();` `Hash HashSpendKeyV2(const Hash& ask);` `Hash DiversifiedSpendPublicKeyV2(const Hash& hk, const Hash& d_padded);`
  - `struct BundleSpendWitness { Hash ask, nullifier_key, d, value, randomness; uint64_t leaf_index; std::array<Hash, TREE_DEPTH> merkle_path; };`
  - `struct BundleOutputWitness { Hash value, public_key, randomness, d; };`
  - `struct BundleWitness { std::vector<BundleSpendWitness> spends; std::vector<BundleOutputWitness> outputs; };`
  - `struct BundlePublicInputs { Hash sighash; uint64_t vb_pos, vb_neg; std::vector<Hash> anchors, nullifiers, commitments; static BundlePublicInputs FromBundle(const ShieldedBundle&, const Hash& sighash); };`
  - `struct BundleCircuitNeuter { bool omit_balance, omit_range, omit_sighash; };` (test-only)
  - `zk::zkvm::R1CS BuildBundleCircuitV2(const BundleWitness&, const BundlePublicInputs&, BundleCircuitNeuter = {});`
  - `void BindBundleTranscriptV2(zk::zkvm::Transcript&, const BundlePublicInputs&, bool absorb_sighash = true);`
  - `std::vector<uint8_t> ProveBundleV2(const BundleWitness&, const BundlePublicInputs&, secp256k1_context*);` (returns the DZV2 envelope, empty on failure)
  - `bool VerifyBundleV2(const std::vector<uint8_t>& envelope, const BundlePublicInputs&, secp256k1_context*);` (cache-aware)
  - `Hash BundleProofCacheKeyV2(const std::vector<uint8_t>& envelope, const BundlePublicInputs&);`
  - `constexpr const char* kBundleTranscriptDomainV2 = "dinero.shielded.bundle.v2";`

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/consensus/test_shielded_v2_circuit.cpp
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/v2/bundle_circuit.h"
#include "consensus/shielded/v2/envelope.h"
#include "consensus/shielded/v2/keys.h"
#include "crypto/evp_secp256k1.h"
#include "zk/zkvm/r1cs.h"
#include <gtest/gtest.h>
#include <cstring>

namespace {
using namespace dinero::consensus::shielded;
namespace v2 = dinero::consensus::shielded::v2;

Hash H(uint8_t seed, uint8_t tail = 0xCD) { Hash h{}; h[0] = seed; h[31] = tail; return h; }
Hash U64(uint64_t v) { Hash h{}; for (int i = 0; i < 8; ++i) h[31 - i] = static_cast<uint8_t>(v >> (8 * i)); return h; }

struct Fixture {
    CommitmentTree tree;
    v2::BundleWitness w;
    v2::BundlePublicInputs pub;
    ShieldedBundle bundle;  // what a transaction would carry (without v2_proof)
};

// n_in notes owned by (ask, nvk-derived nfk), spent into n_out outputs, with value_balance vb.
Fixture Make(size_t n_in, size_t n_out, int64_t vb, uint8_t seed = 1) {
    Fixture f;
    f.pub.sighash = H(0x51, seed);
    f.pub.vb_pos = vb > 0 ? static_cast<uint64_t>(vb) : 0;
    f.pub.vb_neg = vb < 0 ? static_cast<uint64_t>(-vb) : 0;
    f.bundle.value_balance = vb;
    const uint64_t each = 1'000'000;
    for (size_t i = 0; i < n_in; ++i) {
        v2::BundleSpendWitness s{};
        s.ask = H(0x10 + i, seed); s.nullifier_key = H(0x20 + i, seed); s.d = H(0x30 + i, seed);
        s.value = U64(each); s.randomness = H(0x40 + i, seed);
        const Hash hk = v2::HashSpendKeyV2(s.ask);
        const Hash pk_d = v2::DiversifiedSpendPublicKeyV2(hk, s.d);
        const Hash nfk_c = PoseidonHash2(s.nullifier_key, NullifierKeyTag());
        const Hash pk = AuthRecipientCommitmentKey(pk_d, nfk_c);
        const Hash cm = NoteCommitment(s.d, pk, s.value, s.randomness);
        s.leaf_index = f.tree.Append(cm);
        f.w.spends.push_back(s);
    }
    for (auto& s : f.w.spends) {
        s.merkle_path = f.tree.GetAuthPath(s.leaf_index)->siblings;
        f.pub.anchors.push_back(f.tree.Root());
        f.pub.nullifiers.push_back(ComputeNullifier(s.nullifier_key, s.leaf_index));
        f.bundle.spends.push_back({f.pub.nullifiers.back(), f.pub.anchors.back(), {}, {}});
    }
    const uint64_t total_out = each * n_in + f.pub.vb_pos - f.pub.vb_neg;
    for (size_t j = 0; j < n_out; ++j) {
        v2::BundleOutputWitness o{};
        const uint64_t v = (j + 1 == n_out) ? total_out - (total_out / n_out) * (n_out - 1) : total_out / n_out;
        o.value = U64(v); o.public_key = H(0x60 + j, seed); o.randomness = H(0x70 + j, seed); o.d = H(0x80 + j, seed);
        f.pub.commitments.push_back(NoteCommitment(o.d, o.public_key, o.value, o.randomness));
        f.bundle.outputs.push_back({f.pub.commitments.back(), {}, std::vector<uint8_t>(611, 0xEE), {}});
        f.w.outputs.push_back(o);
    }
    return f;
}

secp256k1_context* Ctx() { return dinero::crypto::GetSecp256k1ContextSignVerify(); }

TEST(ShieldedV2Circuit, KeyDerivationIsPoseidonWithPinnedTag) {
    const Hash ask = H(0x01);
    Hash tag{}; std::memcpy(tag.data(), "DIN/v7/shielded/v2/hk", 21);
    EXPECT_EQ(v2::HashSpendKeyTagV2(), tag);
    EXPECT_EQ(v2::HashSpendKeyV2(ask), PoseidonHash2(ask, tag));
    EXPECT_EQ(v2::DiversifiedSpendPublicKeyV2(H(0x02), H(0x03)), PoseidonHash2(H(0x02), H(0x03)));
}

TEST(ShieldedV2Circuit, HonestBundlesProveAndVerify) {
    for (auto [n_in, n_out, vb] : {std::tuple{1u, 2u, int64_t{-1000}}, std::tuple{2u, 2u, int64_t{-1000}},
                                   std::tuple{4u, 2u, int64_t{-1000}}, std::tuple{0u, 1u, int64_t{500000}},
                                   std::tuple{1u, 0u, int64_t{-1000000}}}) {
        SCOPED_TRACE(std::to_string(n_in) + "-in-" + std::to_string(n_out) + "-out");
        auto f = Make(n_in, n_out, vb);
        auto cs = v2::BuildBundleCircuitV2(f.w, f.pub);
        EXPECT_TRUE(cs.is_satisfied());
        auto env = v2::ProveBundleV2(f.w, f.pub, Ctx());
        ASSERT_FALSE(env.empty());
        EXPECT_LE(env.size(), 20480u);
        EXPECT_TRUE(v2::VerifyBundleV2(env, f.pub, Ctx()));
        EXPECT_TRUE(v2::VerifyBundleV2(env, f.pub, Ctx())) << "cache hit path";
    }
}

TEST(ShieldedV2Circuit, FromBundleMatchesFixtureOrder) {
    auto f = Make(2, 2, -1000);
    const auto pub = v2::BundlePublicInputs::FromBundle(f.bundle, f.pub.sighash);
    EXPECT_EQ(pub.vb_pos, f.pub.vb_pos);
    EXPECT_EQ(pub.vb_neg, f.pub.vb_neg);
    EXPECT_EQ(pub.anchors, f.pub.anchors);
    EXPECT_EQ(pub.nullifiers, f.pub.nullifiers);
    EXPECT_EQ(pub.commitments, f.pub.commitments);
    ShieldedBundle pos = f.bundle; pos.value_balance = 7;
    const auto p2 = v2::BundlePublicInputs::FromBundle(pos, f.pub.sighash);
    EXPECT_EQ(p2.vb_pos, 7u); EXPECT_EQ(p2.vb_neg, 0u);
}

TEST(ShieldedV2Circuit, EveryPublicInputMutationFails) {
    auto f = Make(2, 2, -1000);
    const auto env = v2::ProveBundleV2(f.w, f.pub, Ctx());
    ASSERT_FALSE(env.empty());
    auto mutated = [&](auto fn) { auto p = f.pub; fn(p); return v2::VerifyBundleV2(env, p, Ctx()); };
    EXPECT_FALSE(mutated([](auto& p) { p.sighash[0] ^= 1; })) << "sighash binding";
    EXPECT_FALSE(mutated([](auto& p) { p.vb_neg += 1; })) << "fee / balance";
    EXPECT_FALSE(mutated([](auto& p) { p.vb_pos += 1; p.vb_neg += 1; })) << "both sides shift";
    EXPECT_FALSE(mutated([](auto& p) { p.anchors[0][5] ^= 1; })) << "anchor";
    EXPECT_FALSE(mutated([](auto& p) { p.nullifiers[1][5] ^= 1; })) << "nullifier";
    EXPECT_FALSE(mutated([](auto& p) { p.commitments[0][5] ^= 1; })) << "commitment";
    EXPECT_FALSE(mutated([](auto& p) { std::swap(p.nullifiers[0], p.nullifiers[1]); })) << "order";
    EXPECT_FALSE(mutated([](auto& p) { p.commitments.pop_back(); })) << "count";
}

TEST(ShieldedV2Circuit, ProofBytesMutationsFail) {
    auto f = Make(1, 2, -1000);
    auto env = v2::ProveBundleV2(f.w, f.pub, Ctx());
    ASSERT_FALSE(env.empty());
    auto bad = env; bad[env.size() / 2] ^= 0x01;
    EXPECT_FALSE(v2::VerifyBundleV2(bad, f.pub, Ctx()));
    bad = env; bad[4] = 0x02;  // phase-2 id: decodes, must not verify in phase 1
    EXPECT_FALSE(v2::VerifyBundleV2(bad, f.pub, Ctx()));
    bad = env; bad[4] = 0x7F;
    EXPECT_FALSE(v2::VerifyBundleV2(bad, f.pub, Ctx()));
    bad = env; bad.resize(bad.size() - 1);
    EXPECT_FALSE(v2::VerifyBundleV2(bad, f.pub, Ctx()));
    EXPECT_FALSE(v2::VerifyBundleV2({}, f.pub, Ctx()));
    // The cache must not have learned any of the failures.
    auto f2 = Make(1, 2, -1000, /*seed=*/9);
    EXPECT_FALSE(v2::VerifyBundleV2(env, f2.pub, Ctx()));
}

TEST(ShieldedV2Circuit, DishonestWitnessesCannotProve) {
    auto f = Make(2, 2, -1000);
    auto w = f.w; w.outputs[0].value = U64(2'000'000);  // creates value
    EXPECT_FALSE(v2::BuildBundleCircuitV2(w, f.pub).is_satisfied());
    EXPECT_TRUE(v2::ProveBundleV2(w, f.pub, Ctx()).empty());
    w = f.w; w.spends[0].ask[0] ^= 1;                  // wrong owner
    EXPECT_FALSE(v2::BuildBundleCircuitV2(w, f.pub).is_satisfied());
    w = f.w; w.spends[0].nullifier_key[0] ^= 1;        // wrong nullifier key (also changes nf)
    EXPECT_FALSE(v2::BuildBundleCircuitV2(w, f.pub).is_satisfied());
    w = f.w; w.spends[1].merkle_path[3][0] ^= 1;       // not in tree
    EXPECT_FALSE(v2::BuildBundleCircuitV2(w, f.pub).is_satisfied());
}

// Spec §6.2: each safety constraint is load-bearing. With the constraint removed the
// dishonest witness satisfies the circuit; with it present it does not.
TEST(ShieldedV2Circuit, NeuterTestsProveConstraintsAreLoadBearing) {
    auto f = Make(2, 2, -1000);
    auto w = f.w; w.outputs[0].value = U64(2'000'000);
    EXPECT_FALSE(v2::BuildBundleCircuitV2(w, f.pub).is_satisfied());
    EXPECT_TRUE(v2::BuildBundleCircuitV2(w, f.pub, {.omit_balance = true}).is_satisfied());

    // Wraparound: a huge "value" that only the range check catches.
    w = f.w;
    Hash big{}; big.fill(0xFF); big[0] = 0x7F;  // > 2^64, < field order
    w.outputs[0].value = big;
    // Rebuild the commitment so only the range check can reject.
    auto pub = f.pub;
    pub.commitments[0] = NoteCommitment(w.outputs[0].d, w.outputs[0].public_key, big, w.outputs[0].randomness);
    // Make the balance hold mod p: in[0] + in[1] = out[0] + out[1] + vb_neg  ⇒ set out[1] = in_sum - vb_neg - big (mod p).
    // Simpler: keep out[1] and let balance fail; then assert the range check alone rejects a balance-satisfying pair.
    EXPECT_FALSE(v2::BuildBundleCircuitV2(w, pub, {.omit_range = true, .omit_balance = true}).is_satisfied() &&
                 !v2::BuildBundleCircuitV2(w, pub, {.omit_balance = true}).is_satisfied())
        << "with balance omitted the only difference is the range check; the range check must reject";
    EXPECT_TRUE(v2::BuildBundleCircuitV2(w, pub, {.omit_range = true, .omit_balance = true}).is_satisfied());
    EXPECT_FALSE(v2::BuildBundleCircuitV2(w, pub, {.omit_balance = true}).is_satisfied());

    // Sighash: with the constraint and the transcript absorption removed, a proof made
    // for one sighash verifies under another (the neutered verifier), proving the
    // binding is what stops replay.
    auto cs_a = v2::BuildBundleCircuitV2(f.w, f.pub, {.omit_sighash = true});
    EXPECT_TRUE(cs_a.is_satisfied());
    // (The prove/verify path with neuter flags is exercised through the internal
    //  ProveBundleV2Neutered/VerifyBundleV2Neutered test hooks declared in bundle_circuit.h
    //  under DINERO_SHIELDED_V2_TEST_HOOKS.)
}
}  // namespace
```

Register the target in `tests/CMakeLists.txt` with the link set of `spike_shielded_v2_bench` (`dinero_shielded dinero_wallet wallet_test_deps dinero_zk sqlite3` + gtest + `-framework Security` on Apple), `target_compile_definitions(... PRIVATE DINERO_SHIELDED_V2_TEST_HOOKS=1)`, include dirs `include`, `src`, googletest include, sqlite include. `add_test(NAME ShieldedV2Circuit COMMAND test_shielded_v2_circuit)` with `TIMEOUT 600`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build-spike --target test_shielded_v2_circuit`
Expected: compile error, `v2/keys.h` missing.

- [ ] **Step 3: Keys**

```cpp
// include/consensus/shielded/v2/keys.h
#pragma once
#include "consensus/shielded/commitment_tree.h"

namespace dinero::consensus::shielded::v2 {
// ASCII "DIN/v7/shielded/v2/hk" left-aligned in 32 bytes, zero padded (same shape as AddrBindTag()).
const Hash& HashSpendKeyTagV2();
// hk = Poseidon2(ask, HK_TAG). Account-level; part of the v2 full viewing key.
Hash HashSpendKeyV2(const Hash& ask);
// pk_d_spend = Poseidon2(hk, d_padded), d_padded = 11-byte diversifier zero-padded to 32 bytes.
Hash DiversifiedSpendPublicKeyV2(const Hash& hk, const Hash& d_padded);
}  // namespace dinero::consensus::shielded::v2
```

```cpp
// src/consensus/shielded/v2/keys.cpp
#include "consensus/shielded/v2/keys.h"
#include <cstring>

namespace dinero::consensus::shielded::v2 {
const Hash& HashSpendKeyTagV2() {
    static const Hash tag = [] {
        static constexpr char kTag[] = "DIN/v7/shielded/v2/hk";
        Hash h{};
        std::memcpy(h.data(), kTag, sizeof(kTag) - 1);
        return h;
    }();
    return tag;
}
Hash HashSpendKeyV2(const Hash& ask) { return PoseidonHash2(ask, HashSpendKeyTagV2()); }
Hash DiversifiedSpendPublicKeyV2(const Hash& hk, const Hash& d_padded) { return PoseidonHash2(hk, d_padded); }
}  // namespace dinero::consensus::shielded::v2
```

- [ ] **Step 4: Circuit header**

```cpp
// include/consensus/shielded/v2/bundle_circuit.h
#pragma once
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_tx.h"
#include <array>
#include <cstdint>
#include <vector>

struct secp256k1_context_struct;
namespace dinero::zk::zkvm { class R1CS; class Transcript; }

namespace dinero::consensus::shielded::v2 {

constexpr const char* kBundleTranscriptDomainV2 = "dinero.shielded.bundle.v2";

struct BundleSpendWitness {
    Hash ask{};             ///< account spend authority; hk = Poseidon2(ask, HK_TAG) in-circuit
    Hash nullifier_key{};   ///< nfk = Poseidon2(nvk, d_padded) (wallet-derived, as Auth)
    Hash d{};               ///< 11-byte diversifier zero-padded to 32
    Hash value{};           ///< u64 big-endian in the low 8 bytes (ValueToHash)
    Hash randomness{};      ///< rcm
    uint64_t leaf_index = 0;
    std::array<Hash, TREE_DEPTH> merkle_path{};
};
struct BundleOutputWitness {
    Hash value{}; Hash public_key{}; Hash randomness{}; Hash d{};
};
struct BundleWitness {
    std::vector<BundleSpendWitness> spends;
    std::vector<BundleOutputWitness> outputs;
};
struct BundlePublicInputs {
    Hash sighash{};                 ///< ComputeShieldedTxSighash(tx), version 7 domain
    uint64_t vb_pos = 0;            ///< value_balance if > 0 (transparent value entering the pool)
    uint64_t vb_neg = 0;            ///< -value_balance if < 0 (leaving the pool, fee included)
    std::vector<Hash> anchors;      ///< one per spend, bundle order
    std::vector<Hash> nullifiers;   ///< one per spend, bundle order
    std::vector<Hash> commitments;  ///< one per output, bundle order
    static BundlePublicInputs FromBundle(const ShieldedBundle& bundle, const Hash& sighash);
};
/// Test-only: removes a constraint so tests can show it is load-bearing. Production passes {}.
struct BundleCircuitNeuter { bool omit_balance = false; bool omit_range = false; bool omit_sighash = false; };

zk::zkvm::R1CS BuildBundleCircuitV2(const BundleWitness& w, const BundlePublicInputs& pub,
                                    BundleCircuitNeuter neuter = {});
void BindBundleTranscriptV2(zk::zkvm::Transcript& t, const BundlePublicInputs& pub, bool absorb_sighash = true);
/// Returns the DZV2 envelope (id 0x01), or empty if the witness does not satisfy the statement,
/// the shape exceeds kV2MaxSpends/kV2MaxOutputs, or the envelope would exceed kV2MaxEnvelopeBytes.
std::vector<uint8_t> ProveBundleV2(const BundleWitness& w, const BundlePublicInputs& pub,
                                   secp256k1_context_struct* ctx);
/// Decodes the envelope, rejects any id other than SpartanHyrax, verifies, and records
/// successes in the v2 VerifiedProofCache. Failures are never cached.
bool VerifyBundleV2(const std::vector<uint8_t>& envelope, const BundlePublicInputs& pub,
                    secp256k1_context_struct* ctx);
Hash BundleProofCacheKeyV2(const std::vector<uint8_t>& envelope, const BundlePublicInputs& pub);

#ifdef DINERO_SHIELDED_V2_TEST_HOOKS
std::vector<uint8_t> ProveBundleV2Neutered(const BundleWitness&, const BundlePublicInputs&,
                                           secp256k1_context_struct*, BundleCircuitNeuter);
bool VerifyBundleV2Neutered(const std::vector<uint8_t>&, const BundlePublicInputs&,
                            secp256k1_context_struct*, BundleCircuitNeuter);
#endif
}  // namespace dinero::consensus::shielded::v2
```

- [ ] **Step 5: Circuit, prover, verifier**

```cpp
// src/consensus/shielded/v2/bundle_circuit.cpp
#include "consensus/shielded/v2/bundle_circuit.h"
#include "consensus/shielded/v2/envelope.h"
#include "consensus/shielded/v2/keys.h"
#include "consensus/shielded/proof_verification_cache.h"
#include "crypto/sha256.h"
#include "zk/zkvm/gadgets.h"
#include "zk/zkvm/hyrax.h"
#include "zk/zkvm/ipa.h"
#include "zk/zkvm/poseidon_gadget.h"
#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/r1cs_spartan.h"
#include "zk/zkvm/transcript.h"
#include <algorithm>
#include <mutex>
#include <string>

namespace dinero::consensus::shielded::v2 {
namespace {
using zk::zkvm::LinearCombination;
using zk::zkvm::R1CS;
using zk::zkvm::Scalar;
using zk::zkvm::Transcript;
using zk::zkvm::Variable;
using zk::zkvm::poseidon2_gadget;
namespace gadgets = zk::zkvm::gadgets;

Scalar S(const Hash& h) { return Scalar(h.data()); }               // same reduction as legacy HashToScalar
Scalar U64(uint64_t v) { return Scalar(v); }                        // same as legacy U64ToScalar

Variable MerklePath(R1CS& cs, Variable leaf, Variable idx, const std::array<Hash, TREE_DEPTH>& sib, const std::string& p) {
    Variable cur = leaf;
    const auto bits = gadgets::to_bits(cs, idx, TREE_DEPTH, p + "_idx_bits");
    for (size_t d = 0; d < TREE_DEPTH; ++d) {
        Variable s = cs.alloc(S(sib[d]));
        Variable l = gadgets::select(cs, bits[d], s, cur, p + "_l" + std::to_string(d));
        Variable r = gadgets::select(cs, bits[d], cur, s, p + "_r" + std::to_string(d));
        cur = poseidon2_gadget(cs, l, r, p + "_h" + std::to_string(d));
    }
    return cur;
}
// cm = P(P(P(ADDR_TAG, P(d, pk)), value), rcm) — identical to NoteCommitment().
Variable NoteCm(R1CS& cs, Variable addr_tag, Variable d, Variable pk, Variable val, Variable rnd, const std::string& p) {
    Variable dpk = poseidon2_gadget(cs, d, pk, p + "_dpk");
    Variable bind = poseidon2_gadget(cs, addr_tag, dpk, p + "_bind");
    Variable inner = poseidon2_gadget(cs, bind, val, p + "_inner");
    return poseidon2_gadget(cs, inner, rnd, p + "_cm");
}

size_t GensFor(const R1CS& cs) {
    using zk::zkvm::HyraxParams;
    return std::max<size_t>(4, std::max(HyraxParams::from_n(cs.num_variables()).n_cols,
                                        HyraxParams::from_n(cs.num_constraints()).n_cols));
}
BundleWitness ZeroWitnessShapedLike(const BundlePublicInputs& pub) {
    BundleWitness w;
    w.spends.resize(pub.anchors.size());
    w.outputs.resize(pub.commitments.size());
    return w;
}
bool ShapeOk(const BundleWitness& w, const BundlePublicInputs& pub) {
    return w.spends.size() == pub.anchors.size() && w.spends.size() == pub.nullifiers.size() &&
           w.outputs.size() == pub.commitments.size() && w.spends.size() <= kV2MaxSpends &&
           w.outputs.size() <= kV2MaxOutputs && !(w.spends.empty() && w.outputs.empty());
}

VerifiedProofCache<1024>& Cache() { static VerifiedProofCache<1024> c; return c; }

std::vector<uint8_t> ProveImpl(const BundleWitness& w, const BundlePublicInputs& pub,
                               secp256k1_context_struct* ctx, BundleCircuitNeuter n) {
    if (!ShapeOk(w, pub)) return {};
    R1CS cs = BuildBundleCircuitV2(w, pub, n);
    if (!cs.is_satisfied()) return {};
    const auto& gens = zk::zkvm::GeneratorSet::cached(GensFor(cs), ctx);
    Transcript t(kBundleTranscriptDomainV2);
    BindBundleTranscriptV2(t, pub, !n.omit_sighash);
    auto proof = zk::zkvm::r1cs_spartan_prove(cs, std::vector<Scalar>(cs.num_constraints(), Scalar::zero()),
                                              Scalar::one(), gens, t, ctx, true);
    auto env = EncodeV2Envelope({ProofSystemId::SpartanHyrax, proof.serialize(ctx)});
    return env.size() <= kV2MaxEnvelopeBytes ? env : std::vector<uint8_t>{};
}

bool VerifyImpl(const std::vector<uint8_t>& envelope, const BundlePublicInputs& pub,
                secp256k1_context_struct* ctx, BundleCircuitNeuter n) {
    if (pub.anchors.size() != pub.nullifiers.size() || pub.anchors.size() > kV2MaxSpends ||
        pub.commitments.size() > kV2MaxOutputs || (pub.anchors.empty() && pub.commitments.empty())) return false;
    if (envelope.size() > kV2MaxEnvelopeBytes) return false;
    V2Envelope env;
    if (DecodeV2Envelope(envelope, &env) != EnvelopeDecodeError::Ok) return false;
    if (env.id != ProofSystemId::SpartanHyrax) return false;   // phase 2 id fails closed in phase 1
    const Hash key = BundleProofCacheKeyV2(envelope, pub);
    if (Cache().Contains(key)) return true;
    zk::zkvm::SpartanProof proof;
    if (!zk::zkvm::SpartanProof::deserialize(env.proof, proof, ctx)) return false;
    R1CS vcs = BuildBundleCircuitV2(ZeroWitnessShapedLike(pub), pub, n);
    const auto& gens = zk::zkvm::GeneratorSet::cached(GensFor(vcs), ctx);
    Transcript t(kBundleTranscriptDomainV2);
    BindBundleTranscriptV2(t, pub, !n.omit_sighash);
    const bool ok = zk::zkvm::r1cs_spartan_verify(proof, vcs, vcs.num_constraints(), vcs.num_variables(),
                                                  zk::zkvm::spartan_hash_r1cs_structure(vcs), Scalar::one(),
                                                  gens, t, ctx, true, true);
    if (ok) Cache().RememberVerified(key);
    return ok;
}
}  // namespace

BundlePublicInputs BundlePublicInputs::FromBundle(const ShieldedBundle& b, const Hash& sighash) {
    BundlePublicInputs p;
    p.sighash = sighash;
    p.vb_pos = b.value_balance > 0 ? static_cast<uint64_t>(b.value_balance) : 0;
    p.vb_neg = b.value_balance < 0 ? static_cast<uint64_t>(-(b.value_balance + 1)) + 1 : 0;  // safe for INT64_MIN
    for (const auto& s : b.spends) { p.anchors.push_back(s.anchor); p.nullifiers.push_back(s.nullifier); }
    for (const auto& o : b.outputs) p.commitments.push_back(o.commitment);
    return p;
}

R1CS BuildBundleCircuitV2(const BundleWitness& w, const BundlePublicInputs& pub, BundleCircuitNeuter n) {
    R1CS cs;
    // Public inputs first, fixed order (see plan Global Constraints).
    Variable sighash = cs.alloc_input(S(pub.sighash));
    Variable vb_pos = cs.alloc_input(U64(pub.vb_pos));
    Variable vb_neg = cs.alloc_input(U64(pub.vb_neg));
    std::vector<Variable> anchors, nullifiers, commitments;
    for (size_t i = 0; i < pub.anchors.size(); ++i) {
        anchors.push_back(cs.alloc_input(S(pub.anchors[i])));
        nullifiers.push_back(cs.alloc_input(S(pub.nullifiers[i])));
    }
    for (const auto& c : pub.commitments) commitments.push_back(cs.alloc_input(S(c)));
    // The sighash column must be non-zero in the matrices or its value is free.
    if (!n.omit_sighash)
        cs.constrain(LinearCombination(sighash), LinearCombination::constant(Scalar::one()),
                     LinearCombination(sighash), "sighash_bound");

    Variable hk_tag = gadgets::constant(cs, S(HashSpendKeyTagV2()), "hk_tag");
    Variable nfk_tag = gadgets::constant(cs, S(NullifierKeyTag()), "nfk_tag");
    Variable addr_tag = gadgets::constant(cs, S(AddrBindTag()), "addr_tag");

    Variable sum_in = vb_pos;
    for (size_t i = 0; i < w.spends.size(); ++i) {
        const auto& s = w.spends[i];
        const std::string p = "spend" + std::to_string(i);
        Variable ask = cs.alloc(S(s.ask));
        Variable nfk = cs.alloc(S(s.nullifier_key));
        Variable d = cs.alloc(S(s.d));
        Variable val = cs.alloc(S(s.value));
        Variable rnd = cs.alloc(S(s.randomness));
        Variable idx = cs.alloc(U64(s.leaf_index));
        Variable hk = poseidon2_gadget(cs, ask, hk_tag, p + "_hk");
        Variable pk_d = poseidon2_gadget(cs, hk, d, p + "_pkd");
        Variable nfk_c = poseidon2_gadget(cs, nfk, nfk_tag, p + "_nfkc");
        Variable pk = poseidon2_gadget(cs, pk_d, nfk_c, p + "_pk");
        Variable cm = NoteCm(cs, addr_tag, d, pk, val, rnd, p);
        Variable root = MerklePath(cs, cm, idx, s.merkle_path, p);
        gadgets::assert_equal(cs, root, anchors[i], p + "_anchor");
        Variable nf = poseidon2_gadget(cs, nfk, idx, p + "_nf");
        gadgets::assert_equal(cs, nf, nullifiers[i], p + "_nullifier");
        if (!n.omit_range) gadgets::range_check(cs, val, 64, p + "_range");
        sum_in = gadgets::add(cs, sum_in, val, p + "_sum");
    }
    Variable sum_out = vb_neg;
    for (size_t j = 0; j < w.outputs.size(); ++j) {
        const auto& o = w.outputs[j];
        const std::string p = "out" + std::to_string(j);
        Variable val = cs.alloc(S(o.value)), pk = cs.alloc(S(o.public_key)), rnd = cs.alloc(S(o.randomness)),
                 d = cs.alloc(S(o.d));
        Variable cm = NoteCm(cs, addr_tag, d, pk, val, rnd, p);
        gadgets::assert_equal(cs, cm, commitments[j], p + "_cm");
        if (!n.omit_range) gadgets::range_check(cs, val, 64, p + "_range");
        sum_out = gadgets::add(cs, sum_out, val, p + "_sum");
    }
    if (!n.omit_range) {
        gadgets::range_check(cs, vb_pos, 64, "vb_pos_range");
        gadgets::range_check(cs, vb_neg, 64, "vb_neg_range");
    }
    if (!n.omit_balance) gadgets::assert_equal(cs, sum_in, sum_out, "balance");
    return cs;
}

void BindBundleTranscriptV2(Transcript& t, const BundlePublicInputs& pub, bool absorb_sighash) {
    if (absorb_sighash) t.append_scalar("sighash", S(pub.sighash));
    t.append_u64("vb_pos", pub.vb_pos);
    t.append_u64("vb_neg", pub.vb_neg);
    t.append_u64("n_spends", pub.anchors.size());
    t.append_u64("n_outputs", pub.commitments.size());
    for (size_t i = 0; i < pub.anchors.size(); ++i) {
        t.append_scalar("an", S(pub.anchors[i]));
        t.append_scalar("nf", S(pub.nullifiers[i]));
    }
    for (const auto& c : pub.commitments) t.append_scalar("cm", S(c));
}

Hash BundleProofCacheKeyV2(const std::vector<uint8_t>& envelope, const BundlePublicInputs& pub) {
    dinero::crypto::CSHA256 h;
    static constexpr char kDomain[] = "dinero.shielded.verified.bundle.v2";
    h.Write(reinterpret_cast<const uint8_t*>(kDomain), sizeof(kDomain) - 1);
    auto u64 = [&](uint64_t v) { uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i)); h.Write(b, 8); };
    u64(envelope.size()); h.Write(envelope.data(), envelope.size());
    h.Write(pub.sighash.data(), 32); u64(pub.vb_pos); u64(pub.vb_neg);
    u64(pub.anchors.size());
    for (size_t i = 0; i < pub.anchors.size(); ++i) { h.Write(pub.anchors[i].data(), 32); h.Write(pub.nullifiers[i].data(), 32); }
    u64(pub.commitments.size());
    for (const auto& c : pub.commitments) h.Write(c.data(), 32);
    Hash out{}; h.Finalize(out.data()); return out;
}

std::vector<uint8_t> ProveBundleV2(const BundleWitness& w, const BundlePublicInputs& pub, secp256k1_context_struct* ctx) {
    return ProveImpl(w, pub, ctx, {});
}
bool VerifyBundleV2(const std::vector<uint8_t>& envelope, const BundlePublicInputs& pub, secp256k1_context_struct* ctx) {
    return VerifyImpl(envelope, pub, ctx, {});
}
#ifdef DINERO_SHIELDED_V2_TEST_HOOKS
std::vector<uint8_t> ProveBundleV2Neutered(const BundleWitness& w, const BundlePublicInputs& pub,
                                           secp256k1_context_struct* ctx, BundleCircuitNeuter n) { return ProveImpl(w, pub, ctx, n); }
bool VerifyBundleV2Neutered(const std::vector<uint8_t>& e, const BundlePublicInputs& pub,
                            secp256k1_context_struct* ctx, BundleCircuitNeuter n) { return VerifyImpl(e, pub, ctx, n); }
#endif
}  // namespace dinero::consensus::shielded::v2
```

Notes for the implementer: the exact header for `CSHA256` is the one `src/consensus/shielded/binding_sig.cpp` includes; `VerifiedProofCache` lives at `src/consensus/shielded/proof_verification_cache.h` (not under `include/`), so `dinero_shielded` already has the include path. The neutered hooks must be compiled into the library only for the test build: add `target_compile_definitions(dinero_shielded PRIVATE $<$<BOOL:${DINERO_SHIELDED_V2_TEST_HOOKS}>:DINERO_SHIELDED_V2_TEST_HOOKS=1>)` and set the option ON in the test configuration; release builds do not define it, and the production `dinerod` binary must not export the neutered symbols (verify with `nm build-release/dinerod | grep -c Neutered` → `0`).

- [ ] **Step 6: Add the sighash-replay neuter test**

Append to the last test in Step 1, replacing the comment block at the end:

```cpp
    const v2::BundleCircuitNeuter no_sighash{.omit_sighash = true};
    auto env_a = v2::ProveBundleV2Neutered(f.w, f.pub, Ctx(), no_sighash);
    ASSERT_FALSE(env_a.empty());
    auto other = f.pub; other.sighash[0] ^= 1;
    EXPECT_TRUE(v2::VerifyBundleV2Neutered(env_a, other, Ctx(), no_sighash)) << "without binding a proof moves between transactions";
    auto env_b = v2::ProveBundleV2(f.w, f.pub, Ctx());
    EXPECT_FALSE(v2::VerifyBundleV2(env_b, other, Ctx())) << "with binding it does not";
```

- [ ] **Step 7: Run the tests**

Run: `cmake --build build-spike --target test_shielded_v2_circuit && ./build-spike/test_shielded_v2_circuit`
Expected: 8 tests PASS in under 3 minutes (Release). Record `bundle 2-in-2-out` constraint count from a `std::cout` in `HonestBundlesProveAndVerify` and compare with Task 0's JSON (must match exactly; a mismatch means the spike and production statements diverged).

- [ ] **Step 8: Commit**

```bash
git add include/consensus/shielded/v2/keys.h include/consensus/shielded/v2/bundle_circuit.h src/consensus/shielded/v2/keys.cpp src/consensus/shielded/v2/bundle_circuit.cpp src/consensus/shielded/CMakeLists.txt tests/consensus/test_shielded_v2_circuit.cpp tests/CMakeLists.txt
git commit -m "shielded-v2: hash-key derivation, bundle circuit, Spartan+Hyrax prover/verifier with cache

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Consensus validation for version 7, v1 sunset, resource limits

**Files:**
- Create: `include/consensus/shielded/v2/validation.h`, `src/consensus/shielded/v2/validation.cpp`
- Modify: `include/consensus/shielded/shielded_validation.h:59-70` (enum), `:90-139` (`ValidationContext::v2_rules`), `:209-228` (13th parameter)
- Modify: `src/consensus/shielded/shielded_validation.cpp:258-275` (dispatch), `:278-310` (set `v2_rules`)
- Modify: `src/consensus/block_validation.cpp:101-112` (error string), `:167-179` (pass rules), `:651-660` (block resource gate passes rules)
- Modify: `src/daemon/mempool.cpp:2526-2538`, `src/consensus/reindexer.cpp:2471-2483` (pass rules)
- Modify: `include/consensus/shielded/resource_limits.h:73-120` (version-7 branch)
- Modify: `src/consensus/shielded/CMakeLists.txt` (add `v2/validation.cpp`)
- Test: `tests/consensus/test_shielded_v2_validation.cpp` (target `test_shielded_v2_validation`, ctest `ShieldedV2Validation`, labels `shielded;consensus;mandatory`, TIMEOUT 900)

**Interfaces:**
- Consumes: `ShieldedV2Rules` (Task 1); codec front door (Task 2); `BundlePublicInputs::FromBundle`, `VerifyBundleV2` (Task 3); `ValidationContext`, `ShieldedValidationError`, `ApplyShieldedBundle` (existing).
- Produces: `ShieldedValidationError ValidateShieldedBundleV2(const ShieldedBundle&, const ValidationContext&)`; `ShieldedValidationError::ProofVersionSunset = 10`; `ValidationContext::v2_rules` (`ShieldedV2Rules`, defaulted member); `BuildShieldedValidationContext(..., CompactShieldedRules compact_rules = {}, ShieldedV2Rules v2_rules = {})`; `ShieldedValidationErrorToString(ProofVersionSunset) == "shielded-proof-version-sunset"`.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/consensus/test_shielded_v2_validation.cpp
#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/resource_limits.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/shielded/v2/bundle_circuit.h"
#include "consensus/shielded/v2/rules.h"
#include "consensus/shielded/v2/serialization.h"
#include "consensus/shielded/v2/validation.h"
#include "crypto/evp_secp256k1.h"
#include "primitives/transaction.h"
#include <gtest/gtest.h>

namespace {
using namespace dinero::consensus::shielded;
namespace v2 = dinero::consensus::shielded::v2;
using dinero::Transaction;

Hash H(uint8_t s, uint8_t t = 0xCD) { Hash h{}; h[0] = s; h[31] = t; return h; }
Hash U64(uint64_t v) { Hash h{}; for (int i = 0; i < 8; ++i) h[31 - i] = static_cast<uint8_t>(v >> (8 * i)); return h; }

// A version-7 transaction whose bundle proves: spend n_in notes (1,000,000 each), pay
// n_out notes, with transparent delta `vb` (negative = fee/unshield, positive = shield).
struct Built { Transaction tx; ShieldedBundle bundle; CommitmentTree tree; NullifierSet nullifiers; };

Built BuildV7(size_t n_in, size_t n_out, int64_t vb, uint8_t seed = 1) {
    Built b;
    b.tx.version = Transaction::TX_VERSION_SHIELDED_BUNDLE_V2;
    b.tx.has_explicit_fee = true;
    b.tx.explicit_fee = dinero::AmountUna::FromUna(vb < 0 ? static_cast<uint64_t>(-vb) : 0);
    v2::BundleWitness w;
    b.bundle.value_balance = vb;
    const uint64_t each = 1'000'000;
    for (size_t i = 0; i < n_in; ++i) {
        v2::BundleSpendWitness s{};
        s.ask = H(0x10 + i, seed); s.nullifier_key = H(0x20 + i, seed); s.d = H(0x30 + i, seed);
        s.value = U64(each); s.randomness = H(0x40 + i, seed);
        const Hash pk = AuthRecipientCommitmentKey(
            v2::DiversifiedSpendPublicKeyV2(v2::HashSpendKeyV2(s.ask), s.d),
            PoseidonHash2(s.nullifier_key, NullifierKeyTag()));
        s.leaf_index = b.tree.Append(NoteCommitment(s.d, pk, s.value, s.randomness));
        w.spends.push_back(s);
    }
    for (auto& s : w.spends) {
        s.merkle_path = b.tree.GetAuthPath(s.leaf_index)->siblings;
        b.bundle.spends.push_back({ComputeNullifier(s.nullifier_key, s.leaf_index), b.tree.Root(), {}, {}});
    }
    std::sort(b.bundle.spends.begin(), b.bundle.spends.end(), [](auto& x, auto& y) { return x.nullifier < y.nullifier; });
    // Re-order witnesses to match the canonical (sorted) bundle order.
    std::vector<v2::BundleSpendWitness> sorted;
    for (const auto& sp : b.bundle.spends)
        for (const auto& s : w.spends) if (ComputeNullifier(s.nullifier_key, s.leaf_index) == sp.nullifier) sorted.push_back(s);
    w.spends = sorted;
    const uint64_t vb_pos = vb > 0 ? vb : 0, vb_neg = vb < 0 ? -vb : 0;
    const uint64_t total_out = each * n_in + vb_pos - vb_neg;
    std::vector<std::pair<Hash, v2::BundleOutputWitness>> outs;
    for (size_t j = 0; j < n_out; ++j) {
        v2::BundleOutputWitness o{};
        const uint64_t v = (j + 1 == n_out) ? total_out - (total_out / n_out) * (n_out - 1) : total_out / n_out;
        o.value = U64(v); o.public_key = H(0x60 + j, seed); o.randomness = H(0x70 + j, seed); o.d = H(0x80 + j, seed);
        outs.emplace_back(NoteCommitment(o.d, o.public_key, o.value, o.randomness), o);
    }
    std::sort(outs.begin(), outs.end(), [](auto& x, auto& y) { return x.first < y.first; });
    for (auto& [cm, o] : outs) { b.bundle.outputs.push_back({cm, {}, std::vector<uint8_t>(611, 0xEE), {}}); w.outputs.push_back(o); }
    // Sighash needs the final envelope; bundle bytes with an empty proof first, then prove, then re-serialize.
    b.tx.shielded_bundle_bytes = SerializeShieldedBundleV2(b.bundle);
    const Hash sighash = ComputeShieldedTxSighash(b.tx);
    const auto pub = v2::BundlePublicInputs::FromBundle(b.bundle, sighash);
    b.bundle.v2_proof = v2::ProveBundleV2(w, pub, dinero::crypto::GetSecp256k1ContextSignVerify());
    b.tx.shielded_bundle_bytes = SerializeShieldedBundleV2(b.bundle);
    return b;
}

ValidationContext Ctx(const Built& b, uint32_t height, ShieldedV2Rules rules, const AnchorHistory* hist = nullptr) {
    return BuildShieldedValidationContext(b.tx, &b.nullifiers, &b.tree, height, b.bundle.value_balance,
        /*activation=*/1, hist, /*input_binding=*/2, /*cv=*/3, /*auth=*/4, /*covenant=*/UINT32_MAX, {}, rules);
}
const ShieldedV2Rules kLive{true, 100, 200};

TEST(ShieldedV2Validation, SighashIsIndependentOfTheProofBytes) {
    // The proof is inside the bundle bytes; the sighash must not cover it or proving is circular.
    auto b = BuildV7(1, 1, -1000);
    auto tx2 = b.tx; auto bundle2 = b.bundle; bundle2.v2_proof.clear();
    tx2.shielded_bundle_bytes = SerializeShieldedBundleV2(bundle2);
    EXPECT_EQ(ComputeShieldedTxSighash(b.tx), ComputeShieldedTxSighash(tx2));
}

TEST(ShieldedV2Validation, HonestBundleValidatesAtAndAfterActivation) {
    auto b = BuildV7(2, 2, -1000);
    EXPECT_EQ(ValidateShieldedBundle(b.bundle, Ctx(b, 100, kLive)), ShieldedValidationError::Ok);
    EXPECT_EQ(ValidateShieldedBundle(b.bundle, Ctx(b, 150, kLive)), ShieldedValidationError::Ok);
    EXPECT_EQ(ValidateShieldedBundle(b.bundle, Ctx(b, 99, kLive)), ShieldedValidationError::NotActive);
    EXPECT_EQ(ValidateShieldedBundle(b.bundle, Ctx(b, 150, {})), ShieldedValidationError::NotActive) << "dormant network";
    EXPECT_TRUE(ApplyShieldedBundle(b.bundle, &b.tree, &b.nullifiers, 100));
    EXPECT_EQ(b.nullifiers.Size(), 2u);
}

TEST(ShieldedV2Validation, ShieldAndUnshieldShapes) {
    auto shield = BuildV7(0, 1, 500000);
    EXPECT_EQ(ValidateShieldedBundle(shield.bundle, Ctx(shield, 100, kLive)), ShieldedValidationError::Ok);
    auto unshield = BuildV7(1, 0, -1000000);
    EXPECT_EQ(ValidateShieldedBundle(unshield.bundle, Ctx(unshield, 100, kLive)), ShieldedValidationError::Ok);
}

TEST(ShieldedV2Validation, RejectsEachRuleViolation) {
    auto b = BuildV7(2, 2, -1000);
    auto ctx = Ctx(b, 100, kLive);

    auto x = b.bundle; x.value_balance = -999;      // transparent delta mismatch
    ctx.transparent_value_delta = -1000;
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::ValueBalanceMismatch);
    ctx.transparent_value_delta = -999;             // consistent delta but proof was for -1000
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::ProofInvalid);
    ctx.transparent_value_delta = -1000;

    x = b.bundle; x.spends[0].anchor[0] ^= 1;
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::AnchorInvalid);

    x = b.bundle; x.spends[1].nullifier = x.spends[0].nullifier;
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::NullifierDuplicate);

    NullifierSet spent; spent.Insert(b.bundle.spends[0].nullifier, 50);
    auto ctx_spent = ctx; ctx_spent.nullifier_set = &spent;
    EXPECT_EQ(ValidateShieldedBundle(b.bundle, ctx_spent), ShieldedValidationError::NullifierDuplicate);

    x = b.bundle; x.v2_proof[x.v2_proof.size() / 2] ^= 1;
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::ProofInvalid);
    x = b.bundle; x.v2_proof[4] = 0x02;
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::ProofInvalid) << "unknown/phase-2 id";
    x = b.bundle; x.v2_proof.clear();
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::BundleMalformed);
    x = b.bundle; x.v2_proof.assign(v2::kV2MaxEnvelopeBytes + 1, 0);
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::BundleTooLarge);

    auto other = b; other.tx.lockTime = 77;          // wrong sighash binding
    auto ctx_other = Ctx(other, 100, kLive);
    EXPECT_EQ(ValidateShieldedBundle(b.bundle, ctx_other), ShieldedValidationError::ProofInvalid);

    x = b.bundle; x.spends[0].cv[0] = 0x08;         // v1 fields must be zero/empty in v2
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::BundleMalformed);
    x = b.bundle; x.spends[0].zk_proof = {1};
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::BundleMalformed);
    x = b.bundle; x.binding_sig[0] = 1;
    EXPECT_EQ(ValidateShieldedBundle(x, ctx), ShieldedValidationError::BundleMalformed);

    for (int i = 0; i < 3; ++i) { ShieldedSpend s{}; s.nullifier = H(0xF0 + i); s.anchor = b.tree.Root(); b.bundle.spends.push_back(s); }
    EXPECT_EQ(ValidateShieldedBundle(b.bundle, ctx), ShieldedValidationError::BundleTooLarge);
}

TEST(ShieldedV2Validation, CachedProofDoesNotSkipContextualRules) {
    auto b = BuildV7(1, 1, -1000);
    ASSERT_EQ(ValidateShieldedBundle(b.bundle, Ctx(b, 100, kLive)), ShieldedValidationError::Ok);  // proof now cached
    EXPECT_EQ(ValidateShieldedBundle(b.bundle, Ctx(b, 99, kLive)), ShieldedValidationError::NotActive);
    NullifierSet spent; spent.Insert(b.bundle.spends[0].nullifier, 50);
    auto ctx = Ctx(b, 100, kLive); ctx.nullifier_set = &spent;
    EXPECT_EQ(ValidateShieldedBundle(b.bundle, ctx), ShieldedValidationError::NullifierDuplicate);
}

TEST(ShieldedV2Validation, V1ProofsSunset) {
    // A v6 bundle validated under the v1 rules is out of scope here; the sunset gate fires
    // before any v1 work. Use an empty-proof v6 bundle which would otherwise be BundleMalformed.
    Transaction v6; v6.version = Transaction::TX_VERSION_SHIELDED_V2;
    ShieldedBundle legacy; legacy.value_balance = -1; legacy.spends.push_back({H(1), H(2), {}, {1}});
    v6.shielded_bundle_bytes = SerializeShieldedBundle(legacy);
    CommitmentTree tree; NullifierSet nf;
    auto ctx = [&](uint32_t h, ShieldedV2Rules r) {
        return BuildShieldedValidationContext(v6, &nf, &tree, h, -1, 1, nullptr, 2, 3, 4, UINT32_MAX, {}, r);
    };
    EXPECT_EQ(ValidateShieldedBundle(legacy, ctx(199, kLive)), ShieldedValidationError::BundleMalformed) << "v1 path still reached";
    EXPECT_EQ(ValidateShieldedBundle(legacy, ctx(200, kLive)), ShieldedValidationError::ProofVersionSunset);
    EXPECT_EQ(ValidateShieldedBundle(legacy, ctx(200, {})), ShieldedValidationError::BundleMalformed) << "dormant: no sunset";
    ShieldedBundle empty;
    EXPECT_EQ(ValidateShieldedBundle(empty, ctx(200, kLive)), ShieldedValidationError::Ok) << "empty bundles are never sunset";
}

TEST(ShieldedV2Validation, ResourceLimitsForVersionSeven) {
    auto b = BuildV7(2, 2, -1000);
    size_t proofs = 99; std::string err;
    EXPECT_TRUE(CheckAuthTransactionResources(b.tx, 100, 4, proofs, err, {}, kLive)) << err;
    EXPECT_EQ(proofs, 1u);
    auto pre = b.tx;  // version 7 before v2 activation is not a valid auth resource claim
    EXPECT_FALSE(CheckAuthTransactionResources(pre, 99, 4, proofs, err, {}, kLive));
    EXPECT_EQ(err, "shielded-v2-not-active");
    auto big = b.bundle; big.v2_proof.assign(v2::kV2MaxEnvelopeBytes + 1, 0);
    auto tx_big = b.tx; tx_big.shielded_bundle_bytes = SerializeShieldedBundleV2(big);
    EXPECT_FALSE(CheckAuthTransactionResources(tx_big, 100, 4, proofs, err, {}, kLive));
}
}  // namespace
```

Register the target like `test_shielded_v2_circuit` (same links, plus the test-hooks define is not needed here).

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build-spike --target test_shielded_v2_validation`
Expected: compile errors (`v2/validation.h`, `ProofVersionSunset`, 13th parameter).

- [ ] **Step 3: Context, enum, dispatch**

`include/consensus/shielded/shielded_validation.h`:
- enum: add `ProofVersionSunset = 10,   ///< v1 proof (tx version 5/6) at/after shielded_v1_sunset_height`.
- `#include "consensus/shielded/v2/rules.h"`; after `CompactShieldedRules compact_rules{};` add `ShieldedV2Rules v2_rules{};`.
- `BuildShieldedValidationContext` declaration: append `, ShieldedV2Rules v2_rules = {}` after `CompactShieldedRules compact_rules = {}`.

`src/consensus/shielded/shielded_validation.cpp`:
- `#include "consensus/shielded/v2/validation.h"`.
- Top of `ValidateShieldedBundle`, before the compact branch:

```cpp
    if (Transaction::IsShieldedBundleV2Version(ctx.transaction_version)) {
        return v2::ValidateShieldedBundleV2(bundle, ctx);
    }
    if (!bundle.IsEmpty() && !ctx.v2_rules.V1Allowed(ctx.block_height)) {
        return ShieldedValidationError::ProofVersionSunset;
    }
```

- In `BuildShieldedValidationContext`, add the parameter and `ctx.v2_rules = v2_rules;`.

`src/consensus/block_validation.cpp:101-112`: add `case ShieldedValidationError::ProofVersionSunset: return "shielded-proof-version-sunset";`. At the three `BuildShieldedValidationContext` call sites (`block_validation.cpp:167`, `mempool.cpp:2526`, `reindexer.cpp:2471`) append the argument `shielded::V2RulesFor(Params())` (namespace-qualified as each file does for `CompactRulesFor`).

- [ ] **Step 4: v2 validation**

```cpp
// include/consensus/shielded/v2/validation.h
#pragma once
#include "consensus/shielded/shielded_validation.h"
namespace dinero::consensus::shielded::v2 {
// Version-7 bundle rules, in this order and all before any proof work:
// activation, shape caps, v1-field emptiness, envelope size, nullifier duplicates,
// anchors, then the single proof (cache-aware), then value balance.
ShieldedValidationError ValidateShieldedBundleV2(const ShieldedBundle& bundle, const ValidationContext& ctx);
}
```

```cpp
// src/consensus/shielded/v2/validation.cpp
#include "consensus/shielded/v2/validation.h"
#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/v2/bundle_circuit.h"
#include "consensus/shielded/v2/envelope.h"
#include "crypto/evp_secp256k1.h"
#include <unordered_set>

namespace dinero::consensus::shielded::v2 {
namespace {
struct HashHasher { size_t operator()(const Hash& h) const { size_t r = 0; for (int i = 0; i < 8; ++i) r |= size_t(h[i]) << (8 * i); return r; } };
bool V1FieldsEmpty(const ShieldedBundle& b) {
    if (!b.aggregated_range_proof.empty() || b.bvk_commitment != ValueCommitment{} || b.binding_sig != BindingSignature{}) return false;
    for (const auto& s : b.spends) if (s.cv != ValueCommitment{} || !s.zk_proof.empty()) return false;
    for (const auto& o : b.outputs) if (o.cv != ValueCommitment{} || !o.zk_proof.empty()) return false;
    return true;
}
}  // namespace

ShieldedValidationError ValidateShieldedBundleV2(const ShieldedBundle& b, const ValidationContext& ctx) {
    if (b.IsEmpty()) return ShieldedValidationError::Ok;
    if (!ctx.v2_rules.Active(ctx.block_height)) return ShieldedValidationError::NotActive;
    if (b.spends.size() > kV2MaxSpends || b.outputs.size() > kV2MaxOutputs) return ShieldedValidationError::BundleTooLarge;
    if (b.v2_proof.size() > kV2MaxEnvelopeBytes) return ShieldedValidationError::BundleTooLarge;
    if (b.v2_proof.empty() || !V1FieldsEmpty(b)) return ShieldedValidationError::BundleMalformed;
    for (const auto& o : b.outputs) if (o.encrypted_note.empty()) return ShieldedValidationError::BundleMalformed;

    std::unordered_set<Hash, HashHasher> seen;
    for (const auto& s : b.spends) {
        if (!seen.insert(s.nullifier).second) return ShieldedValidationError::NullifierDuplicate;
        if (ctx.nullifier_set && ctx.nullifier_set->Contains(s.nullifier)) return ShieldedValidationError::NullifierDuplicate;
    }
    for (const auto& s : b.spends) {
        const bool current = ctx.commitment_tree && s.anchor == ctx.commitment_tree->Root();
        const bool recent = ctx.anchor_history && ctx.anchor_history->Contains(s.anchor);
        if (!current && !recent) return ShieldedValidationError::AnchorInvalid;
    }
    // The proof is over the sighash the transaction actually has; the bundle's value_balance
    // is a public input, so a proof for a different balance fails here, and the transparent
    // side is checked last exactly as v1 does.
    const auto pub = BundlePublicInputs::FromBundle(b, ctx.tx_sighash);
    if (!VerifyBundleV2(b.v2_proof, pub, dinero::crypto::GetSecp256k1ContextSignVerify()))
        return ShieldedValidationError::ProofInvalid;
    if (b.value_balance != ctx.transparent_value_delta) return ShieldedValidationError::ValueBalanceMismatch;
    return ShieldedValidationError::Ok;
}
}  // namespace dinero::consensus::shielded::v2
```

The anchor rule mirrors `ValidateExpandedShieldedBundle` lines 139-149; read them and keep the same current-root-or-history semantics (if that code also requires `anchor_history` to be non-null in some mode, copy that exactly).

- [ ] **Step 5: Resource limits**

In `include/consensus/shielded/resource_limits.h`, `CheckAuthTransactionResources` gains a trailing parameter `ShieldedV2Rules v2_rules = {}` (include `v2/rules.h` and `v2/envelope.h`), and after the `shielded-auth-requires-tx-v6` check add:

```cpp
    if (Transaction::IsShieldedBundleV2Version(tx.version)) {
        if (!v2_rules.Active(height) || !tx.has_explicit_fee) { error = "shielded-v2-not-active"; return false; }
        if (!CheckTxResourceEnvelope(tx, true, error)) return false;
        ShieldedBundle bundle;
        if (DeserializeShieldedBundleForVersion(tx.version, tx.shielded_bundle_bytes, &bundle) != BundleDecodeError::Ok) {
            error = "shielded-bundle-malformed"; return false;
        }
        if (bundle.spends.size() > v2::kV2MaxSpends || bundle.outputs.size() > v2::kV2MaxOutputs ||
            bundle.v2_proof.size() > v2::kV2MaxEnvelopeBytes) {
            error = "shielded-bundle-resource-limit"; return false;
        }
        proofs = 1;
        return true;
    }
```

Pass `V2RulesFor(Params())` at its callers: `mempool.cpp:2407-2411`, `block_validation.cpp:655` (`CheckAuthBlockResources` → thread the parameter through `AccumulateAuthBlockResources`/`CheckAuthBlockResources` the same way `compact_rules` is threaded today), and any other caller found by `grep -rn 'CheckAuthTransactionResources\|CheckAuthBlockResources' src include`.

- [ ] **Step 6: Run the tests**

Run: `cmake --build build-spike --target test_shielded_v2_validation test_shielded_validation test_shielded_resource_limits dinerod && ./build-spike/test_shielded_v2_validation && ctest --test-dir build-spike -R 'ShieldedValidation$|ShieldedResourceLimits|CompactProductionV6Vectors|CompactActivation' --output-on-failure`
Expected: all PASS.

- [ ] **Step 7: Commit**

```bash
git add include/consensus/shielded/v2/validation.h src/consensus/shielded/v2/validation.cpp include/consensus/shielded/shielded_validation.h src/consensus/shielded/shielded_validation.cpp src/consensus/block_validation.cpp src/daemon/mempool.cpp src/consensus/reindexer.cpp include/consensus/shielded/resource_limits.h src/consensus/shielded/CMakeLists.txt tests/consensus/test_shielded_v2_validation.cpp tests/CMakeLists.txt
git commit -m "shielded-v2: version-7 bundle validation, v1 sunset gate, resource limits

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Wallet: v2 key scheme, addresses, scanning, bundle builder, migration, RPC, prover kit

**Blocked on:** owner approval of spec §10 (Amendment A). Build Tasks 6–8 first if approval is pending.

**Files:**
- Modify: `include/wallet/shielded_note_store.h:41-49` (`HashKeyV2 = 3`)
- Modify: `include/wallet/shielded_derivation.h:50-90,252-292`, `src/wallet/shielded_derivation.cpp` (`hk`, `DeriveDiversifiedAddressV2`, v2 HRPs, decoder)
- Create: `include/wallet/shielded_v2_wallet_ops.h`, `src/wallet/shielded_v2_wallet_ops.cpp`
- Modify: `src/wallet/shielded_wallet_ops.cpp` (scan ownership check; change-output scheme), `include/wallet/shielded_wallet_ops.h:258,507`
- Modify: `src/rpc/shielded_rpc_json.cpp:1764-1776` (register three RPCs)
- Modify: `include/shielded_prover_kit/shielded_prover_kit.h`, `src/shielded_prover_kit/shielded_prover_kit.cpp` (v2 request)
- Modify: `src/consensus/shielded/CMakeLists.txt` (add `${CMAKE_SOURCE_DIR}/src/wallet/shielded_v2_wallet_ops.cpp`)
- Test: `tests/wallet/test_shielded_v2_wallet.cpp` (target `test_shielded_v2_wallet`, ctest `ShieldedV2Wallet`, TIMEOUT 900)

**Interfaces:**
- Consumes: `v2::HashSpendKeyV2`, `v2::DiversifiedSpendPublicKeyV2`, `v2::ProveBundleV2`, `v2::BundleWitness`, `v2::BundlePublicInputs::FromBundle` (Task 3); `SerializeShieldedBundleV2`, `TX_VERSION_SHIELDED_BUNDLE_V2` (Task 2); existing `DeriveShieldedAccount`, `ChaCha20Diversifier`, `HashToPoint`, `DerivePkD`, `DeriveDiversifiedNullifierKey`, `NullifierKeyCommitment`, `BuildAddressPayload`, `EncodeShieldedAddress`, `EncryptNoteForRecipient`, `TryDecryptNoteForViewer`, `ShieldedNoteStore`, `ComputeShieldedTxSighash`.
- Produces (`namespace dinero::wallet::shielded`):
  - `ShieldedAccountKeys::hk` (`Hash`), set by `DeriveShieldedAccount` as `v2::HashSpendKeyV2(ask)`.
  - `constexpr const char* kHrpMainnetV2 = "dinz"; kHrpTestnetV2 = "tdinz"; kHrpRegtestV2 = "rdinz";`
  - `DiversifiedAddress DeriveDiversifiedAddressV2(const ShieldedAccountKeys&, uint64_t j, const std::string& hrp_v2);` (payload `d || pk_d_enc || pk_d_spend_v2 || nfk_c`).
  - `DecodedShieldedAddress::scheme` (`NoteKeyScheme`, `Auth` for `dins*`, `HashKeyV2` for `dinz*`); decoder accepts both HRP families, curve-checks `pk_d` always and `pk_d_spend` only for `Auth`.
  - `struct V2SpendInput { ShieldedNote note; };` `struct V2OutputRequest { DecodedShieldedAddress to; uint64_t value_una; std::array<uint8_t,512> memo; };`
  - `OpResult BuildV2BundleForTx(Transaction& tx, const ShieldedAccountKeys& keys, const std::vector<ShieldedNote>& spends, const std::vector<V2OutputRequest>& outputs, int64_t value_balance, const CommitmentTree& tree, ShieldedNoteStore& store);` sets `tx.version = 7`, fills `tx.shielded_bundle_bytes`, persists outputs to self as `HashKeyV2` notes.
  - RPCs: `wallet.shieldedaddressv2 {"index": j}` → `{address}`; `wallet.transferv2 {"to": addr, "amount_una": n, "fee_una": f}` → `{txid}`; `wallet.shieldedmigratev2 {"max_notes": 4}` → `{txid, migrated_una}` (v1 auth-proof transfer of up to `kAuthMaxSpends` auth notes to the wallet's own v2 address 0).
  - Prover kit: `typedef struct dinero_shielded_v2_bundle_request { const uint8_t* serialized_unsigned_tx; size_t serialized_unsigned_tx_len; int64_t value_balance; const dinero_shielded_v2_spend_note* spends; size_t n_spends; const dinero_shielded_v2_output* outputs; size_t n_outputs; uint8_t ask32[32]; uint8_t nvk32[32]; }` and `dinero_shielded_status dinero_shielded_prove_bundle_v2(const dinero_shielded_v2_bundle_request*, dinero_shielded_unshield_result*)` returning the bundle bytes to splice into the version-7 envelope (same contract as the existing unshield entry point).

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/wallet/test_shielded_v2_wallet.cpp
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/shielded/v2/keys.h"
#include "consensus/shielded/v2/rules.h"
#include "consensus/shielded/v2/serialization.h"
#include "wallet/shielded_derivation.h"
#include "wallet/shielded_note_store.h"
#include "wallet/shielded_v2_wallet_ops.h"
#include "wallet/shielded_wallet_ops.h"
#include <gtest/gtest.h>

namespace {
namespace wsh = dinero::wallet::shielded;
namespace csh = dinero::consensus::shielded;
namespace v2 = dinero::consensus::shielded::v2;

wsh::ShieldedAccountKeys Keys(uint8_t seed_byte) {
    std::array<uint8_t, 64> seed{}; seed.fill(seed_byte);
    return wsh::DeriveShieldedAccount(seed.data(), seed.size(), 0);
}

TEST(ShieldedV2Wallet, AccountKeysCarryHashSpendKey) {
    const auto k = Keys(0x11);
    EXPECT_EQ(k.hk, v2::HashSpendKeyV2(k.ask));
    EXPECT_NE(k.hk, csh::Hash{});
}

TEST(ShieldedV2Wallet, V2AddressRoundTripAndSchemeDetection) {
    const auto k = Keys(0x11);
    const auto a = wsh::DeriveDiversifiedAddressV2(k, 3, wsh::kHrpRegtestV2);
    EXPECT_EQ(a.address.rfind("rdinz1", 0), 0u);
    csh::Hash d_padded{}; std::copy(a.d.begin(), a.d.end(), d_padded.begin());
    EXPECT_EQ(a.pk_d_spend, v2::DiversifiedSpendPublicKeyV2(k.hk, d_padded));
    EXPECT_EQ(a.nfk_commitment, wsh::NullifierKeyCommitment(wsh::DeriveDiversifiedNullifierKey(k.nvk, a.d)));
    const auto dec = wsh::DecodeShieldedAddress(a.address);
    EXPECT_EQ(dec.scheme, dinero::wallet::NoteKeyScheme::HashKeyV2);
    EXPECT_EQ(dec.pk_d_spend, a.pk_d_spend);
    EXPECT_EQ(dec.pk_d, a.pk_d);  // discovery key unchanged
    const auto auth = wsh::DeriveDiversifiedAddress(k, 3, wsh::kHrpRegtest);
    EXPECT_EQ(wsh::DecodeShieldedAddress(auth.address).scheme, dinero::wallet::NoteKeyScheme::Auth);
    EXPECT_NE(auth.pk_d_spend, a.pk_d_spend);
}

TEST(ShieldedV2Wallet, DecoderStillRejectsGarbageUnderV2Hrp) {
    auto a = wsh::DeriveDiversifiedAddressV2(Keys(0x11), 0, wsh::kHrpRegtestV2);
    auto payload = a.payload; payload[11] ^= 0x01;  // corrupt pk_d_enc x-coordinate byte
    // Re-encoding a corrupted pk_d_enc must still trip the curve check for the discovery key.
    bool threw = false;
    try { (void)wsh::DecodeShieldedAddress(wsh::EncodeShieldedAddress(payload, wsh::kHrpRegtestV2)); }
    catch (const std::runtime_error&) { threw = true; }
    // Probability ½ the corrupted x is on-curve; run over 8 corruptions and require at least one rejection.
    for (uint8_t i = 1; i < 8 && !threw; ++i) {
        payload = a.payload; payload[11] ^= i;
        try { (void)wsh::DecodeShieldedAddress(wsh::EncodeShieldedAddress(payload, wsh::kHrpRegtestV2)); }
        catch (const std::runtime_error&) { threw = true; }
    }
    EXPECT_TRUE(threw);
}

TEST(ShieldedV2Wallet, TransferBuildsValidVersionSevenTransaction) {
    // Sender: two v2 notes worth 1,000,000 each, received via ordinary output creation.
    const auto sender = Keys(0x21), recipient = Keys(0x22);
    csh::CommitmentTree tree; csh::NullifierSet nullifiers;
    dinero::wallet::ShieldedNoteStore store(":memory:");
    ASSERT_TRUE(store.Open());
    const auto own = wsh::DeriveDiversifiedAddressV2(sender, 0, wsh::kHrpRegtestV2);
    // Fund: build a shield (0-in, 1-out) to `own` for each note using the v2 builder, apply to tree, scan into the store.
    for (int i = 0; i < 2; ++i) {
        dinero::Transaction shield; shield.version = dinero::Transaction::TX_VERSION_SHIELDED_BUNDLE_V2; shield.has_explicit_fee = true;
        auto r = wsh::BuildV2BundleForTx(shield, sender, {}, {{wsh::DecodeShieldedAddress(own.address), 1'000'000, {}}}, +1'000'000, tree, store);
        ASSERT_EQ(r.status, dinero::wallet::shielded_ops::OpStatus::Ok) << r.error;
        csh::ShieldedBundle b; ASSERT_EQ(csh::DeserializeShieldedBundleV2(shield.shielded_bundle_bytes, &b), csh::BundleDecodeError::Ok);
        ASSERT_TRUE(csh::ApplyShieldedBundle(b, &tree, &nullifiers, 100 + i));
        ASSERT_TRUE(wsh::RescanConfirmedBlockV2(store, sender, shield, 100 + i, tree));  // recognises scheme-3 ownership
    }
    auto notes = store.ListUnspent();
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_EQ(notes[0].key_scheme, dinero::wallet::NoteKeyScheme::HashKeyV2);

    dinero::Transaction tx; tx.version = dinero::Transaction::TX_VERSION_SHIELDED_BUNDLE_V2; tx.has_explicit_fee = true;
    tx.explicit_fee = dinero::AmountUna::FromUna(1000);
    const auto to = wsh::DeriveDiversifiedAddressV2(recipient, 0, wsh::kHrpRegtestV2);
    auto r = wsh::BuildV2BundleForTx(tx, sender, notes, {{wsh::DecodeShieldedAddress(to.address), 1'500'000, {}}}, -1000, tree, store);
    ASSERT_EQ(r.status, dinero::wallet::shielded_ops::OpStatus::Ok) << r.error;
    csh::ShieldedBundle b; ASSERT_EQ(csh::DeserializeShieldedBundleV2(tx.shielded_bundle_bytes, &b), csh::BundleDecodeError::Ok);
    EXPECT_EQ(b.spends.size(), 2u);
    EXPECT_EQ(b.outputs.size(), 2u) << "payment + change";
    EXPECT_EQ(b.value_balance, -1000);
    auto ctx = csh::BuildShieldedValidationContext(tx, &nullifiers, &tree, 102, -1000, 1, nullptr, 2, 3, 4, UINT32_MAX, {}, {true, 100, UINT32_MAX});
    EXPECT_EQ(csh::ValidateShieldedBundle(b, ctx), csh::ShieldedValidationError::Ok);
    // Recipient discovers the payment with ivk only.
    size_t found = 0;
    for (const auto& o : b.outputs) {
        csh::EncryptedNote enc{}; std::copy(o.encrypted_note.begin(), o.encrypted_note.end(), enc.begin());
        if (wsh::TryDecryptNoteForViewer(recipient.ivk, enc)) ++found;
    }
    EXPECT_EQ(found, 1u);
}

TEST(ShieldedV2Wallet, MigrationSpendsAuthNotesWithV1ProofsToOwnV2Address) {
    // An Auth note (scheme 1) is spent through the EXISTING v1 auth builder into a v2 address.
    const auto k = Keys(0x31);
    csh::CommitmentTree tree; csh::NullifierSet nullifiers;
    dinero::wallet::ShieldedNoteStore store(":memory:"); ASSERT_TRUE(store.Open());
    auto note = wsh::test::MakeConfirmedAuthNote(k, tree, store, 2'000'000);  // helper mirroring AutoFeeAuthNote in shielded_validation_tests.cpp
    dinero::Transaction tx;
    auto r = wsh::BuildMigrateToV2Tx(tx, k, {note}, /*fee_una=*/1000, tree, store, wsh::kHrpRegtestV2);
    ASSERT_EQ(r.status, dinero::wallet::shielded_ops::OpStatus::Ok) << r.error;
    EXPECT_EQ(tx.version, dinero::Transaction::TX_VERSION_SHIELDED_V2) << "migration is a v1 (v6) transaction";
    csh::ShieldedBundle b; ASSERT_EQ(csh::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &b), csh::BundleDecodeError::Ok);
    ASSERT_EQ(b.outputs.size(), 1u);
    auto ctx = csh::BuildShieldedValidationContext(tx, &nullifiers, &tree, 200, -1000, 1, nullptr, 2, 3, 4, UINT32_MAX, {}, {});
    EXPECT_EQ(csh::ValidateShieldedBundle(b, ctx), csh::ShieldedValidationError::Ok);
    ASSERT_TRUE(csh::ApplyShieldedBundle(b, &tree, &nullifiers, 200));
    ASSERT_TRUE(wsh::RescanConfirmedBlockV2(store, k, tx, 200, tree));
    auto unspent = store.ListUnspent();
    ASSERT_EQ(unspent.size(), 1u);
    EXPECT_EQ(unspent[0].key_scheme, dinero::wallet::NoteKeyScheme::HashKeyV2);
    EXPECT_EQ(unspent[0].value_una, 1'999'000u);
}
}  // namespace
```

`wsh::test::MakeConfirmedAuthNote` lives in a new header `tests/wallet/shielded_v2_test_helpers.h` and is a straight extraction of the `AutoFeeAuthNote` fixture logic from `src/test/shielded_validation_tests.cpp` (find it with `grep -n AutoFeeAuthNote src/test/shielded_validation_tests.cpp`); copy the body, do not include the test file. `RescanConfirmedBlockV2` is the scheme-aware wrapper produced in Step 5.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build-spike --target test_shielded_v2_wallet`
Expected: compile errors (`hk`, `kHrpRegtestV2`, `shielded_v2_wallet_ops.h`).

- [ ] **Step 3: Keys and addresses**

`include/wallet/shielded_note_store.h`: add `HashKeyV2 = 3,  ///< pk_d_spend = Poseidon2(hk, d), hk = Poseidon2(ask, HK_TAG). Hash-only ownership (spec §10).` to `NoteKeyScheme`.

`include/wallet/shielded_derivation.h`: in `ShieldedAccountKeys` after `Hash ivk{};` add `/// Hash spend key for v2 notes: Poseidon2(ask, "DIN/v7/shielded/v2/hk"). Part of the v2 full viewing key. \n Hash hk{};`. Add the three v2 HRP constants after `kHrpRegtest`. Declare:

```cpp
/// v2 address at index j: payload d || pk_d_enc || Poseidon2(hk, d) || nfk_c under a v2 HRP.
DiversifiedAddress DeriveDiversifiedAddressV2(const ShieldedAccountKeys& keys, uint64_t j, const std::string& hrp_v2);
```

and add `NoteKeyScheme scheme = NoteKeyScheme::Auth;` to `DecodedShieldedAddress` (include `wallet/shielded_note_store.h`).

`src/wallet/shielded_derivation.cpp`:
- In `DeriveShieldedAccount`, after `ivk` is computed: `keys.hk = consensus::shielded::v2::HashSpendKeyV2(keys.ask);` (include `consensus/shielded/v2/keys.h`).
- `DeriveDiversifiedAddressV2`: copy `DeriveDiversifiedAddress`'s body; replace the `pk_d_spend` computation by `Hash d_padded{}; std::memcpy(d_padded.data(), d.data(), d.size()); out.pk_d_spend = consensus::shielded::v2::DiversifiedSpendPublicKeyV2(keys.hk, d_padded);`; encode under `hrp_v2`.
- `DecodeShieldedAddress`: accept HRPs in `{dins,tdins,rdins}` (scheme `Auth`) and `{dinz,tdinz,rdinz}` (scheme `HashKeyV2`); in the on-curve loop at `:457-462`, skip the check for `&out.pk_d_spend` when `out.scheme == HashKeyV2`; still require `nfk_commitment != 0`.

- [ ] **Step 4: Bundle builder and migration**

```cpp
// include/wallet/shielded_v2_wallet_ops.h
#pragma once
#include "consensus/shielded/commitment_tree.h"
#include "primitives/transaction.h"
#include "wallet/shielded_derivation.h"
#include "wallet/shielded_note_store.h"
#include "wallet/shielded_wallet_ops.h"   // OpStatus, OpResult
#include <array>
#include <vector>

namespace dinero::wallet::shielded {

struct V2OutputRequest {
    DecodedShieldedAddress to;
    uint64_t value_una = 0;
    std::array<uint8_t, 512> memo{};
};

/// Builds the version-7 bundle for `tx` (tx.version/has_explicit_fee must already be set),
/// spending `spends` (all NoteKeyScheme::HashKeyV2, owned by `keys`) into `outputs` plus
/// change to the wallet's v2 address 0 when Σ spends + max(vb,0) − Σ outputs − max(−vb,0) > 0.
/// value_balance: transparent value entering (+) or leaving (−, fee included) the pool.
/// Persists outputs addressed to this wallet as HashKeyV2 notes in `store` (unconfirmed).
shielded_ops::OpResult BuildV2BundleForTx(Transaction& tx, const ShieldedAccountKeys& keys,
                                          const std::vector<ShieldedNote>& spends,
                                          const std::vector<V2OutputRequest>& outputs,
                                          int64_t value_balance,
                                          const consensus::shielded::CommitmentTree& tree,
                                          ShieldedNoteStore& store);

/// v1 (tx version 6, Auth-profile proofs) transfer of `auth_notes` (≤ kAuthMaxSpends, all
/// NoteKeyScheme::Auth) to this wallet's v2 address 0 under `hrp_v2`, minus `fee_una`.
/// Delegates to the existing BuildAddressedTransferBundleForTx; consensus is unchanged.
shielded_ops::OpResult BuildMigrateToV2Tx(Transaction& tx, const ShieldedAccountKeys& keys,
                                          const std::vector<ShieldedNote>& auth_notes, uint64_t fee_una,
                                          const consensus::shielded::CommitmentTree& tree,
                                          ShieldedNoteStore& store, const std::string& hrp_v2);

/// Scheme-aware scan of one confirmed transaction: decrypts each output with ivk, then
/// recognises ownership under Auth (pk_d = s·G) OR HashKeyV2 (Poseidon2(hk, d)) and stores
/// the note with the matching key_scheme. Wraps the existing RescanConfirmedBlock logic.
bool RescanConfirmedBlockV2(ShieldedNoteStore& store, const ShieldedAccountKeys& keys,
                            const Transaction& tx, uint32_t height,
                            const consensus::shielded::CommitmentTree& tree);
}  // namespace dinero::wallet::shielded
```

`src/wallet/shielded_v2_wallet_ops.cpp` implementation outline with the load-bearing code:

```cpp
shielded_ops::OpResult BuildV2BundleForTx(Transaction& tx, const ShieldedAccountKeys& keys,
        const std::vector<ShieldedNote>& spends, const std::vector<V2OutputRequest>& outputs,
        int64_t value_balance, const consensus::shielded::CommitmentTree& tree, ShieldedNoteStore& store) {
    namespace csh = consensus::shielded; namespace v2 = csh::v2;
    shielded_ops::OpResult out{};
    if (!Transaction::IsShieldedBundleV2Version(tx.version) || !tx.has_explicit_fee) { out.status = shielded_ops::OpStatus::InvalidParams; out.error = "v2_requires_tx_v7_explicit_fee"; return out; }
    if (spends.size() > v2::kV2MaxSpends || outputs.size() + 1 > v2::kV2MaxOutputs) { out.status = shielded_ops::OpStatus::InvalidParams; out.error = "v2_shape_exceeds_caps"; return out; }
    uint64_t sum_in = 0; for (const auto& n : spends) { if (n.key_scheme != NoteKeyScheme::HashKeyV2) { out.status = shielded_ops::OpStatus::InvalidParams; out.error = "v2_spend_requires_hashkey_note"; return out; } sum_in += n.value_una; }
    uint64_t sum_out = 0; for (const auto& o : outputs) sum_out += o.value_una;
    const uint64_t vb_pos = value_balance > 0 ? value_balance : 0, vb_neg = value_balance < 0 ? -value_balance : 0;
    if (sum_in + vb_pos < sum_out + vb_neg) { out.status = shielded_ops::OpStatus::InsufficientFunds; return out; }
    const uint64_t change = sum_in + vb_pos - sum_out - vb_neg;

    v2::BundleWitness w; csh::ShieldedBundle bundle; bundle.value_balance = value_balance;
    // Spends: witness from the stored note; nullifier from the note's nfk.
    std::vector<std::pair<csh::Hash, v2::BundleSpendWitness>> spend_rows;
    for (const auto& n : spends) {
        v2::BundleSpendWitness s{}; s.ask = keys.ask; s.nullifier_key = n.nullifier_key; s.d = n.d;
        s.value = ValueToHash(n.value_una); s.randomness = n.randomness; s.leaf_index = n.leaf_index;
        auto path = tree.GetAuthPath(n.leaf_index); if (!path) { out.status = shielded_ops::OpStatus::InternalError; out.error = "auth_path_missing"; return out; }
        s.merkle_path = path->siblings;
        spend_rows.emplace_back(csh::ComputeNullifier(n.nullifier_key, n.leaf_index), s);
    }
    std::sort(spend_rows.begin(), spend_rows.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [nf, s] : spend_rows) { bundle.spends.push_back({nf, tree.Root(), {}, {}}); w.spends.push_back(s); }
    // Outputs (+ change to own v2 address 0): pk = Poseidon2(pk_d_spend, nfk_c) from the address; note encrypted to pk_d_enc.
    std::vector<V2OutputRequest> all = outputs;
    if (change > 0) { auto own = DeriveDiversifiedAddressV2(keys, 0, HrpV2For(tx)); all.push_back({DecodeShieldedAddress(own.address), change, {}}); }
    std::vector<std::tuple<csh::Hash, v2::BundleOutputWitness, EncryptedNote, bool>> rows;  // cm, witness, ciphertext, is_own
    for (const auto& o : all) {
        v2::BundleOutputWitness ow{}; ow.value = ValueToHash(o.value_una); ow.randomness = RandomHash();
        std::memcpy(ow.d.data(), o.to.d.data(), o.to.d.size());
        ow.public_key = csh::AuthRecipientCommitmentKey(o.to.pk_d_spend, o.to.nfk_commitment);
        const csh::Hash cm = csh::NoteCommitment(ow.d, ow.public_key, ow.value, ow.randomness);
        NotePlaintext pt{}; pt.d = o.to.d; pt.value_una = o.value_una; pt.rcm = ow.randomness; pt.memo = o.memo;
        rows.emplace_back(cm, ow, EncryptNoteForRecipient(o.to.d, o.to.pk_d, pt), o.to.pk_d_spend == DeriveDiversifiedAddressV2(keys, 0, HrpV2For(tx)).pk_d_spend);
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return std::get<0>(a) < std::get<0>(b); });
    for (auto& [cm, ow, enc, own] : rows) { bundle.outputs.push_back({cm, {}, std::vector<uint8_t>(enc.begin(), enc.end()), {}}); w.outputs.push_back(ow); }
    // Sighash excludes the proof (Task 4 test), so serialize once without it, prove, then serialize with it.
    tx.shielded_bundle_bytes = csh::SerializeShieldedBundleV2(bundle);
    const csh::Hash sighash = csh::ComputeShieldedTxSighash(tx);
    bundle.v2_proof = v2::ProveBundleV2(w, v2::BundlePublicInputs::FromBundle(bundle, sighash), nullptr);
    if (bundle.v2_proof.empty()) { out.status = shielded_ops::OpStatus::ProofError; out.error = "bundle proof generation failed"; return out; }
    tx.shielded_bundle_bytes = csh::SerializeShieldedBundleV2(bundle);
    // Persist own outputs as unconfirmed HashKeyV2 notes (same AddNote call the Auth path uses, scheme 3).
    ...
    out.status = shielded_ops::OpStatus::Ok; return out;
}
```

`HrpV2For(tx)` maps the daemon's chain to `kHrpMainnetV2/kHrpTestnetV2/kHrpRegtestV2` the same way the existing ops pick `kHrpMainnet/...` (grep `kHrpMainnet` in `shielded_wallet_ops.cpp` and mirror). `ValueToHash` and `RandomHash` are the existing helpers in `shielded_wallet_ops.cpp`; move them to a small internal header `src/wallet/shielded_ops_util.h` so both files share one definition (do not duplicate).

`BuildMigrateToV2Tx` derives address 0 under `hrp_v2`, then calls the existing `BuildAddressedTransferBundleForTx` (`shielded_wallet_ops.cpp:1228`) with that address as the single recipient, amount `Σ notes − fee_una`, no change. The existing builder already accepts any decodable address; the only new behaviour is that `DecodeShieldedAddress` now accepts the v2 HRP, so the change here is a thin wrapper plus a test.

`RescanConfirmedBlockV2`: locate the ownership check in the existing scan (`grep -n DeriveDiversifiedSpendPublicKey src/wallet/shielded_wallet_ops.cpp`, the site that recomputes `pk` from `(ak, d)` and compares `NoteCommitment`); extend it to also try `csh::AuthRecipientCommitmentKey(csh::v2::DiversifiedSpendPublicKeyV2(keys.hk, d_padded), NullifierKeyCommitment(nfk))` and store `key_scheme = HashKeyV2` on a match. `RescanConfirmedBlockV2` is the exported per-transaction entry that the test and the daemon's block-scan loop both call.

- [ ] **Step 5: RPC and prover kit**

In `src/rpc/shielded_rpc_json.cpp`, next to the existing `wallet.shield` registration (`:1764`), register `wallet.shieldedaddressv2`, `wallet.transferv2`, `wallet.shieldedmigratev2`, each following the `wallet.unshield` handler's shape (`:701-867`: unlock check → note selection → builder → sign/submit). `wallet.transferv2` requires all selected notes to be `HashKeyV2` and refuses when `V2RulesFor(Params()).Active(next_height)` is false with error `shielded-v2-not-active`. `wallet.shieldedmigratev2` selects up to `kAuthMaxSpends` `Auth` notes and refuses when there are none.

Prover kit: add the request/entry point declared in Interfaces; implementation calls `BuildV2BundleForTx` on a deserialized transaction exactly as the Auth unshield entry does, and cleanses `ask32`/`nvk32` copies on every exit (mirror the existing cleanse pattern in `shielded_prover_kit.cpp`).

- [ ] **Step 6: Run the tests**

Run: `cmake --build build-spike --target test_shielded_v2_wallet test_shielded_validation dinerod && ./build-spike/test_shielded_v2_wallet && ctest --test-dir build-spike -R 'ShieldedValidation$|ShieldedV2' --output-on-failure`
Expected: all PASS; the Auth address tests inside `ShieldedValidation` still pass (decoder accepts both HRP families).

- [ ] **Step 7: Commit**

```bash
git add include/wallet/shielded_note_store.h include/wallet/shielded_derivation.h src/wallet/shielded_derivation.cpp include/wallet/shielded_v2_wallet_ops.h src/wallet/shielded_v2_wallet_ops.cpp src/wallet/shielded_ops_util.h src/wallet/shielded_wallet_ops.cpp include/wallet/shielded_wallet_ops.h src/rpc/shielded_rpc_json.cpp include/shielded_prover_kit/shielded_prover_kit.h src/shielded_prover_kit/shielded_prover_kit.cpp src/consensus/shielded/CMakeLists.txt tests/wallet/test_shielded_v2_wallet.cpp tests/wallet/shielded_v2_test_helpers.h tests/CMakeLists.txt
git commit -m "shielded-v2: hash-key v2 addresses, v7 bundle builder, migration RPC, prover-kit entry

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: Parallel pre-verification before the ingress lock

**Files:**
- Create: `include/consensus/shielded/v2/batch_verifier.h`, `src/consensus/shielded/v2/batch_verifier.cpp`
- Modify: `src/daemon/block_acceptor.cpp:721` (first statement of `AcceptBlockFromPeer`)
- Modify: `src/consensus/shielded/CMakeLists.txt`
- Test: `tests/consensus/test_shielded_v2_batch.cpp` (target `test_shielded_v2_batch`, ctest `ShieldedV2Batch`, TIMEOUT 900)

**Interfaces:**
- Consumes: `VerifyBundleV2`, `BundlePublicInputs::FromBundle` (Task 3); `DeserializeShieldedBundleForVersion` (Task 2); `ComputeShieldedTxSighash`; `ShieldedV2Rules` (Task 1).
- Produces: `struct PrewarmStats { size_t candidates = 0, verified = 0, failed = 0, skipped = 0; double wall_ms = 0; };` `PrewarmStats PrewarmBundleProofCacheV2(const Block& block, uint32_t height, const ShieldedV2Rules& rules, size_t threads);` Thread count 0 means `std::thread::hardware_concurrency()` capped at 16.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/consensus/test_shielded_v2_batch.cpp
#include "consensus/shielded/v2/batch_verifier.h"
#include "consensus/shielded/v2/bundle_circuit.h"
#include "consensus/shielded/v2/serialization.h"
#include "primitives/block.h"
#include <gtest/gtest.h>
#include <chrono>
// reuse BuildV7 from tests/consensus/shielded_v2_fixtures.h (extract it from test_shielded_v2_validation.cpp in this task; both tests include the header)
#include "shielded_v2_fixtures.h"

namespace {
using namespace dinero::consensus::shielded;
namespace v2 = dinero::consensus::shielded::v2;

dinero::Block BlockOf(std::vector<dinero::Transaction> txs) {
    dinero::Block b; b.vtx.push_back(dinero::Transaction{}); /* coinbase placeholder */
    for (auto& t : txs) b.vtx.push_back(std::move(t));
    return b;
}
const ShieldedV2Rules kLive{true, 100, UINT32_MAX};

TEST(ShieldedV2Batch, PrewarmVerifiesEveryVersionSevenTxAndCachesOnlySuccesses) {
    std::vector<dinero::Transaction> txs;
    for (uint8_t i = 1; i <= 6; ++i) txs.push_back(BuildV7(2, 2, -1000, i).tx);
    auto bad = BuildV7(1, 1, -1000, 77); bad.bundle.v2_proof[10] ^= 1; bad.tx.shielded_bundle_bytes = SerializeShieldedBundleV2(bad.bundle);
    txs.push_back(bad.tx);
    dinero::Transaction plain; plain.version = 2; txs.push_back(plain);
    const auto stats = v2::PrewarmBundleProofCacheV2(BlockOf(txs), 100, kLive, 4);
    EXPECT_EQ(stats.candidates, 7u);
    EXPECT_EQ(stats.verified, 6u);
    EXPECT_EQ(stats.failed, 1u);
    // Warm: each good tx now verifies from cache in well under a millisecond.
    for (size_t i = 0; i < 6; ++i) {
        ShieldedBundle b; ASSERT_EQ(DeserializeShieldedBundleForVersion(7, txs[i].shielded_bundle_bytes, &b), BundleDecodeError::Ok);
        const auto pub = v2::BundlePublicInputs::FromBundle(b, ComputeShieldedTxSighash(txs[i]));
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_TRUE(v2::VerifyBundleV2(b.v2_proof, pub, dinero::crypto::GetSecp256k1ContextSignVerify()));
        EXPECT_LT(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), 5.0);
    }
    ShieldedBundle bb; ASSERT_EQ(DeserializeShieldedBundleForVersion(7, bad.tx.shielded_bundle_bytes, &bb), BundleDecodeError::Ok);
    EXPECT_FALSE(v2::VerifyBundleV2(bb.v2_proof, v2::BundlePublicInputs::FromBundle(bb, ComputeShieldedTxSighash(bad.tx)), dinero::crypto::GetSecp256k1ContextSignVerify()));
}

TEST(ShieldedV2Batch, PrewarmIsANoOpWhenDormantOrBeforeActivation) {
    auto tx = BuildV7(1, 1, -1000, 5).tx;
    EXPECT_EQ(v2::PrewarmBundleProofCacheV2(BlockOf({tx}), 100, {}, 2).skipped, 1u);
    EXPECT_EQ(v2::PrewarmBundleProofCacheV2(BlockOf({tx}), 99, kLive, 2).skipped, 1u);
}

TEST(ShieldedV2Batch, ParallelIsFasterThanSerialFor16Proofs) {
    std::vector<dinero::Transaction> txs;
    for (uint8_t i = 1; i <= 16; ++i) txs.push_back(BuildV7(2, 2, -1000, 100 + i).tx);
    const auto serial = v2::PrewarmBundleProofCacheV2(BlockOf(txs), 100, kLive, 1);
    std::vector<dinero::Transaction> txs2;
    for (uint8_t i = 1; i <= 16; ++i) txs2.push_back(BuildV7(2, 2, -1000, 200 + i).tx);
    const auto parallel = v2::PrewarmBundleProofCacheV2(BlockOf(txs2), 100, kLive, 8);
    EXPECT_EQ(serial.verified, 16u); EXPECT_EQ(parallel.verified, 16u);
    EXPECT_LT(parallel.wall_ms, serial.wall_ms * 0.6) << "serial " << serial.wall_ms << " ms, parallel " << parallel.wall_ms << " ms";
}
}  // namespace
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build-spike --target test_shielded_v2_batch`
Expected: compile error, `v2/batch_verifier.h` missing.

- [ ] **Step 3: Implement**

```cpp
// include/consensus/shielded/v2/batch_verifier.h
#pragma once
#include "consensus/shielded/v2/rules.h"
#include "primitives/block.h"
#include <cstddef>
namespace dinero::consensus::shielded::v2 {
struct PrewarmStats { size_t candidates = 0, verified = 0, failed = 0, skipped = 0; double wall_ms = 0; };
/// Verifies every version-7 shielded proof in `block` on `threads` workers (0 = hardware
/// concurrency, max 16) and records successes in the v2 proof cache. Never fails the block:
/// consensus validation re-runs the same VerifyBundleV2 and hits the cache. Call BEFORE any
/// consensus lock (spec §3.4).
PrewarmStats PrewarmBundleProofCacheV2(const Block& block, uint32_t height, const ShieldedV2Rules& rules, size_t threads = 0);
}
```

```cpp
// src/consensus/shielded/v2/batch_verifier.cpp
#include "consensus/shielded/v2/batch_verifier.h"
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/v2/bundle_circuit.h"
#include "consensus/shielded/v2/serialization.h"
#include "crypto/evp_secp256k1.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace dinero::consensus::shielded::v2 {
PrewarmStats PrewarmBundleProofCacheV2(const Block& block, uint32_t height, const ShieldedV2Rules& rules, size_t threads) {
    PrewarmStats st;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<const Transaction*> jobs;
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const auto& tx = block.vtx[i];
        if (!Transaction::IsShieldedBundleV2Version(tx.version) || tx.shielded_bundle_bytes.empty()) continue;
        ++st.candidates;
        if (!rules.Active(height)) { ++st.skipped; continue; }
        jobs.push_back(&tx);
    }
    if (jobs.empty()) { st.wall_ms = 0; return st; }
    if (threads == 0) threads = std::min<size_t>(16, std::max<unsigned>(1, std::thread::hardware_concurrency()));
    threads = std::min(threads, jobs.size());
    std::atomic<size_t> next{0}, ok{0}, bad{0};
    auto worker = [&] {
        auto* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();
        for (size_t j = next.fetch_add(1); j < jobs.size(); j = next.fetch_add(1)) {
            ShieldedBundle b;
            if (DeserializeShieldedBundleForVersion(jobs[j]->version, jobs[j]->shielded_bundle_bytes, &b) != BundleDecodeError::Ok ||
                b.spends.size() > kV2MaxSpends || b.outputs.size() > kV2MaxOutputs || b.v2_proof.size() > kV2MaxEnvelopeBytes) { ++bad; continue; }
            const auto pub = BundlePublicInputs::FromBundle(b, ComputeShieldedTxSighash(*jobs[j]));
            (VerifyBundleV2(b.v2_proof, pub, ctx) ? ok : bad)++;
        }
    };
    std::vector<std::thread> pool;
    for (size_t t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
    st.verified = ok; st.failed = bad;
    st.wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return st;
}
}  // namespace dinero::consensus::shielded::v2
```

Check that `GetSecp256k1ContextSignVerify()` is safe to call concurrently from several threads (read `src/crypto/evp_secp256k1.cpp`); if it lazily initialises a shared context without a lock, call it once on the calling thread before spawning workers and pass the pointer in. `VerifyBundleV2`'s cache is already mutex-protected (`VerifiedProofCache`).

Call site, `src/daemon/block_acceptor.cpp:721`, first lines of `AcceptBlockFromPeer`:

```cpp
    {
        const auto st = consensus::shielded::v2::PrewarmBundleProofCacheV2(
            block, /*height=*/block.header.height, consensus::shielded::V2RulesFor(dinero::Params()));
        if (st.candidates) LOG_INFO("[ShieldedV2] prewarm peer=" + peer_id + " candidates=" + std::to_string(st.candidates) +
                                    " verified=" + std::to_string(st.verified) + " failed=" + std::to_string(st.failed) +
                                    " ms=" + std::to_string(static_cast<int>(st.wall_ms)));
    }
```

Use whatever field the `Block` type exposes for its height (`grep -n 'height' include/primitives/block.h`); if the header carries no height, look it up from the parent index without taking `g_block_index_mutex` for longer than the lookup, and skip the prewarm when the parent is unknown (orphans are verified on the normal path later). The RPC path (`AcceptBlockFromRPC`) parses after the lock and is left alone: pool submissions are one block per minute.

- [ ] **Step 4: Run the tests**

Run: `cmake --build build-spike --target test_shielded_v2_batch dinerod && ./build-spike/test_shielded_v2_batch`
Expected: 3 PASS; the parallel test prints both timings.

- [ ] **Step 5: Commit**

```bash
git add include/consensus/shielded/v2/batch_verifier.h src/consensus/shielded/v2/batch_verifier.cpp src/daemon/block_acceptor.cpp src/consensus/shielded/CMakeLists.txt tests/consensus/test_shielded_v2_batch.cpp tests/consensus/shielded_v2_fixtures.h tests/consensus/test_shielded_v2_validation.cpp tests/CMakeLists.txt
git commit -m "shielded-v2: parallel proof prewarm before the block ingress lock

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: Release-only performance gate

**Files:**
- Create: `tests/consensus/test_shielded_v2_perf.cpp` (target `test_shielded_v2_perf`, ctest `ShieldedV2Performance`, registered only when `CMAKE_BUILD_TYPE STREQUAL "Release"`, labels `shielded;performance;mandatory`, TIMEOUT 1200)
- Modify: `tests/CMakeLists.txt`
- Create: `docs/benchmarks/shielded-v2-phase1-gates.md`

**Interfaces:**
- Consumes: `BuildV7` fixture (Task 6's header), `VerifyBundleV2`, `ProveBundleV2`, `PrewarmBundleProofCacheV2`.

- [ ] **Step 1: Write the test (it is the gate; it fails until the implementation meets it)**

```cpp
// tests/consensus/test_shielded_v2_perf.cpp
#include "consensus/shielded/v2/batch_verifier.h"
#include "consensus/shielded/v2/bundle_circuit.h"
#include "consensus/shielded/v2/serialization.h"
#include "shielded_v2_fixtures.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>

namespace {
using namespace dinero::consensus::shielded;
namespace v2 = dinero::consensus::shielded::v2;
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// Gates from the plan's Global Constraints (CI runner class). Local numbers are printed, not asserted.
constexpr size_t kMaxProofBytes = 20480;
constexpr double kMaxVerifyMs = 140.0;
constexpr double kMaxBatchedMsPerProof = 25.0;
constexpr double kMaxProveMs = 1000.0;

TEST(ShieldedV2Performance, TwoInTwoOutMeetsGates) {
#ifndef NDEBUG
    GTEST_SKIP() << "Release only";
#endif
    std::vector<double> prove, verify; size_t bytes = 0;
    for (uint8_t i = 1; i <= 5; ++i) {
        auto t0 = Clock::now();
        auto b = BuildV7(2, 2, -1000, i);      // includes ProveBundleV2
        prove.push_back(Ms(t0));
        ShieldedBundle bundle; ASSERT_EQ(DeserializeShieldedBundleV2(b.tx.shielded_bundle_bytes, &bundle), BundleDecodeError::Ok);
        bytes = bundle.v2_proof.size();
        const auto pub = v2::BundlePublicInputs::FromBundle(bundle, ComputeShieldedTxSighash(b.tx));
        t0 = Clock::now();
        ASSERT_TRUE(v2::VerifyBundleV2(bundle.v2_proof, pub, dinero::crypto::GetSecp256k1ContextSignVerify()));
        verify.push_back(Ms(t0));
    }
    std::sort(prove.begin(), prove.end()); std::sort(verify.begin(), verify.end());
    std::printf("SHIELDED_V2_PERF proof_bytes=%zu prove_ms_median=%.1f verify_ms_median=%.1f\n", bytes, prove[2], verify[2]);
    EXPECT_LE(bytes, kMaxProofBytes);
    EXPECT_LE(verify[2], kMaxVerifyMs);
    EXPECT_LE(prove[2], kMaxProveMs);
}

TEST(ShieldedV2Performance, FiftyProofBlockBatched) {
#ifndef NDEBUG
    GTEST_SKIP() << "Release only";
#endif
    dinero::Block block; block.vtx.emplace_back();
    for (uint8_t i = 1; i <= 50; ++i) block.vtx.push_back(BuildV7(2, 2, -1000, 50 + i).tx);
    const auto st = v2::PrewarmBundleProofCacheV2(block, 100, {true, 100, UINT32_MAX}, 8);
    ASSERT_EQ(st.verified, 50u);
    std::printf("SHIELDED_V2_PERF batched_ms_per_proof=%.2f wall_ms=%.0f\n", st.wall_ms / 50.0, st.wall_ms);
    EXPECT_LE(st.wall_ms / 50.0, kMaxBatchedMsPerProof);
}
}  // namespace
```

CMake: wrap the target and `add_test` in `if(CMAKE_BUILD_TYPE STREQUAL "Release")`. The CI lane (Task 8) builds Release and runs `-R ShieldedV2Performance`; the two `SHIELDED_V2_PERF` lines are grepped into the evidence artifact.

- [ ] **Step 2: Run locally**

Run: `cmake --build build-spike --target test_shielded_v2_perf && ./build-spike/test_shielded_v2_perf`
Expected: PASS on M4 Max with margins; record the two printed lines in `docs/benchmarks/shielded-v2-phase1-gates.md` together with the Task 0 numbers and the gate table.

- [ ] **Step 3: Commit**

```bash
git add tests/consensus/test_shielded_v2_perf.cpp tests/CMakeLists.txt docs/benchmarks/shielded-v2-phase1-gates.md
git commit -m "shielded-v2: Release-only performance gate (size, verify, batched, prove)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: Consensus vectors, fuzzers, sanitizer lane, vector lane, shielded e2e lane

**Files:**
- Create: `tests/vectors/shielded_v2_v1/{manifest.json,valid_1in2out.tx.hex,valid_2in2out.tx.hex,valid_4in2out.tx.hex,README.md}`
- Create: `tests/consensus/test_shielded_v2_vectors.cpp` (generator + consumer; target `test_shielded_v2_vectors`, ctests `ShieldedV2Vectors` and `ShieldedV2VectorsRegenerateCheck`)
- Create: `tests/fuzz/fuzz_shielded_v2_envelope.cpp`, `tests/fuzz/fuzz_shielded_v2_bundle_codec.cpp`, `tests/fuzz/fuzz_shielded_v2_verify.cpp` (target group `shielded_v2_fuzzers`, built only with `-DENABLE_FUZZ=ON` and clang)
- Create: `.github/workflows/shielded-v2-vectors.yml`, `.github/workflows/shielded-v2-sanitizers.yml`, `.github/workflows/shielded-e2e.yml`
- Modify: `scripts/ci/unexecuted_tests_baseline.txt` (remove the 22 shielded e2e names once `shielded-e2e.yml` runs them)

**Interfaces:**
- Consumes: `BuildV7` fixture; everything from Tasks 1–6.
- Produces: fixed hex transactions with pinned `txid`, `sighash`, `wire_bytes`; a regeneration check that fails when the statement, codec, transcript or sighash preimage drifts.

- [ ] **Step 1: Generator and consumer test**

```cpp
// tests/consensus/test_shielded_v2_vectors.cpp
// Consumer: reads tests/vectors/shielded_v2_v1/manifest.json + *.tx.hex, re-validates each
// transaction under the manifest's context, and pins txid/sighash/wire bytes.
// Generator: with SHIELDED_V2_WRITE_VECTORS=<dir> set, rebuilds the fixtures deterministically
// (fixed seeds, fixed esk) and writes them; CI runs it into a temp dir and diffs against the
// committed files (ShieldedV2VectorsRegenerateCheck), so any consensus drift is a red lane.
```

Vectors (all at height 101 under rules `{true, 101, 300}`, Auth scheduled at 4):

| file | shape | expected |
|---|---|---|
| `valid_1in2out.tx.hex` | 1-in-2-out, vb = −1000 | Ok |
| `valid_2in2out.tx.hex` | 2-in-2-out, vb = −1000 | Ok |
| `valid_4in2out.tx.hex` | 4-in-2-out, vb = −1000 | Ok |
| derived in-test from `valid_2in2out` | `value_balance` −999 with delta −999 | ProofInvalid (wrong balance) |
| derived | anchor byte flipped | AnchorInvalid |
| derived | nullifier duplicated | NullifierDuplicate |
| derived | `lockTime` changed | ProofInvalid (sighash binding) |
| derived | height 100 | NotActive (v2 before activation) |
| derived | v6 legacy bundle at height 300 | ProofVersionSunset |
| derived | envelope id 0x7F | ProofInvalid (unknown id) |

The consumer asserts every row; the manifest stores `txid`, `tx_sighash`, `wire_bytes`, `proof_bytes`, `constraints` per valid vector. Deterministic generation needs `BuildV7` to take an explicit RNG seed for `rcm`/`esk` (it already takes `seed`), and `ProveBundleV2`'s Hyrax blinding must be seeded for the regenerate check: add a test-hook `v2::SetDeterministicProverRngForTests(uint64_t)` under `DINERO_SHIELDED_V2_TEST_HOOKS` that the generator calls; production builds have no such symbol. If Hyrax blinding cannot be seeded through the existing `dinero_zk` API, the regenerate check compares everything except `proof_bytes` and additionally asserts that the regenerated proof verifies under the committed public inputs.

- [ ] **Step 2: Fuzzers**

Each fuzzer is ~20 lines: `LLVMFuzzerTestOneInput` feeds `data` to `DecodeV2Envelope`, `DeserializeShieldedBundleV2` (then re-serialises and asserts byte equality on `Ok`), and `VerifyBundleV2` with a fixed 2-in-2-out public-input set (must return false quickly and never crash). Build with `-fsanitize=fuzzer,address,undefined`; CI runs each for 60 s with the committed seed corpus `tests/fuzz/corpus/shielded_v2/*` (the three valid vectors and 20 mutations).

- [ ] **Step 3: Workflows**

`shielded-v2-vectors.yml`: copy `compact-regtest-vectors.yml`, path filters plus `tests/vectors/shielded_v2_v1/**`, `include/consensus/shielded/v2/**`, `src/consensus/shielded/v2/**`, `tests/consensus/test_shielded_v2_*.cpp`; matrix `ubuntu-24.04`, `ubuntu-24.04-arm`; builds Release and runs `ctest -R '^(ShieldedV2Activation|ShieldedV2Codec|ShieldedV2Circuit|ShieldedV2Validation|ShieldedV2Batch|ShieldedV2Vectors|ShieldedV2VectorsRegenerateCheck|ShieldedV2Performance)$' --no-tests=error`; uploads `evidence/`.

`shielded-v2-sanitizers.yml`: same triggers; two jobs, `-DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"` and `-fsanitize=undefined`, building only the v2 test targets plus `test_shielded_validation`; runs the same ctest regex minus `ShieldedV2Performance`; third job builds `shielded_v2_fuzzers` with clang and runs each fuzzer 60 s.

`shielded-e2e.yml`: nightly + `workflow_dispatch` + path filter on `src/consensus/shielded/**`, `src/wallet/shielded_*`, `src/rpc/shielded_*`; builds `dinerod` + integration harness and runs `ctest -R '^(ShieldedDaemonRestartEquivalence|ShieldedReorgDisconnectRestartEquivalence|ShieldedReorgSecondRestartInvalidityEquivalence|ShieldedTipMarkerRestartEquivalence|ShieldedTipPersistRestartEquivalence|CSNRecoveryShieldedApply|CSNShieldedSpendSync|ShieldedEpochResetBoundary|ShieldedEpochResetStateless|ShieldedOutputsRpcFeed|ShieldedReorgInvertibility|ShieldedReorgInvertibility_AtomicPersistOn|ShieldedReorgInvertibility_AtomicPersistToggleOffToOn|ShieldedReorgInvertibility_AtomicPersistToggleOnToOff|ShieldedRpcGetAddress|ShieldedRpcShieldEndToEnd|ShieldedRpcTransferAddressedDetectEndToEnd|ShieldedRpcTransferAddressedEndToEnd|ShieldedRpcTransferEndToEnd|ShieldedRpcTransferMultiEndToEnd|ShieldedRpcUnshieldEndToEnd|<22nd name from the baseline file>)$' --no-tests=error --timeout 1800`. Then delete those names from `scripts/ci/unexecuted_tests_baseline.txt` and confirm `python3 scripts/ci/check_mandatory_tests_execute.py` still passes with the new lane listed.

- [ ] **Step 4: Run**

Run locally: `ctest --test-dir build-spike -R 'ShieldedV2Vectors|ShieldedV2VectorsRegenerateCheck' --output-on-failure`; push the branch and confirm all three new workflows are green on the PR (`gh run list -R DineroLabs/dinero-v8 --branch claude/shielded-v2 --limit 6`). For the e2e lane, verify in the log that each of the 22 tests actually executed (`grep -c 'Test #' ctest.log` = 22), per the repo's verification rule.

- [ ] **Step 5: Commit**

```bash
git add tests/vectors/shielded_v2_v1 tests/consensus/test_shielded_v2_vectors.cpp tests/fuzz .github/workflows/shielded-v2-vectors.yml .github/workflows/shielded-v2-sanitizers.yml .github/workflows/shielded-e2e.yml scripts/ci/unexecuted_tests_baseline.txt tests/CMakeLists.txt
git commit -m "shielded-v2: consensus vectors, fuzzers, sanitizer and vector lanes, shielded e2e lane

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Regtest harness across activation, reorg, restart, sunset, replay

**Files:**
- Create: `tests/integration/test_shielded_v2_regtest.py` (or the harness language the combined migration harness uses; find it with `grep -rn 'combined-migration' .github/workflows/combined-migration-qualification.yml`)
- Modify: `.github/workflows/combined-migration-qualification.yml` (add the v2 scenario job)

**Interfaces:**
- Consumes: `dinerod --consensus-shielded-v2-height=H --consensus-shielded-v1-sunset-height=S` (Task 1), RPCs from Task 5.

- [ ] **Step 1: Scenario (each numbered step is an assertion in the harness)**

Two regtest nodes A and B, Auth at 4, v2 at H = 40, sunset S = 60.

1. Mine to 30. Shield 5 DIN into an Auth address on A (`wallet.shield`). Mine 1. Balance visible on A and B.
2. At 35: `wallet.transferv2` → error `shielded-v2-not-active`. Mine to 40.
3. At 40: `wallet.shieldedmigratev2` on A (v6 tx). Mine 1. A's unspent notes: one `HashKeyV2` note. B validates the block (same tip).
4. At 41: `wallet.transferv2` to B's v2 address (v7 tx). Mine 1. B sees the note with ivk; both tips equal; `getblock` shows the tx with version 7.
5. Reorg across activation: disconnect B, mine 3 blocks on A (including a v7 tx) and 4 on B (including a v6 tx), reconnect. Both converge on B's chain; A's v7 tx returns to A's mempool and is re-mined at ≥ 44; A's note store shows the notes unspent then spent again.
6. Restart A and B (graceful stop, start). Heights, best hashes, shielded roots (`getshieldedroot` or the existing state RPC) equal before and after.
7. Mine to 60. At 60 a v6 shielded tx is rejected by the mempool with `shielded-proof-version-sunset`; a v7 tx is accepted. A block containing a v6 shielded tx submitted via `submitblock` is rejected with `shielded-proof-version-sunset`.
8. Replay: stop B, delete its chainstate, restart with `-reindex`; B reaches the same tip and shielded root as A.

- [ ] **Step 2: Wire into the qualification workflow** as a separate job `shielded-v2-regtest` with the same runner and timeout as the existing combined harness; upload the node logs as evidence.

- [ ] **Step 3: Run once locally against `build-spike/dinerod`** and once in CI; both must pass before the plan is called complete.

- [ ] **Step 4: Commit**

```bash
git add tests/integration/test_shielded_v2_regtest.py .github/workflows/combined-migration-qualification.yml
git commit -m "shielded-v2: regtest qualification across activation, reorg, restart, sunset, replay

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10: Documentation and hand-off

**Files:**
- Modify: `docs/superpowers/specs/2026-09-22-shielded-v2-design.md` (mark §10 approved/rejected per owner; fold the decision into §3.1/§3.5/§4)
- Create: `docs/shielded-v2-operator-notes.md` (what changes for node operators, pool operators and wallet users; nothing until heights are set)
- Modify: `~/src/MemoryMD/dinero-shielded-v2.md` (local only, never pushed)

- [ ] **Step 1:** Update the spec with the owner's decision on §10 and record the measured numbers from Tasks 0 and 7 in §2's table as a third column "measured (phase 1)".
- [ ] **Step 2:** Write the operator notes: version-7 transactions appear only after `shielded_v2_activation_height`; pool software needs no change (templates carry v7 txs like any other); wallets need the v2 address (`dinz…`) and the migration RPC; the v1 sunset schedule is announced separately.
- [ ] **Step 3:** Open the PR against `dinero-main` as **draft**, titled "shielded-v2 phase 1: bundle proofs behind dormant heights", body listing the blast-radius table, the gate results, and the owner decisions still open. Do not request merge.
- [ ] **Step 4:** Commit and push.

```bash
git add docs/superpowers/specs/2026-09-22-shielded-v2-design.md docs/shielded-v2-operator-notes.md
git commit -m "shielded-v2: spec decision record and operator notes

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
git push origin claude/shielded-v2
```

---

## Self-review

**Spec coverage.** §2 targets → Task 7 gates + Task 0 measurement. §3.1 statement (as amended §10.2) → Task 3. §3.2 envelope → Task 2. §3.3 phase 1 on the native prover → Task 3. §3.4 batch verification off the locks → Task 6 (prewarm before `AcceptBlockFromPeer`'s locked path; note: per-proof parallel verification gives failing-tx attribution for free, so §5's "reject batch then re-verify individually" applies only to the algebraic batching of phase 2). §3.5 coexistence → Tasks 1 and 4 (`V1Allowed`, `ProofVersionSunset`); migration → Task 5. §4 prover API → Task 5 (C++ builder + prover-kit C entry; FFI crate deferred to phase 2 as the report says). §5 threat notes: sighash binding → Task 3 constraint + transcript + neuter test; caps before verification → Tasks 2 and 4; fee as public input → `vb_neg`. §6.1 vectors → Task 8; §6.2 neuter tests → Task 3; §6.3 perf ctest → Task 7; §6.4 sanitizers + fuzzers → Task 8; §6.5 regtest harness → Task 9; §6.6 the 22 e2e tests → Task 8 lane; §6.7 every rule with a test → every task. §10 → Tasks 0, 3, 5.

**Gaps found and fixed while reviewing.** (1) The spec's §3.1 "fee" public input could not express shield/unshield; replaced with `vb_pos`/`vb_neg` derived from `value_balance`, consistent with v1's `value_balance == transparent_delta` rule. (2) The sighash must not cover the proof bytes (circularity); Task 4's first test pins that the version-7 sighash preimage excludes `v2_proof`, which holds because `ComputeShieldedTxSighash` hashes the transparent envelope, not the bundle bytes; if a reading of `binding_sig.cpp` shows the bundle bytes are hashed for v6, the version-7 branch must hash the bundle with `v2_proof` cleared. (3) `IsShieldedAuthVersion(7) == true` is load-bearing for txid commitment and the auth resource envelope; Task 2's test pins it.

**Placeholder scan.** No TBD/TODO. Two steps intentionally reference existing code to copy (`AutoFeeAuthNote`, the compact override parsing) with the grep that locates it; both are existing code, not other tasks. Task 8's e2e regex names 21 tests explicitly and tells the implementer to take the 22nd from the baseline file.

**Type consistency.** `BundlePublicInputs`, `BundleWitness`, `BundleSpendWitness`, `BundleOutputWitness`, `BundleCircuitNeuter`, `ShieldedV2Rules`, `V2Envelope`, `ProofSystemId`, `EnvelopeDecodeError`, `PrewarmStats`, `V2OutputRequest` are spelled identically in every task; `DeserializeShieldedBundleForVersion(int32_t, const std::vector<uint8_t>&, ShieldedBundle*)` and `BuildShieldedValidationContext(..., CompactShieldedRules, ShieldedV2Rules)` match between Tasks 2, 4, 5, 6.

## Execution handoff

Plan complete. Owner decisions that gate execution: approve spec §10 (Amendment A); everything else is dormant code behind `UINT32_MAX` heights and can be built and reviewed before any activation discussion. Recommended execution: subagent-driven, one fresh agent per task with review between tasks, on the `claude/shielded-v2` branch only.

# Shielded v2 Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure, with throwaway code, what the v2 bundle circuit costs on the existing Spartan+Hyrax prover (phase-1 estimate) and on candidate hash-based polynomial-commitment stacks (phase-2 estimate), and produce a go/no-go report with a concrete library choice.

**Architecture:** A standalone C++ benchmark executable (not a ctest) builds the v2 bundle statement from the existing R1CS gadgets (`src/zk/zkvm/gadgets.h`, `poseidon_gadget.h`) and drives `r1cs_spartan_prove`/`r1cs_spartan_verify` exactly as `ProveSpend` does, recording constraints, proof bytes, prove/verify time. A separate Rust workspace under `spike/` measures shape-equivalent circuits on candidate stacks. Output is JSON plus a report; nothing here is consensus code and nothing is merged to `dinero-main`.

**Tech Stack:** C++20 (existing `dinero_zk`, `dinero_shielded` libraries), CMake/Ninja, Rust 1.91 (`cargo`), Python 3 for tables.

**Spec:** `docs/superpowers/specs/2026-09-22-shielded-v2-design.md`

## Global Constraints

- Branch `claude/shielded-v2`, worktree `~/src/dinero-v8-shielded-v2`; never touch `dinero-main`, never build inside another worktree.
- Spike code lives under `tests/spike/` (C++) and `spike/` (Rust); it is labelled throwaway in file headers and is NOT registered with `add_test`.
- Release builds only for numbers (`-DCMAKE_BUILD_TYPE=Release`); report the median of 5 runs after 1 warm-up.
- Targets from the spec §2 (phase 1: proof < 16 KB, verify < 10 ms per 2-in-2-out; phase 2: proof < 64 KB, verify < 10 ms).
- Every measurement lands in `docs/benchmarks/shielded-v2-spike-YYYYMMDD.json` with machine, commit and build type recorded.
- Commit after every task; do not push consensus source changes (there are none in this plan).

---

### Task 1: Spike harness skeleton reproducing today's per-spend cost

**Files:**
- Create: `tests/spike/shielded_v2_bundle_bench.cpp`
- Modify: `tests/CMakeLists.txt` (append a block after the `test_shielded_cv_binding` block)

**Interfaces:**
- Consumes: `dinero::consensus::shielded::ProveSpend/VerifySpend/ProveOutput/VerifyOutput` (`include/consensus/shielded/shielded_circuit.h:131-191`), `SpendWitness`, `SpendPublicInputs`, `OutputWitness`, `OutputPublicInputs`.
- Produces: `struct BenchRow { std::string label; size_t constraints, variables, proof_bytes; double prove_ms, verify_ms; }` and `void PrintJson(const std::vector<BenchRow>&)` reused by Tasks 2–4.

- [ ] **Step 1: Create the harness with a baseline measurement of one cv-bound spend + one output**

```cpp
// tests/spike/shielded_v2_bundle_bench.cpp — THROWAWAY spike code (spec §7). Not a ctest.
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/pedersen_generators.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace dinero::consensus::shielded;
using Clock = std::chrono::steady_clock;

struct BenchRow { std::string label; size_t constraints = 0, variables = 0, proof_bytes = 0; double prove_ms = 0, verify_ms = 0; };

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static void PrintJson(const std::vector<BenchRow>& rows, const char* build_type) {
    std::printf("{\"build\":\"%s\",\"rows\":[", build_type);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        std::printf("%s{\"label\":\"%s\",\"constraints\":%zu,\"variables\":%zu,\"proof_bytes\":%zu,\"prove_ms\":%.1f,\"verify_ms\":%.1f}",
                    i ? "," : "", r.label.c_str(), r.constraints, r.variables, r.proof_bytes, r.prove_ms, r.verify_ms);
    }
    std::printf("]}\n");
}

// Baseline: today's cv-bound spend-auth spend proof + cv-bound output proof, 5 runs, median.
static BenchRow BaselineSpend() {
    SpendWitness w{};        // zero witness is acceptable for cost measurement: the circuit shape
    SpendPublicInputs pub{}; // does not depend on values; ProveSpend requires is_satisfied() so
                             // fill the public inputs from the witness with the native helpers if
                             // ProveSpend returns empty (see Step 3).
    std::vector<double> prove, verify; size_t bytes = 0;
    for (int i = 0; i < 6; ++i) {
        auto t0 = Clock::now();
        auto proof = ProveSpend(w, pub, nullptr, true, /*cv_bound=*/true, /*spend_auth=*/true);
        double p = ms_since(t0);
        if (proof.empty()) { std::fprintf(stderr, "baseline ProveSpend returned empty\n"); std::exit(2); }
        t0 = Clock::now();
        bool ok = VerifySpend(proof, pub, nullptr, true, true, true);
        double v = ms_since(t0);
        if (!ok) { std::fprintf(stderr, "baseline VerifySpend failed\n"); std::exit(2); }
        if (i) { prove.push_back(p); verify.push_back(v); }
        bytes = proof.size();
    }
    std::sort(prove.begin(), prove.end()); std::sort(verify.begin(), verify.end());
    return BenchRow{"baseline_spend_cv_auth", 0, 0, bytes, prove[2], verify[2]};
}

int main() {
    std::vector<BenchRow> rows;
    rows.push_back(BaselineSpend());
    PrintJson(rows, BUILD_TYPE_STR);
    return 0;
}
```

- [ ] **Step 2: Register the executable (no `add_test`)**

Append to `tests/CMakeLists.txt` right after the `test_shielded_cv_binding` block:

```cmake
# SPIKE (spec docs/superpowers/specs/2026-09-22-shielded-v2-design.md §7) — throwaway
# benchmark, deliberately NOT registered with add_test.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/spike/shielded_v2_bundle_bench.cpp)
  add_executable(spike_shielded_v2_bench tests/spike/shielded_v2_bundle_bench.cpp)
  add_dependencies(spike_shielded_v2_bench dinero_shielded)
  target_link_libraries(spike_shielded_v2_bench PRIVATE dinero_shielded dinero_wallet dinero_zk)
  target_include_directories(spike_shielded_v2_bench BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(spike_shielded_v2_bench PRIVATE BUILD_TYPE_STR="${CMAKE_BUILD_TYPE}")
  if(APPLE)
    target_link_libraries(spike_shielded_v2_bench PRIVATE "-framework Security")
  endif()
endif()
```

Copy the exact `target_link_libraries` list from the `test_shielded_cv_binding` block if it lists more libraries than shown above (e.g. `secp256k1`, `OpenSSL::Crypto`).

- [ ] **Step 3: Make the zero witness satisfiable, if needed**

Run the harness (Step 4). If it exits 2 with "returned empty", the circuit is not satisfied by a zero witness. Then build a consistent witness: look at `tests/consensus/test_shielded_cv_binding.cpp` lines 97–125 for how `OutputWitness`/`OutputPublicInputs` are filled (`pub.commitment` from the native Poseidon of the witness, `pub.cv` from `PedersenCommit`), and mirror it for the spend using `commitment_tree.h` to build a one-leaf tree and its auth path (`CommitmentTree::Append`, `AuthPath`). Keep this helper in the harness file as `MakeSpendFixture()`.

- [ ] **Step 4: Configure a Release build directory for this worktree and run**

```bash
cd ~/src/dinero-v8-shielded-v2
cmake -S . -B build-spike -G Ninja -DCMAKE_BUILD_TYPE=Release -DDINERO_USE_VENDORED_DEPS=ON \
  -DENABLE_GRPC=OFF -DENABLE_GPU_MINING=OFF -DDINERO_ENABLE_PORTMAPPING=OFF
cmake --build build-spike --target spike_shielded_v2_bench -j12
./build-spike/spike_shielded_v2_bench
```

Expected: one JSON line with `label":"baseline_spend_cv_auth"`, `proof_bytes` in the 35,000–80,000 range, `verify_ms` in the 800–2,000 range on the M4 Max (matches `docs/benchmarks/shielded-cost-20260915.json`). If `proof_bytes` is under 10 KB the cv-bound profile is not engaged: check the `cv_bound=true` argument.

- [ ] **Step 5: Commit**

```bash
git add tests/spike/shielded_v2_bundle_bench.cpp tests/CMakeLists.txt
git commit -m "spike(shielded-v2): benchmark harness reproducing today's cv-bound spend cost"
```

---

### Task 2: Bundle circuit builder with satisfiability tests

**Files:**
- Create: `tests/spike/bundle_circuit_v2.h` (header-only, throwaway)
- Modify: `tests/spike/shielded_v2_bundle_bench.cpp` (add `--selftest` mode)

**Interfaces:**
- Consumes: `dinero::zk::zkvm::R1CS` (`src/zk/zkvm/r1cs.h:138`: `Variable alloc(const Scalar&)`, `Variable alloc_input(const Scalar&)`, `void constrain(LinearCombination a, LinearCombination b, LinearCombination c, const std::string&)`, `enforce_equal`, `enforce_zero`, `num_constraints()`, `num_variables()`, `is_satisfied()`), gadgets in `src/zk/zkvm/gadgets.h` (`to_bits`, `range_check`, `select`, `add`, `sub`, `assert_equal`, `enforce_boolean`), `poseidon2_gadget(R1CS&, Variable, Variable, const std::string&)` and `poseidon2_native(const Scalar&, const Scalar&)` from `src/zk/zkvm/poseidon_gadget.h`, `TREE_DEPTH` and `Hash`/`HashToScalar` from `include/consensus/shielded/commitment_tree.h`.
- Produces:

```cpp
struct SpendLeg  { dinero::zk::zkvm::Scalar secret_key, nullifier_key, value, randomness, diversifier;
                   uint32_t leaf_index; std::array<dinero::consensus::shielded::Hash, TREE_DEPTH> siblings;
                   dinero::zk::zkvm::Scalar anchor, nullifier; /* public */ };
struct OutputLeg { dinero::zk::zkvm::Scalar value, public_key, randomness, diversifier; dinero::zk::zkvm::Scalar commitment; /* public */ };
struct BundleV2  { std::vector<SpendLeg> spends; std::vector<OutputLeg> outputs; uint64_t fee; dinero::zk::zkvm::Scalar sighash; };
dinero::zk::zkvm::R1CS BuildBundleCircuitV2(const BundleV2& b);   // public inputs allocated first, in order: sighash, fee, anchors[], nullifiers[], commitments[]
BundleV2 MakeHonestBundle(size_t n_in, size_t n_out, uint64_t fee); // deterministic fixture; builds a real tree with n_in leaves
```

- [ ] **Step 1: Write the self-test first (in the bench file, `--selftest` mode)**

```cpp
static int SelfTest() {
    using namespace dinero::zk::zkvm;
    int failures = 0;
    auto check = [&](bool c, const char* name) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", name); if (!c) ++failures; };

    BundleV2 honest = MakeHonestBundle(2, 2, /*fee=*/1000);
    check(BuildBundleCircuitV2(honest).is_satisfied(), "honest 2-in-2-out bundle is satisfied");

    BundleV2 bad_balance = honest; bad_balance.outputs[0].value = bad_balance.outputs[0].value + Scalar::one();
    check(!BuildBundleCircuitV2(bad_balance).is_satisfied(), "output value +1 breaks balance");

    BundleV2 bad_fee = honest; bad_fee.fee += 1;
    check(!BuildBundleCircuitV2(bad_fee).is_satisfied(), "fee +1 breaks balance");

    BundleV2 bad_anchor = honest; bad_anchor.spends[0].anchor = bad_anchor.spends[0].anchor + Scalar::one();
    check(!BuildBundleCircuitV2(bad_anchor).is_satisfied(), "wrong anchor breaks the Merkle path");

    BundleV2 bad_nf = honest; bad_nf.spends[1].nullifier = bad_nf.spends[1].nullifier + Scalar::one();
    check(!BuildBundleCircuitV2(bad_nf).is_satisfied(), "wrong nullifier breaks the spend");

    BundleV2 big_value = honest; big_value.spends[0].value = Scalar::from_u64(1) << 64;  // use the Scalar API available; if no shift, construct 2^64 via repeated doubling
    check(!BuildBundleCircuitV2(big_value).is_satisfied(), "65-bit value fails the range check");

    const auto cs = BuildBundleCircuitV2(honest);
    std::printf("  2-in-2-out constraints=%zu variables=%zu\n", cs.num_constraints(), cs.num_variables());
    check(cs.num_constraints() < 40000, "2-in-2-out is under 40k constraints (spec budget ≈ 2×8.4k + 2×0.5k + 200)");
    return failures == 0 ? 0 : 1;
}
```

Wire `int main(int argc, char** argv)`: if `argc > 1 && std::string(argv[1]) == "--selftest"` return `SelfTest()`.

- [ ] **Step 2: Build and run the self-test; it must FAIL to compile (builder missing)**

```bash
cmake --build build-spike --target spike_shielded_v2_bench -j12
```
Expected: compile error, `BuildBundleCircuitV2` / `MakeHonestBundle` undeclared.

- [ ] **Step 3: Implement the builder**

```cpp
// tests/spike/bundle_circuit_v2.h — THROWAWAY spike code (spec §3.1 statement). Not consensus code.
#pragma once
#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/gadgets.h"
#include "zk/zkvm/poseidon_gadget.h"
#include "consensus/shielded/commitment_tree.h"
#include <array>
#include <vector>

using dinero::zk::zkvm::R1CS; using dinero::zk::zkvm::Scalar; using dinero::zk::zkvm::Variable;
namespace gadgets = dinero::zk::zkvm::gadgets;   // adjust to the real namespace used in shielded_circuit.cpp:311
using dinero::consensus::shielded::Hash; using dinero::consensus::shielded::TREE_DEPTH; using dinero::consensus::shielded::HashToScalar;

struct SpendLeg  { Scalar secret_key, nullifier_key, value, randomness, diversifier; uint32_t leaf_index = 0;
                   std::array<Hash, TREE_DEPTH> siblings{}; Scalar anchor, nullifier; };
struct OutputLeg { Scalar value, public_key, randomness, diversifier; Scalar commitment; };
struct BundleV2  { std::vector<SpendLeg> spends; std::vector<OutputLeg> outputs; uint64_t fee = 0; Scalar sighash; };

static const Scalar ADDR_TAG = Scalar::from_u64(0x41444452);  // same tag the legacy circuit uses; copy the constant from shielded_circuit.cpp

// Merkle path: copy of shielded_circuit.cpp:297-330 (static there), kept verbatim.
static Variable MerklePathV2(R1CS& cs, Variable leaf, Variable leaf_index, const std::array<Hash, TREE_DEPTH>& siblings, const std::string& p) {
    Variable current = leaf;
    const auto bits = gadgets::to_bits(cs, leaf_index, TREE_DEPTH, p + "_idx_bits");
    for (size_t d = 0; d < TREE_DEPTH; ++d) {
        Variable sib = cs.alloc(HashToScalar(siblings[d]));
        Variable left  = gadgets::select(cs, bits[d], sib, current, p + "_l" + std::to_string(d));
        Variable right = gadgets::select(cs, bits[d], current, sib, p + "_r" + std::to_string(d));
        current = dinero::zk::zkvm::poseidon2_gadget(cs, left, right, p + "_h" + std::to_string(d));
    }
    return current;
}

static Variable NoteCommitmentV2(R1CS& cs, Variable diversifier, Variable public_key, Variable value, Variable randomness, const std::string& p) {
    Variable dpk  = dinero::zk::zkvm::poseidon2_gadget(cs, diversifier, public_key, p + "_dpk");
    Variable tag  = gadgets::constant(cs, ADDR_TAG, p + "_tag");
    Variable bind = dinero::zk::zkvm::poseidon2_gadget(cs, tag, dpk, p + "_bind");
    Variable bv   = dinero::zk::zkvm::poseidon2_gadget(cs, bind, value, p + "_bv");
    return dinero::zk::zkvm::poseidon2_gadget(cs, bv, randomness, p + "_cm");
}

inline R1CS BuildBundleCircuitV2(const BundleV2& b) {
    R1CS cs;
    // Public inputs first, fixed order (spec §3.1).
    Variable sighash = cs.alloc_input(b.sighash);
    Variable fee     = cs.alloc_input(Scalar::from_u64(b.fee));
    std::vector<Variable> anchors, nullifiers, commitments;
    for (auto& s : b.spends)  { anchors.push_back(cs.alloc_input(s.anchor)); nullifiers.push_back(cs.alloc_input(s.nullifier)); }
    for (auto& o : b.outputs) { commitments.push_back(cs.alloc_input(o.commitment)); }
    // sighash is bound by being a public input the verifier supplies; consume it once so it is not a dangling input.
    cs.enforce_equal(dinero::zk::zkvm::LinearCombination(sighash), dinero::zk::zkvm::LinearCombination(sighash), "sighash_bound");

    Variable sum_in = gadgets::constant(cs, Scalar::zero(), "sum_in0");
    for (size_t i = 0; i < b.spends.size(); ++i) {
        const auto& s = b.spends[i]; const std::string p = "spend" + std::to_string(i);
        Variable sk   = cs.alloc(s.secret_key);
        Variable nk   = cs.alloc(s.nullifier_key);
        Variable val  = cs.alloc(s.value);
        Variable rnd  = cs.alloc(s.randomness);
        Variable div  = cs.alloc(s.diversifier);
        Variable idx  = cs.alloc(Scalar::from_u64(s.leaf_index));
        Variable zero = gadgets::constant(cs, Scalar::zero(), p + "_zero");
        Variable pk   = dinero::zk::zkvm::poseidon2_gadget(cs, sk, zero, p + "_pk");
        Variable cm   = NoteCommitmentV2(cs, div, pk, val, rnd, p);
        Variable root = MerklePathV2(cs, cm, idx, s.siblings, p);
        gadgets::assert_equal(cs, root, anchors[i], p + "_anchor");
        Variable nf   = dinero::zk::zkvm::poseidon2_gadget(cs, nk, idx, p + "_nf");
        gadgets::assert_equal(cs, nf, nullifiers[i], p + "_nullifier");
        gadgets::range_check(cs, val, 64, p + "_range");
        sum_in = gadgets::add(cs, sum_in, val, p + "_sum");
    }
    Variable sum_out = fee;
    for (size_t j = 0; j < b.outputs.size(); ++j) {
        const auto& o = b.outputs[j]; const std::string p = "out" + std::to_string(j);
        Variable val = cs.alloc(o.value), pk = cs.alloc(o.public_key), rnd = cs.alloc(o.randomness), div = cs.alloc(o.diversifier);
        Variable cm  = NoteCommitmentV2(cs, div, pk, val, rnd, p);
        gadgets::assert_equal(cs, cm, commitments[j], p + "_cm");
        gadgets::range_check(cs, val, 64, p + "_range");
        sum_out = gadgets::add(cs, sum_out, val, p + "_sum");
    }
    gadgets::range_check(cs, fee, 64, "fee_range");
    gadgets::assert_equal(cs, sum_in, sum_out, "balance");
    return cs;
}
```

`MakeHonestBundle(n_in, n_out, fee)`: build `n_in` notes with deterministic scalars (`Scalar::from_u64(1000+i)` etc.), compute each commitment with `poseidon2_native` using the same formula, append them to a `dinero::consensus::shielded::CommitmentTree`, take `AuthPath(i)` as siblings and `Root()` as the anchor for every spend, set `nullifier = poseidon2_native(nullifier_key, leaf_index)`, split `Σ value_in − fee` across the outputs, and set `sighash = Scalar::from_u64(0x5148)`. If the tree API names differ, read `include/consensus/shielded/commitment_tree.h` and use its append/path/root methods; keep the fixture deterministic.

- [ ] **Step 4: Build and run the self-test; all checks must pass**

```bash
cmake --build build-spike --target spike_shielded_v2_bench -j12 && ./build-spike/spike_shielded_v2_bench --selftest
```
Expected: six `[PASS]` lines and the printed constraint count. If the legacy `ADDR_TAG` or the Poseidon argument order differs from `shielded_circuit.cpp`, fix the copy in the header so the honest fixture's commitment matches `poseidon2_native` (mismatch shows as the honest bundle failing).

- [ ] **Step 5: Neuter check, then commit**

Temporarily delete the `gadgets::assert_equal(cs, sum_in, sum_out, "balance");` line, rebuild, run `--selftest`: "output value +1 breaks balance" and "fee +1 breaks balance" must FAIL. Restore the line, rebuild, all PASS.

```bash
git add tests/spike/bundle_circuit_v2.h tests/spike/shielded_v2_bundle_bench.cpp
git commit -m "spike(shielded-v2): bundle circuit v2 builder with satisfiability self-tests"
```

---

### Task 3: Phase-1 measurement — prove/verify the bundle with Spartan+Hyrax

**Files:**
- Modify: `tests/spike/shielded_v2_bundle_bench.cpp`
- Create: `docs/benchmarks/shielded-v2-spike-YYYYMMDD.json` (output of the run)

**Interfaces:**
- Consumes: `dinero::zk::zkvm::r1cs_spartan_prove(const R1CS&, const std::vector<Scalar>& E, const Scalar& u, const GeneratorSet&, Transcript&, secp256k1_context*, bool bind_public_inputs)` and `r1cs_spartan_verify(const SpartanProof&, const R1CS& verifier_cs, size_t num_constraints, size_t num_variables, const std::vector<uint8_t>& expected_circuit_hash, const Scalar& u, const GeneratorSet&, Transcript&, ...)` (`src/zk/zkvm/r1cs_spartan.h:60-145`), `SpartanProof::serialize(secp256k1_context*)`, `ShieldedGenerators(const R1CS&, secp256k1_context*)` and `ZeroErrorVector(const R1CS&)` (non-static in `src/consensus/shielded/shielded_circuit.cpp:216,223`; declare them `extern` in the bench file inside `namespace dinero::consensus::shielded` if the header does not export them), `Transcript` (same header/namespace `ProveSpend` uses, `shielded_circuit.cpp:542`).
- Produces: JSON rows `bundle_1in2out`, `bundle_2in2out`, `bundle_4in2out` with constraints, variables, proof_bytes, prove_ms, verify_ms.

- [ ] **Step 1: Add the measurement**

```cpp
static BenchRow BenchBundle(size_t n_in, size_t n_out) {
    using namespace dinero::zk::zkvm;
    auto* sctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    BundleV2 b = MakeHonestBundle(n_in, n_out, 1000);
    std::vector<double> prove, verify; size_t bytes = 0, ncons = 0, nvars = 0;
    for (int i = 0; i < 6; ++i) {
        R1CS cs = BuildBundleCircuitV2(b);
        if (!cs.is_satisfied()) { std::fprintf(stderr, "bundle not satisfied\n"); std::exit(2); }
        ncons = cs.num_constraints(); nvars = cs.num_variables();
        const auto& gens = dinero::consensus::shielded::ShieldedGenerators(cs, sctx);
        auto t0 = Clock::now();
        Transcript tp("dinero.shielded.bundle.v2.spike");
        SpartanProof proof = r1cs_spartan_prove(cs, dinero::consensus::shielded::ZeroErrorVector(cs), Scalar::one(), gens, tp, sctx, true);
        auto ser = proof.serialize(sctx);
        double p = ms_since(t0);
        // Verifier rebuilds the circuit structure from PUBLIC inputs only: same builder, witness scalars zeroed.
        BundleV2 pub_only = b; for (auto& s : pub_only.spends) { s.secret_key = s.nullifier_key = s.value = s.randomness = s.diversifier = Scalar::zero(); }
        for (auto& o : pub_only.outputs) { o.value = o.public_key = o.randomness = o.diversifier = Scalar::zero(); }
        R1CS vcs = BuildBundleCircuitV2(pub_only);
        t0 = Clock::now();
        SpartanProof parsed; if (!SpartanProof::deserialize(ser, parsed, sctx)) { std::fprintf(stderr, "deserialize failed\n"); std::exit(2); }
        Transcript tv("dinero.shielded.bundle.v2.spike");
        bool ok = r1cs_spartan_verify(parsed, vcs, ncons, nvars, proof.circuit_hash, Scalar::one(), gens, tv, sctx, true);
        double v = ms_since(t0);
        if (!ok) { std::fprintf(stderr, "bundle verify failed\n"); std::exit(2); }
        if (i) { prove.push_back(p); verify.push_back(v); }
        bytes = ser.size();
    }
    std::sort(prove.begin(), prove.end()); std::sort(verify.begin(), verify.end());
    return BenchRow{"bundle_" + std::to_string(n_in) + "in" + std::to_string(n_out) + "out", ncons, nvars, bytes, prove[2], verify[2]};
}
```

Match the exact `r1cs_spartan_verify` parameter list from `src/zk/zkvm/r1cs_spartan.h:133-145` (the tail parameters after `Transcript&` are `secp256k1_context*` and `bind_public_inputs`; copy them). In `main`, after the baseline row, push `BenchBundle(1,2)`, `BenchBundle(2,2)`, `BenchBundle(4,2)`.

- [ ] **Step 2: Build, run, save**

```bash
cmake --build build-spike --target spike_shielded_v2_bench -j12
./build-spike/spike_shielded_v2_bench | tee docs/benchmarks/shielded-v2-spike-$(date -u +%Y%m%d).json
```
Expected: four rows. Sanity: `bundle_2in2out.constraints` between 17,000 and 40,000; `proof_bytes` far below the baseline row; `verify_ms` well below the baseline's. Record `sysctl -n machdep.cpu.brand_string`, `git rev-parse --short HEAD` and the build type in a `"machine"` field (edit the JSON by hand or extend `PrintJson`).

- [ ] **Step 3: Verify the verifier is not vacuous**

Flip one public input in `pub_only` (e.g. `pub_only.fee += 1`) before verification in a temporary edit; the run must exit 2 with "bundle verify failed". Revert.

- [ ] **Step 4: Commit**

```bash
git add tests/spike/shielded_v2_bundle_bench.cpp docs/benchmarks/shielded-v2-spike-*.json
git commit -m "spike(shielded-v2): phase-1 bundle proof measurements on Spartan+Hyrax"
```

---

### Task 4: Batched verification estimate

**Files:**
- Modify: `tests/spike/shielded_v2_bundle_bench.cpp`

**Interfaces:**
- Consumes: `BenchBundle` internals refactored into `struct ProvenBundle { R1CS verifier_cs; std::vector<uint8_t> proof; size_t ncons, nvars; std::vector<uint8_t> circuit_hash; }` and `ProvenBundle ProveOnce(size_t n_in, size_t n_out, secp256k1_context*)`.
- Produces: JSON rows `verify_50_sequential_ms` and `verify_50_parallel8_ms`.

- [ ] **Step 1: Add the batch measurement**

```cpp
static void BenchBatch(std::vector<BenchRow>& rows) {
    auto* sctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    std::vector<ProvenBundle> proofs; for (int i = 0; i < 50; ++i) proofs.push_back(ProveOnce(2, 2, sctx));
    auto verify_one = [&](const ProvenBundle& pb) { /* deserialize + r1cs_spartan_verify as in BenchBundle; return bool */ };
    auto t0 = Clock::now(); for (auto& pb : proofs) if (!verify_one(pb)) std::exit(2);
    rows.push_back(BenchRow{"verify_50_sequential_ms", 0, 0, 0, 0, ms_since(t0)});
    t0 = Clock::now();
    std::vector<std::future<bool>> fs; std::atomic<size_t> next{0};
    for (int t = 0; t < 8; ++t) fs.push_back(std::async(std::launch::async, [&] { bool ok = true; for (size_t i; (i = next++) < proofs.size();) ok = ok && verify_one(proofs[i]); return ok; }));
    for (auto& f : fs) if (!f.get()) std::exit(2);
    rows.push_back(BenchRow{"verify_50_parallel8_ms", 0, 0, 0, 0, ms_since(t0)});
}
```

Use one `secp256k1_context` per worker thread if the verifier mutates the context (create it inside the lambda per thread); the existing `ShieldedGenerators` cache must be thread-safe or pre-warmed once before the parallel run.

- [ ] **Step 2: Build, run, append rows to the JSON, commit**

```bash
cmake --build build-spike --target spike_shielded_v2_bench -j12 && ./build-spike/spike_shielded_v2_bench | tee docs/benchmarks/shielded-v2-spike-$(date -u +%Y%m%d).json
git add -A tests/spike docs/benchmarks && git commit -m "spike(shielded-v2): batched verification estimate (50 bundles, 8 threads)"
```
Expected: `verify_50_parallel8_ms` about 5–7× lower than sequential on the 12-core M4 Max.

---

### Task 5: Phase-2 candidates — hash-based polynomial commitments (Rust)

**Files:**
- Create: `spike/zk-pcs-eval/Cargo.toml`, `spike/zk-pcs-eval/src/main.rs`, `spike/zk-pcs-eval/README.md`
- Create: `docs/benchmarks/shielded-v2-spike-phase2-YYYYMMDD.md`

**Interfaces:**
- Consumes: nothing from the C++ tree; a shape-equivalent synthetic circuit: for a 2-in-2-out bundle, 2 × (32 Merkle Poseidon-2 hashes + 4 commitment hashes + 1 nullifier hash + 1 pk hash) + 2 × 4 output hashes + 5 × 64-bit range checks ≈ 84 two-to-one hashes + 320 booleanity constraints. Report the candidate's constraint count for exactly that shape.
- Produces: a markdown table per candidate: `crate/version`, `field`, `constraints`, `proof_bytes`, `prove_ms`, `verify_ms`, `pq (yes/no)`, `transparent (yes/no)`, `license`, `last release date`, `build result`.

- [ ] **Step 1: Shortlist and availability check (record, do not guess)**

```bash
mkdir -p ~/src/dinero-v8-shielded-v2/spike/zk-pcs-eval && cd ~/src/dinero-v8-shielded-v2/spike/zk-pcs-eval
for c in spartan2 binius plonky3 whir basefold brakedown ligero; do echo "== $c"; cargo search "$c" --limit 3 2>/dev/null | head -3; done
gh repo view microsoft/Spartan2 --json url,pushedAt,licenseInfo -q '"\(.url) pushed=\(.pushedAt) \(.licenseInfo.name)"' 2>/dev/null
gh repo view IrreducibleOSS/binius --json url,pushedAt,licenseInfo -q '"\(.url) pushed=\(.pushedAt) \(.licenseInfo.name)"' 2>/dev/null
```
Write the results into `README.md` under "Availability (checked YYYY-MM-DD)". Candidates that are not published or not buildable get a row saying so; do not fabricate numbers.

- [ ] **Step 2: Build the harness for the first candidate that builds**

`Cargo.toml` (adjust versions to what Step 1 found):

```toml
[package]
name = "zk-pcs-eval"
version = "0.0.1"
edition = "2021"
publish = false

[dependencies]
# candidate A — Spartan with a hash-based PCS; pin to the exact git rev recorded in README.md
spartan2 = { git = "https://github.com/microsoft/Spartan2", rev = "<rev from step 1>", optional = true }
# candidate B — binius (binary-field, hash-based); pin rev likewise
binius_core = { git = "https://github.com/IrreducibleOSS/binius", rev = "<rev>", optional = true }
rand = "0.8"

[features]
default = []
a = ["spartan2"]
b = ["binius_core"]
```

`src/main.rs` skeleton (fill each candidate's circuit using that crate's own constraint API; keep the shape identical):

```rust
//! THROWAWAY spike (spec §7). Measures shape-equivalent bundle circuits on candidate PCS stacks.
use std::time::Instant;

struct Row { label: &'static str, constraints: usize, proof_bytes: usize, prove_ms: f64, verify_ms: f64 }

fn median(mut v: Vec<f64>) -> f64 { v.sort_by(|a, b| a.partial_cmp(b).unwrap()); v[v.len() / 2] }

#[cfg(feature = "a")]
fn run_candidate_a() -> Row {
    // 1. Build the synthetic bundle circuit: 84 Poseidon-2 two-to-one gadgets chained as in the C++ builder
    //    (32-deep Merkle path x2, note commitment chains, nullifier, pk) + 5 x 64-bit range checks.
    // 2. Setup (transparent: no trusted setup), prove 6 times, verify 6 times, take medians of the last 5.
    // 3. proof_bytes = serialized proof length.
    todo_candidate_a()
}

fn main() {
    let mut rows: Vec<Row> = Vec::new();
    #[cfg(feature = "a")] rows.push(run_candidate_a());
    #[cfg(feature = "b")] rows.push(run_candidate_b());
    println!("| candidate | constraints | proof_bytes | prove_ms | verify_ms |");
    println!("|---|---|---|---|---|");
    for r in rows { println!("| {} | {} | {} | {:.1} | {:.1} |", r.label, r.constraints, r.proof_bytes, r.prove_ms, r.verify_ms); }
}
```

Replace `todo_candidate_a()` with the real implementation against the crate's API before building; a candidate whose API cannot express the shape within the spike budget gets a "not evaluated: <reason>" row.

- [ ] **Step 3: Build and run each candidate in release mode**

```bash
cd ~/src/dinero-v8-shielded-v2/spike/zk-pcs-eval
cargo build --release --features a && cargo run --release --features a
cargo build --release --features b && cargo run --release --features b
```
Expected: one table row per candidate that builds; both rows use the same circuit shape so `verify_ms` and `proof_bytes` are directly comparable to Task 3's `bundle_2in2out`.

- [ ] **Step 4: Write the phase-2 comparison and commit**

`docs/benchmarks/shielded-v2-spike-phase2-YYYYMMDD.md`: the table from Step 3 plus the Task 3 `bundle_2in2out` row as the phase-1 reference, machine, commit, crate revisions.

```bash
git add spike/zk-pcs-eval docs/benchmarks/shielded-v2-spike-phase2-*.md
git commit -m "spike(shielded-v2): phase-2 hash-based PCS candidate measurements"
```

---

### Task 6: Spike report and go/no-go

**Files:**
- Create: `docs/superpowers/spike/2026-09-2X-shielded-v2-spike-report.md`
- Modify: `/Users/haydarevich/src/MemoryMD/dinero-shielded-v2.md` (status section)

**Interfaces:**
- Consumes: the two JSON/markdown outputs from Tasks 3–5.
- Produces: the decision document the phase-1 plan is written from.

- [ ] **Step 1: Write the report with these exact sections**

```markdown
# Shielded v2 spike report — <date>
## Machine and commits
## Phase 1 (Spartan+Hyrax on the bundle circuit)
| | constraints | proof bytes | prove ms | verify ms | target | pass? |   ← rows: baseline, 1in2out, 2in2out, 4in2out, verify_50 sequential/parallel
## Phase 2 candidates
<table from Task 5>
## Findings
- constraint reduction factor vs baseline (per tx)
- which spec §2 gates phase 1 already meets, which it misses and by how much
- candidate choice and why (verify time first, bytes second, maturity third, license)
## Go / no-go
- Phase 1: GO / NO-GO with the one-line reason
- Phase 2: GO / NO-GO / DEFER with the one-line reason
## What the phase-1 plan must contain (inputs for writing-plans)
```

Every number in the report must be copy-pasted from the committed JSON/markdown outputs; no rounding beyond one decimal.

- [ ] **Step 2: Update memory and commit**

Append to `MemoryMD/dinero-shielded-v2.md` under `## Status`: date, the four headline numbers (2-in-2-out constraints, proof bytes, verify ms; phase-2 best verify ms), and the go/no-go lines.

```bash
cd ~/src/dinero-v8-shielded-v2 && git add docs/superpowers/spike && git commit -m "spike(shielded-v2): report and go/no-go"
git -C ~/src/MemoryMD commit -am "dinero-shielded-v2: spike results"
```

---

## Self-review against the spec

- §1/§3.1 statement → Task 2 builder (all seven relations: pk, addr_bind, commitment, Merkle path, nullifier, 64-bit ranges, balance; sighash as first public input).
- §2 targets → Task 3 rows compared to the 16 KB / 10 ms gates in Task 6; §3.4 batching → Task 4.
- §3.3 phase 2 → Task 5; library selection criteria → Task 5 Step 1 and Task 6.
- §7 spike definition → Tasks 1–6; throwaway labelling → file headers and no `add_test`.
- Not in this plan (by design): envelope/activation code (§3.2, §3.5), wallets (§4), consensus vectors (§6) — those belong to the phase-1 plan written from Task 6's report.
- Name consistency: `BuildBundleCircuitV2`, `MakeHonestBundle`, `BundleV2`, `SpendLeg`, `OutputLeg`, `BenchRow`, `PrintJson`, `BenchBundle`, `ProveOnce`, `ProvenBundle` used identically across Tasks 1–4.

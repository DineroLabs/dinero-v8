# Heterogeneous Prover Runtime Plan

## Purpose

This document captures the planned architecture for a heterogeneous prover
runtime in Dinero. The goal is to support:

- CPU-only proving on machines without GPU acceleration
- GPU-accelerated proving where large arithmetic kernels benefit from offload
- CPU+GPU cooperative proving on machines with both useful CPU and GPU capacity
- runtime adaptation to the actual machine profile without changing proof
  semantics

This is a prover-runtime plan, not a protocol redesign. The same proof system
and proof semantics must remain valid regardless of which backend executes the
prover kernels.

## Design Constraints

The following rules are non-negotiable:

1. Verifier, mempool, and consensus remain CPU-safe and deterministic.
2. GPU acceleration is a prover backend, not a protocol dependency.
3. CPU reference behavior remains the authoritative correctness baseline.
4. Structural proof-cost reduction and runtime acceleration are separate work
   streams.
5. Secret-scalar and public-scalar arithmetic paths must be distinguished
   explicitly.
6. Backend-facing memory layout must be treated as a hard interface contract,
   not an implementation detail.
7. Deterministic proving mode must be available for debugging, reproducibility,
   and rollout validation.

## Current Code Reality

The current codebase already has natural arithmetic seams that can serve as the
foundation for a backend architecture.

Primary hot-path entrypoints:

- `Point::multi_scalar_mul(...)` in
  `/Users/haydarevich/src/dinero/src/zk/zkvm/scalar.cpp`
- `pedersen_vector_commit(...)` and IPA logic in
  `/Users/haydarevich/src/dinero/src/zk/zkvm/ipa.cpp`
- `r1cs_ipa_prove(...)` and `r1cs_ipa_verify(...)` in
  `/Users/haydarevich/src/dinero/src/zk/zkvm/r1cs_ipa.cpp`
- hybrid covenant proving and verification in
  `/Users/haydarevich/src/dinero/src/zk/zkvm/ring_covenant.cpp`

The current largest structural proof hotspot is still the in-circuit fixed-base
scalar multiplication for TapTweak:

- `ec_scalar_mul_gen(...)` in
  `/Users/haydarevich/src/dinero/src/zk/zkvm/ec_gadget.cpp`
- hybrid Taproot tweak path in
  `/Users/haydarevich/src/dinero/src/zk/zkvm/hidden_member_binding_circuit.cpp`

That means runtime acceleration and structural optimization must be planned as
separate but complementary efforts.

## Long-Term Architecture

The intended prover runtime has four layers:

1. Machine profile
2. Execution planner
3. Kernel backends
4. Telemetry and feedback

### 1. Machine Profile

At startup, build a machine profile containing:

- CPU thread count
- NUMA layout if available
- system RAM
- GPU count
- GPU model and VRAM
- backend availability such as CUDA, ROCm, and later Metal
- high-level transfer characteristics where detectable

This profile should be immutable during a proof session and should feed backend
selection rather than being consulted ad hoc all over the prover.

In addition to raw machine facts, the planner should consume backend
capabilities rather than just backend names. A capability descriptor should
express what a backend can do and under what limits.

Representative shape:

```text
BackendCapabilities {
  msm: bool
  batched_msm: bool
  fixed_base_mul: bool
  max_points: size_t
  prefers_affine: bool
  prefers_projective: bool
  transfer_bandwidth_estimate: optional<double>
  vram_bytes: optional<uint64_t>
}
```

The planner should reason from capabilities, not from assumptions such as
"CUDA means MSM is always profitable."

### 2. Execution Planner

The planner chooses how to execute expensive prover stages based on:

- machine profile
- proof size
- kernel family
- batch size
- memory pressure

The planner should start as a simple rule-based policy engine, not a fully
general scheduler.

Initial planner output should choose among a small number of modes:

- `cpu_ref`
- `cpu_fast`
- `hybrid_msm`
- later `hybrid_extended`

The planner must also carry a simple explicit cost model. Even an approximate
model is better than intuition-only backend decisions.

Initial target shape:

```text
cost_cpu = cpu_kernel_time(n, layout, threads)
cost_gpu = transfer_time(bytes) + gpu_kernel_time(n, layout, device)

offload if cost_gpu < cost_cpu * margin
```

The first implementation does not need to be globally optimal. It does need to
be explainable, measurable, and overrideable.

### 3. Kernel Backends

Backend interfaces should start narrow. The first backend seam should be MSM.

That seam needs a strict memory layout contract. This is not optional. If the
layout is left implicit, later GPU work will force disruptive rewrites.

Representative design target:

```text
struct MsmBatch {
  point_buffer
  scalar_buffer
  size
  layout_type
  alignment_bytes
  ownership_model
}
```

The exact type names may differ, but the contract must specify:

- point representation
- scalar representation
- alignment
- buffer ownership
- mutability
- transfer expectations
- batch size semantics

This contract should be formalized in a follow-on design note before GPU
implementation begins.

Initial backend targets:

- CPU reference backend
- CPU accelerated backend
- later GPU MSM backend

Potential future backend targets:

- batched fixed-base multiplication
- field-vector kernels used in IPA
- polynomial-style kernels if they emerge in future proof systems

Backend execution must also define failure classes explicitly. "Fallback" is not
enough without knowing why the fallback occurred.

Initial failure taxonomy:

- backend unavailable
- backend initialization failure
- out of device memory
- kernel execution failure
- timeout
- validation mismatch

Each category should:

- emit a structured reason
- trigger a deterministic fallback path
- optionally blacklist the backend for the current session

Validation mismatch is the most severe category and should be treated as a
critical event.

### 4. Telemetry and Feedback

All prover modes should emit comparable telemetry:

- witness build time
- vector build time
- MSM time
- Pedersen commitment time
- opening proof time
- IPA time
- total prove time
- memory usage
- backend selected
- fallback reason

This is required so later planner decisions are based on real measurements, not
intuition.

Later phases should also connect runtime telemetry back to structural proof
optimization work. If telemetry repeatedly shows one kernel family dominates,
that should inform circuit and proof redesign decisions.

## Security Model

The architecture must distinguish between:

- verifier/public arithmetic
- prover-secret arithmetic

This matters because some fast arithmetic paths are variable-time and may be
acceptable for public verifier workloads but not acceptable for prover-secret
scalars without an explicit security decision.

Rule:

- public/verifier MSM may use faster variable-time kernels when justified
- prover-secret MSM must remain on an explicitly reviewed path

This separation must be visible in the API, not hidden behind silent heuristics.

## Determinism And Validation Modes

The prover runtime should support an explicit deterministic mode.

Representative goal:

- same inputs
- same backend selection
- same chunking decisions
- same prover randomness source
- same proof bytes

This requires determinism in both scheduling and prover blinding/randomness.

The runtime should also support a shadow-validation mode during rollout.

Representative behavior:

- accelerated backend computes the result
- CPU reference recomputes, or recomputes sampled jobs
- outputs are compared before accepting the accelerated result

Shadow mode is especially important for early GPU rollout because silent
arithmetic or marshalling bugs are a realistic failure mode.

## Phase Plan

### Phase 0: Architecture Freeze

Deliverable:

- short architecture note approved by the team
- agreement on terminology:
  - prover-only vs consensus-safe
  - public-scalar vs secret-scalar
  - backend-neutral kernel interfaces
  - deterministic vs adaptive proving modes

Acceptance criteria:

- no implementation starts before the CPU/GPU boundary and security model are
  agreed

### Phase 1: MSM Backend Interface

Goal:

- isolate MSM behind a narrow backend interface

Likely code touchpoints:

- `/Users/haydarevich/src/dinero/src/zk/zkvm/scalar.h`
- `/Users/haydarevich/src/dinero/src/zk/zkvm/scalar.cpp`
- experimental CPU MSM files currently in the working tree

Deliverable:

- `MsmBackend` or equivalent narrow interface
- CPU reference implementation
- CPU fast implementation behind the same interface
- explicit memory layout contract for MSM batches

Acceptance criteria:

- exact result parity with the current CPU reference path
- backend can be forced in tests
- no proof semantic change

### Phase 2: Secret/Public Policy Split

Goal:

- ensure public and secret arithmetic paths are chosen intentionally

Deliverable:

- explicit API or call-site policy separating:
  - verifier-safe fast MSM
  - prover-secret conservative MSM
- deterministic mode requirements documented at the API boundary

Acceptance criteria:

- no accidental routing of prover-secret scalars into a variable-time fast path

### Phase 3: Backend-Neutral Telemetry

Goal:

- make prover profiling independent of backend implementation

Likely code touchpoints:

- `/Users/haydarevich/src/dinero/src/zk/zkvm/r1cs_ipa.cpp`
- `/Users/haydarevich/src/dinero/src/zk/zkvm/ring_covenant.cpp`

Deliverable:

- a consistent telemetry layer for hybrid proof runs
- planner-visible metrics for transfer cost and backend decisions

Acceptance criteria:

- same proof run yields comparable timing breakdowns regardless of backend

### Phase 4: Minimal Planner

Goal:

- choose execution mode by simple rules rather than hardcoded local decisions

Initial policy examples:

- no GPU available -> `cpu_ref` or `cpu_fast`
- GPU present but batch too small -> stay on CPU
- GPU present and large MSM batch -> use `hybrid_msm`

Initial planner inputs should include:

- backend capabilities
- batch size
- estimated transfer cost
- deterministic mode on/off
- session-local backend health

Acceptance criteria:

- deterministic, overrideable planner output
- no hidden backend selection

### Phase 5: GPU Pilot for MSM Only

Goal:

- introduce one GPU backend for large MSM jobs

Deliverable:

- GPU MSM backend
- CPU-to-GPU marshalling for contiguous scalar/point arrays
- CPU cross-check mode in debug/test environments
- shadow-validation mode for rollout
- explicit batch sizing rules for VRAM fit and minimum profitable offload size

Acceptance criteria:

- exact parity with CPU reference on test vectors and integration paths
- measurable speedup on large MSM-heavy proof stages
- graceful fallback on unsupported hardware or OOM

### Phase 6: Cooperative Pipelining

Goal:

- overlap CPU witness/vector preparation with GPU arithmetic

Deliverable:

- pipeline CPU preparation of batch N+1 while GPU computes batch N
- adaptive chunk sizing based on observed throughput and memory pressure

Acceptance criteria:

- measurable end-to-end prover improvement, not just isolated kernel wins

### Phase 7: Progressive Rollout

Goal:

- move from experimental acceleration to trusted acceleration in controlled
  stages

Suggested rollout stages:

- stage 0: CPU only
- stage 1: accelerated backend in shadow mode
- stage 2: accelerated backend trusted for selected kernels
- stage 3: pipelined hybrid mode
- stage 4: broader kernel coverage if justified

Acceptance criteria:

- each stage has explicit parity and rollback checks
- no jump directly from experimental backend to trusted default

## Out Of Scope For Early Versions

These should not be part of the first heterogeneous prover implementation:

- GPU transcript logic
- GPU verifier path
- GPU-dependent wallet behavior
- full global task-graph scheduling
- moving all witness generation to GPU immediately
- protocol changes justified only by a desired GPU execution model

## File-Level Implications

Likely initial implementation scope:

- arithmetic interface:
  - `/Users/haydarevich/src/dinero/src/zk/zkvm/scalar.h`
  - `/Users/haydarevich/src/dinero/src/zk/zkvm/scalar.cpp`
- IPA and Pedersen callers:
  - `/Users/haydarevich/src/dinero/src/zk/zkvm/ipa.cpp`
  - `/Users/haydarevich/src/dinero/src/zk/zkvm/r1cs_ipa.cpp`
  - `/Users/haydarevich/src/dinero/src/zk/zkvm/nova.cpp`
  - `/Users/haydarevich/src/dinero/src/zk/zkvm/ring_covenant.cpp`
- backend implementation files:
  - CPU backend
  - future GPU backend

Code that should remain unaffected in architecture terms:

- consensus validation semantics
- mempool verification semantics
- transaction serialization semantics
- wallet/RPC behavior except for optional prover-mode configuration

## Risks

Main technical risks:

- CPU/GPU transfer overhead dominates kernel wins
- backend data marshalling becomes too expensive
- secret/public arithmetic boundaries become blurred
- backend divergence causes subtle non-parity bugs
- planner complexity outpaces measurable benefits
- small proofs regress because offload thresholds are wrong
- large proofs fail because chunk sizing and VRAM budgeting are wrong

Main governance risk:

- treating hardware acceleration as a substitute for structural proof-cost work

It is not. Runtime acceleration helps prover cost. It does not solve verifier
cost or proof-system footprint by itself.

## Success Criteria

The heterogeneous prover plan should be considered successful when:

1. CPU-only proving remains correct and fully supported.
2. Backend selection is explicit, testable, and observable.
3. Large MSM-heavy proofs run materially faster on suitable hardware.
4. Consensus and verifier behavior remain unchanged in semantics.
5. GPU availability is never required for correctness.

## Recommended Immediate Next Step

Do not start with a universal adaptive scheduler.

Start with:

1. backend seam around MSM
2. explicit secret/public arithmetic policy split
3. MSM memory layout contract
4. reusable telemetry
5. simple planner
6. one GPU MSM pilot

That is the smallest implementation path that still matches the long-term
heterogeneous architecture.

## Next Design Artifact

Before implementation, write a dedicated contract document for the first backend
seam:

- `MSM_BACKEND_CONTRACT_V1.md`

That document should define:

- input and output memory layout
- alignment rules
- secret/public separation at the API boundary
- deterministic mode behavior
- validation and shadow-mode hooks
- error and fallback semantics
- batch sizing semantics

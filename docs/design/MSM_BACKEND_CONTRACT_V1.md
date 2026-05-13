# MSM Backend Contract v1

## Purpose

This document defines the first backend contract for heterogeneous prover
execution in Dinero.

It applies to multi-scalar multiplication (MSM) backends used by the prover
runtime. The contract is designed to support:

- CPU reference execution
- CPU accelerated execution
- future GPU execution
- deterministic fallback across all modes

This contract is intentionally narrow. It defines the first accelerator seam and
should be treated as a stable interface before any GPU backend is introduced.

## Scope

This contract covers:

- input and output representation for MSM jobs
- layout and alignment requirements
- secret/public policy boundaries
- determinism requirements
- validation hooks
- failure and fallback semantics
- batch sizing semantics

This contract does not define:

- higher-level proof scheduling
- transcript behavior
- wallet or consensus logic
- non-MSM kernels

## Core Principles

1. The CPU reference path is authoritative.
2. All accelerated backends must be exactly equivalent to the CPU reference
   result.
3. Secret-scalar and public-scalar jobs must be classified explicitly.
4. Backend-visible memory layout is part of the API.
5. Fallback behavior must be deterministic and observable.

## Job Classification

Every MSM job must carry a policy classification.

Minimum required categories:

- `public_verifier`
- `secret_prover`

### `public_verifier`

Used for:

- verifier-side MSMs
- public-scalar operations
- any path where variable-time acceleration is explicitly allowed

### `secret_prover`

Used for:

- prover-secret scalars
- witness-derived commitments
- any path where side-channel posture must be reviewed conservatively

No backend may infer this classification silently. The caller must choose it.

## Data Model

The backend interface should consume a concrete batch object rather than raw
vectors with implicit layout.

Representative target shape:

```text
enum class MsmPointLayout {
  affine_compressed,
  affine_uncompressed,
  projective_packed
};

enum class MsmScalarLayout {
  scalar32_be
};

enum class MsmJobClass {
  public_verifier,
  secret_prover
};

struct MsmBatch {
  point_buffer
  scalar_buffer
  size
  point_layout
  scalar_layout
  alignment_bytes
  job_class
  deterministic
};
```

The exact C++ type names may differ, but the semantics below are mandatory.

## Layout Contract

### Scalars

Version 1 scalar format:

- 32-byte big-endian canonical scalar encoding
- contiguous buffer
- fixed stride

Requirements:

- no implicit re-encoding inside the planner
- any backend-specific conversion must be local to the backend

### Points

Version 1 point layout is fixed:

- `affine_uncompressed`

Requirements:

- contiguous representation
- stable stride
- no hidden pointer chasing
- no backend dependence on container internals such as `std::vector<Point>`
- fixed serialized field order

Version 1 rationale:

- simplest CPU/GPU-neutral contract
- avoids committing to projective backend internals at the boundary
- keeps backend conversion local

Implementation note:

- this boundary format is intentionally clarity-first, not transport-optimal
- `affine_uncompressed` implies 64 bytes per point at the interface boundary
- for large MSM batches this memory and transfer cost is significant
- GPU backends should assume this overhead at the boundary and may convert to a
  denser internal representation after import

Future layouts may be added later, but v1 assumes one canonical external point
layout.

### Alignment

Alignment must be explicit in the batch contract.

Version 1 alignment requirement:

- scalar buffer alignment: 32 bytes minimum
- point buffer alignment: 32 bytes minimum

Backends may request stronger internal alignment after marshalling, but the
planner-facing batch contract for v1 assumes 32-byte minimum alignment.

Rationale:

- CPU SIMD performance
- backend marshalling efficiency
- GPU transfer efficiency

Alignment policy may evolve later, but it cannot remain implicit.

## Ownership And Mutability

The contract must define buffer ownership at the interface boundary.

Version 1 requirements:

- input buffers are read-only from the backend perspective
- output is returned as a newly materialized result object
- backend must not mutate caller-owned buffers

This keeps CPU and GPU backends interchangeable and reduces aliasing risks.

Version 1 ownership rule:

- caller owns batch memory
- backend receives immutable borrowed views
- backend-owned temporary buffers are backend-local only

## Result Contract

Every MSM backend returns:

- either a valid `Point`
- or a structured backend failure

Identity results must be represented explicitly and must not be confused with
backend failure.

## Backend Interface Behavior

Representative logical contract:

```text
MsmResult run_msm(const MsmBatch& batch, const MsmBackendContext& ctx)
```

The backend must:

- validate the batch contract
- either produce an exact result
- or return a structured failure reason

No silent partial success is allowed.

## Determinism

The runtime must support a deterministic mode.

For MSM backends, deterministic mode means:

- same batch bytes
- same backend choice
- same chunking plan
- same validation policy
- same result bytes

MSM itself is deterministic arithmetic, but the planner and batch splitter may
not be. Therefore deterministic mode must constrain:

- backend selection
- chunk splitting
- task ordering
- reduction ordering

Deterministic mode must be callable explicitly by higher-level proving code.

## Validation Hooks

Version 1 must support validation hooks for shadow rollout.

Two validation modes are required:

- `off`
- `shadow`

### `off`

- backend result is accepted directly

### `shadow`

- accelerated backend computes the MSM
- CPU reference path recomputes the same batch
- results are compared before use

Validation mismatch must be treated as a critical backend failure.

Version 1 rule:

- shadow mode means full recomputation, not sampling

Sampling may be considered later, but v1 uses full recomputation because rollout
correctness matters more than speed.

## Failure Taxonomy

Backends must classify failure reasons explicitly.

Minimum categories:

- `backend_unavailable`
- `backend_init_failed`
- `invalid_batch`
- `out_of_memory`
- `kernel_failure`
- `timeout`
- `validation_mismatch`

Version 1 requirements for any failure:

- return structured reason
- emit telemetry
- trigger deterministic fallback to CPU reference where safe

Special handling:

- `validation_mismatch` should mark the backend unhealthy for the session
- repeated `kernel_failure` or `out_of_memory` may also blacklist the backend
  for the remainder of the session

## Fallback Semantics

Fallback must be explicit and reproducible.

Required properties:

- fallback target is the CPU reference backend
- fallback reason is recorded
- fallback does not alter proof semantics
- deterministic mode always picks the same fallback path for the same failure

No silent "best effort" behavior is allowed.

## Batch Sizing Contract

MSM backends are sensitive to batch size. The contract must make batch size a
first-class planner input.

Version 1 planner policy must define:

- minimum profitable offload threshold
- maximum points per batch for a backend
- chunking rules when a job exceeds backend capacity
- reduction ordering when chunked

These decisions must be deterministic under deterministic mode.

Version 1 policy defaults:

- no GPU offload below a backend-declared minimum batch threshold
- if `size > max_points`, planner splits into deterministic contiguous chunks
- reduction order is left-to-right by chunk index

Any future adaptive scheme must preserve deterministic mode behavior.

## Capability Descriptor

Planner decisions must use backend capabilities, not backend names.

Minimum capability shape:

```text
struct MsmBackendCapabilities {
  available
  supports_public_verifier
  supports_secret_prover
  max_points
  preferred_point_layout
  preferred_alignment
  transfer_bandwidth_hint
}
```

This descriptor lets the planner choose backends by actual constraints rather
than fixed assumptions.

Version 1 note:

- `transfer_bandwidth_hint` is a hint, not a guarantee
- planner must treat it as advisory because real transfer behavior varies with
  machine load and memory topology

## Security Boundary

The backend interface must preserve the public/secret policy split.

Rules:

- `secret_prover` jobs may only be routed to backends approved for that class
- `public_verifier` jobs may use faster variable-time backends if policy allows
- the planner must not silently upgrade a secret job into a public policy

This boundary should be enforced by type or explicit enum, not convention.

Version 1 policy choice:

- `secret_prover` jobs must not use variable-time acceleration
- `public_verifier` jobs may use variable-time acceleration if explicitly
  approved by backend policy

## Testing Requirements

Before any accelerated backend is considered usable:

1. parity tests against CPU reference on fixed vectors
2. parity tests on randomly generated batches
3. threshold-crossing tests around planner boundaries
4. deterministic-mode tests
5. shadow-mode tests
6. fallback-path tests for each failure category that can be simulated

## Rollout Policy

Backends should be introduced in stages:

- stage 0: CPU reference only
- stage 1: accelerated backend behind explicit opt-in
- stage 2: shadow-mode validation
- stage 3: trusted mode for selected job classes

No accelerated backend should become default before shadow-mode parity is well
understood.

## Initial Implementation Guidance

Version 1 should not attempt to solve all future backend needs.

The first implementation should:

- lock the batch contract
- separate secret/public job classes
- define deterministic and shadow behavior
- classify failures
- support one CPU reference backend and one CPU accelerated backend

Only after that should a GPU backend be introduced.

## Version 1 Locked Decisions

The following decisions are locked for v1:

- canonical point layout: `affine_uncompressed`
- scalar layout: 32-byte big-endian canonical scalar encoding
- minimum batch alignment: 32 bytes for points and scalars
- shadow mode: full recomputation, no sampling
- `secret_prover` jobs: no variable-time acceleration
- chunk reduction order: deterministic left-to-right

Chunk-level telemetry is recommended but not required for the first landed
implementation.

## Immediate Next Step

Use this contract to drive a small refactor of the current MSM entrypoint in:

- `/Users/haydarevich/src/dinero/src/zk/zkvm/scalar.cpp`

But do not introduce GPU-specific code until:

- the contract is accepted
- the CPU reference backend is formalized behind the contract
- the CPU accelerated backend is implemented against the same contract

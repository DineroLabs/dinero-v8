# Ring-Covenant Hidden-Member Binding Design

## Status

Current v4 verifier plumbing is intentionally incomplete.

What is already true:
- the CLSAG message commits to the canonical BIP341 TapLeaf hash
- the ZK proof commits to that same TapLeaf hash
- the public control block verifies that the TapLeaf opens into some Taproot
  output key present in the public ring

What is **not** yet true:
- the proof does not establish that the Taproot script path belongs to the same
  hidden ring member selected by the CLSAG witness

This is why `sendprivatecovenant` remains disabled.

## Why the current public control-block path is not enough

The current verifier in:
- `/Users/haydarevich/src/dinero/src/zk/zkvm/ring_covenant.cpp`

checks the public control block against each public ring member key until one
matches. That is acceptable as verifier groundwork, but it is not the final
anonymity-preserving construction:

1. it only proves membership in the public ring, not equality to the hidden
   CLSAG real member
2. if wallet construction were enabled against this path, public control-block
   matching could reveal which ring member's Taproot key corresponds to the
   hidden script path

The final design therefore needs to move the control block / Taproot path
binding *inside* the proof witness.

## Why public output-key equality is also not enough

It is tempting to treat the derived Taproot output key as a public join object:

- ring proof says "some hidden member in the public ring owns the key image"
- Taproot-path proof says "some hidden script path derives to output key `P_j`"
- verifier checks the two public `P_j` values are equal

That is **not** anonymity-preserving here because the ring members are already
public. If a proof publishes the derived output key as a public join value,
observers can compare it against the public ring and recover the selected
member index directly.

So the final same-member join must stay hidden inside the proof relation
itself. The protocol must not expose the selected output key as a standalone
public equality target.

## Refined architecture

The right split is:

- keep Bitcoin/Taproot semantics public where they do **not** identify the
  hidden ring member
- keep anonymous joins algebraic wherever they can be expressed as
  Schnorr/DLEQ-style relations
- isolate the unavoidable TapTweak hash relation to one minimal private
  subproof

In other words:

`public = non-identifying Bitcoin semantics`

`private = hidden-member joins + the irreducible hidden TapTweak hash`

This is stricter than either extreme:

- it rejects the old public control-block match, because that can reveal the
  selected member
- it also rejects pushing the entire ring/key-image/Taproot relation into one
  generic R1CS proof, because the variable-base EC cost explodes

## Target statement

For a v4 spend, the prover should establish:

1. there exists a hidden ring index `j`
2. there exists a hidden private key `x`
3. the selected public ring member `P_j` satisfies `P_j = x * G`
4. the public key image `KI` satisfies `KI = x * H_p(P_j)`
5. the hidden script and hidden control-block path commit to the same public
   Taproot output key `P_j`
6. the hidden script executes successfully under the existing Tapscript ZK VM
7. the transaction template hash used inside the proof matches the actual
   transaction outputs

That gives the binding we actually want:

`same CLSAG real member == same Taproot output key == same hidden script path`

without revealing which ring member matched.

## Minimal private boundary

The irreducible private relation is:

1. hidden internal x-only key `U_j`
2. public TapLeaf hash `L`
3. hidden tweak scalar `t = TapTweak(U_j || L)`
4. public ring output key `Q_j = U_j + t*G`

That is the one place where a hash-bearing private subproof is unavoidable.

Everything else should stay algebraic:

- ownership: `Q_j = x * G`
- key image: `KI = x * H_p(Q_j)`
- same-member binding: both subproofs must use the same hidden member / hidden
  selector

## Rejected intermediate

The fully unified anonymous single-leaf R1CS proof was a useful correctness
baseline, but profiling showed it is not the right end state:

- about `3.5M` constraints
- about `3.46M` variables
- dominated by variable-base EC work inside the circuit, especially the
  key-image relation

That prototype remains valuable as:

- a native/circuit oracle for the exact same-member acceptance condition
- a regression target for the `A != B` split-member failure mode

But it is **not** the intended production proof shape.

## Proof shape

The intended first secure-v4 construction is a hybrid:

1. Compact ring proof, outside R1CS
   - proves ownership / key-image relation for some hidden ring member
   - binds the proof transcript to the public ring, key image, TapLeaf hash,
     and the same hidden selector/index commitment consumed by the Taproot side

2. Minimal private Taproot subproof, inside R1CS
   - single-leaf only
   - hidden one-hot selector chooses one public ring output key `Q_j`
   - hidden internal x-only key derives that same `Q_j` through one private
     TapTweak evaluation
   - same hidden selector opens a public index commitment `C_idx = j*H + r*G`

3. Hidden join
   - the compact ring proof and the Taproot subproof must bind to the same
     hidden selector / same `C_idx`
   - no public output-key equality check is allowed

This keeps the only SHA-bearing work at the irreducible TapTweak boundary and
keeps the ring/key-image logic in native algebraic proofs.

## Available primitives

Already available:
- compact ring proof groundwork in
  `/Users/haydarevich/src/dinero/src/zk/zkvm/ring_covenant.cpp`
- one-hot selector and point selection helpers in
  `/Users/haydarevich/src/dinero/src/zk/zkvm/hidden_member_binding_circuit.cpp`
- fixed-base scalar multiplication gadgets in
  `/Users/haydarevich/src/dinero/src/zk/zkvm/ec_gadget.h`
- single-leaf TapTweak gadget path in
  `/Users/haydarevich/src/dinero/src/zk/zkvm/taproot_path_circuit.cpp`

Important observation:
- a new in-circuit hash-to-point gadget is not required for this first secure
  mode
- the ring is public, so `H_p(Q_i)` can be precomputed outside the circuit and
  supplied as public constants when needed

## Taproot path strategy

First secure mode is intentionally narrow:

- single-leaf only
- hidden internal x-only key
- public TapLeaf hash remains bound in the CLSAG message
- no public control block on the wallet-enabled path

Multi-leaf TapBranch folding stays out of scope until the same private
single-boundary story exists for branch hashing.

## Proof-shape options

Rejected:
- extend `TapscriptZKProof` with ring-member binding
- keep the fully unified anonymous single-leaf R1CS proof as the final design

Chosen direction:
- keep `TapscriptZKProof` focused on script execution
- keep the compact ring proof algebraic
- add a dedicated minimal Taproot-binding subproof
- join them on the same hidden selector/index commitment

## Wire / serialization implications

Today v4 inputs carry:
- `tapleaf_hash`
- `taproot_control_block`
- `tapscript_zk_proof`

Final target:
- `tapleaf_hash` may remain public if it stays part of the CLSAG binding message
- `taproot_control_block` should become deprecated for wallet-created spends and
  eventually removed from the consensus-critical path
- the hidden-member binding proof should evolve toward:
  - compact ring proof bound to the selector/index commitment
  - minimal private TapTweak subproof opening that same selector/index
    commitment
- the private Taproot witness lives inside the proof blob, not on the wire as a
  public control block

Touched files when this begins:
- `/Users/haydarevich/src/dinero/include/primitives/transaction.h`
- `/Users/haydarevich/src/dinero/src/primitives/transaction.cpp`
- `/Users/haydarevich/src/dinero/src/primitives/transaction_serializer.cpp`
- `/Users/haydarevich/src/dinero/src/zk/zkvm/ring_covenant.h`
- `/Users/haydarevich/src/dinero/src/zk/zkvm/ring_covenant.cpp`

## Concrete implementation phases

### Phase 1: proof interface
- introduce a dedicated hidden-member binding proof type
- thread public ring member keys and key image into the verifier API
- keep wallet construction disabled

### Phase 2: algebraic hidden-member proof
- keep the compact ring proof for:
  - hidden member ownership
  - key image relation
- bind that proof transcript to the same hidden selector/index commitment

### Phase 3: minimal private Taproot subproof
- keep single-leaf only
- use one hidden selector to choose the public ring output key
- privately prove `TapTweak(hidden_internal_xonly || tapleaf_hash)` derives that
  same selected output key
- prove the same selector opens the shared public index commitment
- do not expose a public selected output key as a join object

### Phase 4: wire cleanup
- deprecate the public `taproot_control_block` field for wallet-created spends
- keep legacy verifier compatibility only as long as needed for harnesses/tests

### Phase 5: wallet enablement
- only re-enable `sendprivatecovenant` after Phases 2 and 3 are both complete
- add live/regtest coverage proving v4 spends stay anonymous and valid across:
  - mempool admission
  - restart persistence
  - mined block connection

## Testing requirements

Before wallet re-enable:

1. proof rejects if the selected ring member key does not match `x * G`
2. proof rejects if the selected `H_p(P_j)` does not match the public key image
3. proof rejects if the hidden Taproot path derives to a different member than
   the one bound by the hidden ring proof
4. the adversarial split case fails:
   - ring proof for member `A`
   - Taproot path for member `B`
   - verifier rejects
5. verifier cannot infer which ring member matched from public tx data alone
6. mempool and block validation consume exactly the same public inputs

## Current policy

Until the hidden-member proof exists:
- `sendprivatecovenant` stays disabled
- public control-block matching remains verifier/test groundwork only
- the old fully unified anonymous R1CS proof remains a local correctness oracle,
  not the target production proof shape
- v4 ring-covenant spends remain experimental and not ready for wallet use

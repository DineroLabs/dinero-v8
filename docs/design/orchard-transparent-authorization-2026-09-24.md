# Draft Orchard transparent authorization

This staged component implements transparent signatures and spendability over
`OrchardCoinSnapshot`. It is not connected to mempool/block admission and does
not activate Orchard. The new rules need protocol review and integration tests
before the format is frozen. Historical transaction authorization is unchanged.

## Supported inputs

Profile 1 supports two exact native programs:

| Previous output | Required witness |
| --- | --- |
| `OP_1 PUSH32 <x-only output key>` | One 64-byte BIP340 signature |
| `OP_0 PUSH20 <HASH160(compressed public key)>` | Strict DER, low-S ECDSA signature followed by byte `01`; one 33-byte compressed public key |

Every scriptSig is empty. Extra items, missing items, annexes, Taproot script
paths, alternate sighash types, uncompressed keys, nested witness, P2WSH,
legacy scripts and unknown witness programs reject. This is deliberately a
restricted new-transaction profile, not a generic script interpreter. Wallet
coin selection must enforce this support set; other existing outputs can still
be spent using their historical transaction rules. No existing script gains a
new anyone-can-spend interpretation.

## Exact signing message

Let `D` be the existing candidate Orchard intent digest, derived from the owned
envelope and resolved previous outputs. `D` includes network, genesis, branch,
transaction version, locktime, ordered outpoints/sequences/amounts/scripts,
outputs, mandatory fee, protocol profiles, action count and Orchard effects.

For input index `i`, define:

```
tag = SHA256(ASCII("DIN/orchard-v2/transparent-sighash/v1"))
T_i = SHA256(tag || tag || D || 01 || program || u32le(i))
program = 00 for P2WPKH, 01 for key-path Taproot
```

Sign/verify `T_i` with the corresponding existing secp256k1 algorithm. This is
a Dinero-specific full-intent message, **not** the historical BIP143 or BIP341
transaction sighash. There is no NONE, SINGLE or ANYONECANPAY mode. The fixed
P2WPKH marker `01` selects this full-intent rule; all other markers reject.
Changing the profile requires reviewed signing vectors and an explicit future
protocol change. No public verifier takes a raw digest. The wallet-facing
digest builder takes the same owned snapshot and validates the selected program.

The intent is computed once per verification pass. Each input additionally
commits its index and program. Both signature schemes bind the complete Orchard
effects, including encrypted output payloads. Witness bytes themselves are
excluded from the intent; their exact structure and authorization are checked.
Proof and Orchard signatures are also excluded from `D`, but remain in txid as
specified by the envelope. Final transaction identity still requires finalized
Orchard authorizations, not merely these transparent signatures.

## Spendability and state contract

- Candidate height must be exactly snapshot view height plus one, without
  integer wraparound. Same-block/mempool parents may have candidate height.
- Reject future creation heights. Coinbase inputs require 100 blocks, matching
  current full-block and authenticated stateless validation. Do not substitute
  the looser regtest chainparams convenience value.
- Enforce absolute locktime with the existing height/time threshold and
  all-final exception, and version-2-style relative height/time locks. Time
  locks use branch median-time-past, not wall time. Missing time data rejects
  distinctly from a valid context whose lock has not expired. Arithmetic uses
  widened values and checked time addition.
- Median-time lookups are cached by height within one call. The host supplies
  the authenticated branch and holds its chainstate lock through resolution,
  validation and application. These interfaces do not enforce lock ownership,
  detect same-height reorgs, or authenticate a caller-created view by themselves.

The result is named `VerifiedOrchardTransparentInputs`. It owns the exact
snapshot, intent and candidate height; it is neither an Orchard proof result
nor complete transaction validity. Before admission, the host must also verify
Orchard authorization, anchors, nullifiers, pool conservation, activation and
resource limits against the same locked state, then apply atomic state/undo.
Missing state/MTP is not evidence of a consensus-invalid transaction. Results
cannot be reused across a change to the relevant coin/branch state.

## Test scope

`OrchardTransparent` is mandatory in both standalone and root CI inventories.
Tests use public synthetic keys, actual pinned secp256k1 signing/verifying, and
an explicit deterministic coin view. Python independently constructs `D` and
both transparent signing messages; its Orchard effect input is an opaque saved
fixture, so this does not close independent Orchard-effect computation review.

The mixed Taproot/P2WPKH case verifies. Rejection cases cover altered signatures,
noncanonical/high-S ECDSA, key mismatch, extra or absent witness, unsupported
programs, cross-network/genesis/branch messages, changed fee/output/sequence/
locktime, redistributed input amounts, modified synthetic ciphertext and
same-key input-signature swaps. Maturity, lock boundaries, missing MTP and
overflow are covered; 40 lock cases agree with the existing contextual-lock
implementation. A temporary historical shell is used only as that lock-rule
oracle, never for signing, serialization or admission.

The saved Orchard bundle was originally signed for different synthetic
transparent scripts. The transparent tests deliberately require its Orchard
authorization to **fail** under their new keys, demonstrating that success in
this component is not misreported as a fully authorized transaction. A new
combined valid bundle/transparent fixture is still required when the proving
and wallet construction path is wired.

No daemon, wallet, fleet or activation change is made by this component.

## Local qualification receipt

macOS Release passes all five standalone and six selected root CTest entries,
including the eleven Rust cases. The exact required inventories are checked
for enabled tests and labels. Both independent Python vector generators pass
check mode. ASan/UBSan passes for the transparent verifier, coin adapter and
C++ Orchard wrapper/signing/envelope sources; the pinned Rust, secp256k1 and
historical-codec archives are not instrumented in that run. macOS leak detection
is disabled. These are component results; fresh Linux CI and full-daemon,
combined-authorized transaction and lifecycle qualification remain required.

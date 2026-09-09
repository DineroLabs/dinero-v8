# Dormant recipient-authority resource profile

Status: implemented for the recipient-authority upgrade; shipped activation
heights remain `UINT32_MAX`. No production activation is selected here.

## Scope and activation

The profile uses the existing spend-authority activation height, including its
explicit dormant sentinel. Mempool admission targets tip + 1; mining checks the
actual candidate height; block connection and reindex use the block's height.
Historical blocks keep their previous consensus behavior. A proof's version
byte or a transaction's version never substitutes for the contextual height.

| Resource | Recipient-authority profile |
| --- | ---: |
| Shielded transaction serialized bytes, including witness | 512,000 |
| Shielded transaction weight (`3 * base bytes + total bytes`) | 2,048,000 |
| Shielded spends per transaction | 4 |
| Shielded outputs per transaction | 2 |
| Spend plus output proofs per block | 8 |
| Sum of shielded transaction bytes per block | 1,000,000 |
| Ancestor or descendant package containing shielded transactions | 600,000 bytes |
| Existing package-count policy | 25 ancestors / 25 descendants under the historical counting semantics |

The enlarged transaction profile applies only to version 6 carrying a shielded
bundle. Version 6 commits the bundle into the transaction identifier; version 5
does not, remains under the historical wire envelope before activation, and is
rejected once recipient-authority enforcement is active. The complete bundle
and proof rules remain mandatory. Transparent transactions
retain their existing 100,000-byte/400,000-weight admission limits. Packages
without shielded members retain the 101 KiB policy. Package byte accounting
includes the candidate and all unique ancestors, and the complete descendant
set considered for each direct parent. The existing 25-ancestor/descendant
counting semantics are not changed by this profile.

The four-input/two-output workload supports funding from several notes, one
recipient, and change. Larger batches require multiple confirmed transactions.
Wallet selection uses the largest available Auth notes first so a valid subset
within the input bound is found whenever one exists. A fragmented balance that
requires more inputs produces an explicit input-limit error before proving.
The pure builders also check counts before proving and exact serialized size
and weight before returning a transaction for persistence/submission.

## Rationale and measurements

`ShieldedAuthResourceMeasurements` constructs real Auth transactions with
outgoing envelopes and verifies their complete bundles. It prints total size,
weight, each proof category, encrypted-note bytes, remaining envelope bytes,
construction/verification wall time, and process peak resident memory.

The ordinary one-input/two-output transfer measures 167,935 bytes: 73,281 spend
proof bytes, 86,370 output-proof bytes, 1,514 encrypted-note bytes, 6,390 range
container bytes, and 380 other bytes. Proofs dominate. Increasing only the old
101 KiB ancestor limit cannot make this transaction relayable.

The 512,000-byte cap is sized for the explicitly tested four-input/two-output
workload, not just the first failing transfer. The eight-proof block bound
accommodates the six-proof maximum transaction with limited additional packing
and independently limits expensive verification, including if proof encodings
shrink later. The block shielded-byte bound is explicit consensus enforcement
for the new profile: the existing miner's 1 MB template preference must not be
mistaken for the block wire decoder's 4 MB ceiling. No historical global block
rule is silently replaced by this profile.

Fees continue to use actual virtual size; shielded bundles are base bytes and
receive no witness discount. This change does not wire the stale, unused
shielded VWU constants into consensus or fee accounting. Debug wall times and
process high-water memory are local measurements, not portable production
performance promises. Production activation still requires release-build and
target-hardware capacity review.

## Enforcement paths

`consensus/shielded/resource_limits.h` is the shared selector and checker.

- RPC submission and dry-run use ordinary contextual mempool admission.
- Mempool entry, package, mining, RPC size reporting, and orphan accounting use
  serialized bytes directly; `Serialize()` is a byte vector, not hexadecimal.
- Stateless P2P download/structural checks permit only a bounded union of the
  historical and dormant profiles. Only a raw version-6 claim permits bounded
  parsing under the larger envelope; a
  canonical bundle plus contextual admission/connection still decides validity.
  Historical block parsing retains its weight-based rule; the standalone
  relay byte ceiling is not retroactively imposed on old v5 witness bundles.
  Valid shielded-only input/output sides are allowed through structural checks.
- Orphan holding uses the same structural envelope bounds while retaining a
  10,000,000-byte aggregate budget and existing count/per-peer caps. Holding
  an orphan is not acceptance into the mempool or chain.
- Mining checks target-height transaction rules, actual package weight, and
  aggregate block proof/byte limits before adding a package.
- Live block connection, pure template validation, stored-block shielded replay,
  and disk reindex share the contextual block checker before state mutation.
  Reindex applies these checks even when script/PoW assume-valid skips apply.

## Verification contract

Boundary tests cover transaction bytes, bundle counts, block proof count,
block shielded bytes, preactivation/activation/reorg selection, dormant sentinel,
transparent-profile isolation, malformed/trailing wire payloads, download
ceilings, and aggregate orphan memory. Mempool tests exercise real admission at
changing tips and ensure resource success does not bypass missing-state checks.
Block-validator tests reject oversized Auth blocks before UTXO mutation.

`ShieldedAuthRelayLifecycle` requires two independently initialized daemons:
source submission, peer mempool admission, peer mining, and source acceptance
of the peer's block. It then checks provisional outgoing recovery, encryption
and locked recognition, two process restarts, invalidate/reconsider recovery,
and an unlocked recipient-authorized spend relayed to and mined by the peer.
The direct addressed-shield lifecycle remains separate coverage.

For per-shape process memory rather than a cumulative process high-water mark,
run each shape in a separate process:

```sh
for shape in shield transfer_1in_2out transfer_2in_2out transfer_4in_2out unshield; do
  AUTH_RESOURCE_SHAPE="$shape" build/test_shielded_validation \
    --gtest_filter='*AuthResourceMeasurements'
done
```

On macOS/Linux the output reports peak RSS in bytes. The Windows fallback
reports zero (unavailable); it must not be interpreted as zero memory usage.

## Local Release results

Apple M4 Max, 128 GiB RAM, Apple clang 17, Release build. Each shape ran in a
fresh process with real proofs. Full machine-readable decomposition and source
fingerprints are in [measurement evidence](../audits/SHIELDED_AUTH_RESOURCE_MEASUREMENTS.json).

| Shape | Bytes | Weight | Build seconds | Verify seconds | Peak RSS MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| shield | 46,373 | 185,483 | 3.569 | 1.086 | 543.8 |
| transfer_1in_2out | 167,935 | 671,734 | 11.881 | 4.012 | 764.7 |
| transfer_2in_2out | 243,661 | 974,638 | 17.128 | 6.014 | 764.8 |
| transfer_4in_2out | 394,953 | 1,579,806 | 27.138 | 9.858 | 765.0 |
| unshield | 75,714 | 302,850 | 5.245 | 1.888 | 764.8 |

The maximum tested shape leaves 117,047 bytes beneath the transaction cap.
Eight proofs per block permit the six-proof maximum transaction plus limited
additional work. From these observations, eight single-spend verifications
would be about 15.1 seconds on this machine, before other block-processing
work; this is a sizing estimate, not a worst-case bound. The measured complete
four-input/two-output verification is 9.858 seconds. The explicit count/byte
bounds remain necessary even if future implementations verify faster.

The prover's roughly 765 MiB process peak is material for mobile deployment.
This result does not establish a supported mobile memory budget. Activation
remains dormant pending the release-candidate and target-device review.

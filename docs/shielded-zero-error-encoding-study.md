# Lossless encoding study for ordinary Spartan proofs

Status: local experiment, 2026-09-15. No daemon, wallet, consensus, network format,
activation, fee policy or Utreexo implementation changes. The completed one-pass
unshield optimization remains separate in draft PR #748.

## Result

Ordinary Auth spend and cv output proofs serialize a large array of identity
points in `comm_E`. Their existing verifier requires these points to be the
identity. The experiment omits those point bytes, retains their dimensions and
every other proof field, and reconstructs the exact original bytes before use.

| Proof | Original bytes | Experimental bytes | Saved bytes | Reduction |
| --- | ---: | ---: | ---: | ---: |
| Auth spend (`0x06`) | 73,281 | 39,592 | 33,689 | 45.97% |
| Cv output (`0x04`) | 43,185 | 24,610 | 18,575 | 43.01% |

The experimental representation includes a four-byte `DZE1` tag per proof.
This tag is only a file-format marker for the study; it is not an assigned
consensus proof version. Old nodes do not accept these experimental files as
shielded proofs.

| Fully validated fixture | Original transaction bytes | Proof payload saving | Share of original transaction |
| --- | ---: | ---: | ---: |
| Unshield, one spend | 75,714 | 33,689 | 44.50% |
| Shield, one output | 46,373 | 18,575 | 40.06% |
| Transfer, one spend and two outputs | 167,935 | 70,839 | 42.18% |

The last two columns are derived from the measured proof encodings, with the
original transaction envelope held fixed. They are not measurements of new
accepted transactions or fees. A final format would also change length prefixes
and require finalized transaction identity and signature handling. The study
does not reduce proving or verification CPU by these percentages.

## Why reconstruction is possible

- `src/consensus/shielded/shielded_circuit.cpp` constructs ordinary proofs with
  `ZeroErrorVector(cs)` and relaxation scalar one.
- `src/zk/zkvm/r1cs_spartan.cpp` commits that vector as `comm_E`, includes its
  points in the transcript and serializes the commitment alongside `comm_W`.
- `src/zk/zkvm/hyrax.cpp` stores three eight-byte dimensions followed by one
  33-byte point per row. The identity point serializes as 33 zero bytes.
- `r1cs_spartan_verify` defaults to `require_zero_error=true` and rejects any
  nonidentity error-commitment row. This is a soundness condition: removing it
  can allow proofs of invalid witnesses. Keep it enabled.

The measured spend has 1,021 error rows (33,693 point bytes); the output has 563
(18,579 bytes). The witness commitments, error evaluation proof, scalar claims,
sumchecks and circuit hash all remain present. In particular, this experiment
does not remove the verifier's error check or the error commitment from the
transcript. It reconstructs the exact original transcript input.

Genuine relaxed/folding proofs may have nonzero error commitments. This study
does not support them, private covenant version `0x07`, or other proof versions.

## Evidence and limits

The opt-in `AuthResourceMeasurements` dump occurs only after the existing full
bundle validator and transaction resource checks succeed. Fresh shield,
unshield and one-input/two-output transfer fixtures passed those checks.
All five extracted proofs round-tripped byte for byte. The structured evidence
in [the measurement file](benchmarks/shielded-zero-error-20260915.json) records
field sizes, dimensions and original/restored SHA-256 hashes.

The codec was developed test-first: four initial tests failed against the
identity/no-op implementation; the implemented codec passed. The final six test
cases cover exact round-trip and size, rejecting nonzero error commitments,
malformed/truncated/trailing data, bounded expansion dimensions, unsupported
versions, and preservation of every retained field. They pass on both synthetic
structural fixtures and the five real proof dumps. Synthetic fixtures are not
cryptographically valid proofs. The codec is not a cryptographic verifier.

The existing `SpartanSoundness` suite also passed its three cases: an honest
proof verifies; an invalid witness with a forged nonzero error term is rejected;
the negative control demonstrates acceptance when the guard is deliberately
disabled. These results support the unchanged guard, not approval of a new
production decoder. General cryptographic review, fuzzing and network/chain
qualification of a compact format remain outstanding.

## Reproduce

Build the existing `test_shielded_validation` and `test_spartan_soundness` targets
in a normal configured headless Release build. The only C++ change in this study
is an opt-in fixture exporter in the test binary; it writes local test data to
an existing directory and has no daemon integration.

```sh
mkdir -p build/proof-size-study
for shape in shield unshield transfer_1in_2out; do
  AUTH_RESOURCE_SHAPE="$shape" AUTH_RESOURCE_DUMP_DIR="$PWD/build/proof-size-study" \
    build/test_shielded_validation --gtest_filter='*AuthResourceMeasurements'
done
python3 contrib/benchmarks/zero_error_encoding_study.py
python3 contrib/benchmarks/zero_error_encoding_study.py \
  --fixtures build/proof-size-study --report
ctest --test-dir build -R '^SpartanSoundness$' --output-on-failure
```

Proof randomness can change hashes on a fresh run; recorded byte counts are
specific to these circuit versions and transaction shapes. The Python codec's
dimension/length limits are local experiment bounds, not proposed consensus
parameters. Raw dumps, the initial red log and final green logs remain local
build evidence; they are not mainnet transaction data.

## Required next gates

1. Specify a separately activated canonical encoding. Preserve historical
   decoding and validation rules, reject alternate encodings, and derive/check
   dimensions against the selected circuit before allocating restored rows.
   Do not reinterpret existing version bytes or select an activation height in
   this study.
2. Restore all omitted identity points before the unchanged verifier and
   transcript run. Keep `require_zero_error=true`. Test valid and forged proofs,
   wrong circuits, altered retained fields, dimension attacks and parser bounds.
3. Keep verification-work limits independently of compressed byte counts.
   Current Auth limits include four spends/two outputs per transaction and eight
   proofs per block. Fewer wire bytes do not make those proofs cheaper to verify.
4. Preserve the finalized-transaction/Utreexo contract. Auth v6 commits bundle
   bytes through txid. Final canonical compact transaction bytes must determine
   its txid and transparent outpoints; proof expansion belongs inside proof
   verification, never before transaction identity hashing. Rebuild fees and
   binding signatures only against the finalized transaction rules.
5. Require activation-boundary and mixed-version tests, full-node/CSN equality,
   pool inclusion and commitments, restart/reindex, reorg restoration, and a
   transparent child spending the exact unshield output. The output must prove
   while unspent and cease to prove against the current accumulator after spend.
   Historical proofs against historical roots remain a separate capability.

Recommendation: qualify this lossless representation before undertaking a
proof-system replacement. Its measured size benefit is large, but no production
compatibility claim or rollout approval follows from an offline round-trip.

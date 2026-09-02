# Covenant validation resource limits

Status: implemented and bounded for mainnet activation at block 100,000.
Open-source assurance and release-candidate gates remain mandatory before
fleet deployment.

This note records the deterministic validation limits and the benchmark
evidence for the CTV/CCV covenant profile. Timing measurements are diagnostic,
not consensus rules. The local readiness runner nevertheless applies generous
release-policy ceilings so a major performance regression cannot ship merely
because consensus results remain correct.

## Consensus limits

The limits below apply to the opcodes that are eligible for the first covenant
profile:

- A revealed tapscript may execute at most one
  `OP_CHECKTEMPLATEVERIFY`.
- A revealed tapscript may execute at most one
  `OP_CHECKCONTRACTVERIFY`.
- The existing 100,000-byte transaction limit bounds the transaction-wide
  data hashed or scanned by either opcode.
- BIP342's existing witness-derived signature-validation budget continues to
  charge 50 weight units for each non-empty `CHECKSIG`,
  `CHECKSIGVERIFY`, or `CHECKSIGADD`.

The one-execution limits do not remove a useful composition. CTV always checks
the same transaction and input index, so a second successful CTV can only
repeat the first expected hash. CCV binds the current input, current revealed
script, state-derived internal key, matching-index successor, and unique
successor; a second successful CCV can only repeat the same transition absent
a cryptographic collision.

`CHECKSIGFROMSTACK` and `TXHASH` remain dormant. They are not covered by these
activation limits and must receive their own cost model and activation review
before they can be enabled.

## Transaction-wide precomputation

Production mempool, serial block, stateless block, transaction-validator, and
parallel block-validation routes build one immutable precomputation object per
transaction and share it across every input.

The object caches:

- BIP119 scriptsig, sequence, and output hashes;
- BIP341 prevout, amount, scriptPubKey, sequence, and output hashes; and
- Dinero's whole-prevout confidential commitment extension.

`SIGHASH_ANYONECANPAY`, `SIGHASH_SINGLE`, annex, tapleaf, and code-separator
components remain input- or execution-specific and are computed from bounded
local data. Standalone APIs retain an uncached compatibility path; consensus
callers use the shared path.

This changes many-input CTV and BIP341 validation from repeated
transaction-wide hashing, approximately O(inputs × transaction size), to one
O(transaction size) precomputation plus O(inputs) fixed-size final hashes.
CCV performs one state/key verification and one bounded output uniqueness scan
per covenant input.

## Boundary and neuter evidence

The test suite pins both sides of each new boundary:

- one executed CTV succeeds; two executed CTVs fail;
- one executed CCV succeeds; two executed CCVs fail;
- direct and precomputed CTV hashes match;
- direct and precomputed BIP341 hashes match the official BIP341 vectors; and
- direct and precomputed hashes match for Dinero confidential prevouts and all
  supported sighash families.

Changing either per-script maximum from one to two makes its corresponding
boundary test fail because the repeated script becomes valid. This was
verified before restoring both limits to one.

## Benchmark evidence

Command:

```text
cmake --build build-covenants --target benchmark_covenant_validation -j8
./build-covenants/benchmark_covenant_validation
```

Environment: Apple M4 Max, arm64 macOS/Darwin 25.5.0, CMake
`RelWithDebInfo`, 2026-07-30. Results are microseconds per transaction or
transition and include construction of the shared precomputation once per
transaction.

| Case | Size | Direct | Precomputed |
|---|---:|---:|---:|
| CTV | 64 inputs / 3,404 bytes | 292.77 | 32.13 |
| CTV | 512 inputs / 27,152 bytes | 14,370.60 | 240.65 |
| CTV | 1,024 inputs / 54,288 bytes | 56,247.80 | 476.60 |
| CTV | 1,800 inputs / 95,416 bytes | 159,332.00 | 750.50 |
| BIP341 sighash | 64 inputs / 3,404 bytes | 1,488.83 | 92.86 |
| BIP341 sighash | 512 inputs / 27,152 bytes | 83,349.60 | 794.50 |
| BIP341 sighash | 1,024 inputs / 54,288 bytes | 330,042.00 | 1,485.00 |
| BIP341 sighash | 1,800 inputs / 95,416 bytes | 905,816.00 | 2,403.00 |

At 1,024 inputs, shared precomputation is approximately 118× faster for CTV
and 222× faster for BIP341 transaction-wide hashing. More importantly, the
measured precomputed path grows approximately linearly.

CCV output-scan measurements remained bounded from 53.2 microseconds at one
output to 59.0 microseconds at 8,192 outputs on this machine. The state/key
cryptography dominates the scan at the current transaction-size ceiling.

The near-ceiling cases were added and measured on 2026-08-30 using the Release
build. At 95,416 bytes, shared precomputation was approximately 212× faster for
CTV and 377× faster for BIP341 than recomputing transaction-wide hashes for
every input. The complete benchmark used approximately 13.5 MB maximum RSS.
CCV measured 57.3 microseconds with 8,192 outputs. This closes the previous gap
where the largest measured CTV/BIP341 transaction was only 54,288 bytes.

The activation candidate was rerun on 2026-08-30 at commit `7199ebe79` on an
Apple M4 Max Release build. The 95,416-byte case measured 737.5 microseconds
for precomputed CTV and 2,392 microseconds for precomputed Taproot sighash;
8,192-output CCV measured 56.5 microseconds. The complete process used
12,812,288 bytes maximum RSS. `covenant-readiness.sh` now fails if the largest
case is below 90,000 bytes, either precomputed near-limit transaction exceeds
10 milliseconds, or the 8,192-output CCV transition exceeds 5 milliseconds.
Those thresholds are release policy, deliberately well above current results;
the 100,000-byte transaction ceiling and one-execution-per-script rules remain
the consensus bounds.

The benchmark was rerun after rebasing onto `dinero-main` at
`cb21a91d4226a8a0b8b798f065d1b867f7de88bb` on 2026-07-31. At 1,024 inputs,
the precomputed paths measured 440 microseconds for CTV and 1,411 microseconds
for BIP341 versus 50,995 and 310,424 microseconds for the direct paths. CCV at
8,192 outputs measured 56.7 microseconds. These results reproduce the scaling
and bounds above; timing remains diagnostic rather than a consensus rule.

## Remaining activation blockers

These resource limits do not by themselves authorize shipping the activation
release. The protocol still requires:

- the reproducible consensus and cryptographic assurance record;
- a final normative specification with byte-level vectors;
- production-ready wallet/build/relay surfaces;
- release-candidate verification of newly reviewed activation parameters and
  deployment monitoring; and
- a decision on whether the dormant CSFS and TXHASH profiles are removed or
  separately specified, costed, tested, and reviewed.

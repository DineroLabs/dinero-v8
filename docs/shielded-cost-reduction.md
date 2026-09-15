# Shielded cost profile and one-pass unshield fees

## Result

The active Auth proof format spends most of its bytes on Spartan proofs:
93–97% in the measured transaction shapes. Borromean range proofs account
for 3–5%. Replacing only the range proofs cannot substantially close the
transaction-size gap with Zcash.

This patch removes the second spend-proof generation from automatic-fee
`wallet.unshield`. It measures the completed bundle, adjusts the transparent
payout and explicit fee, and refreshes the binding signature with the existing
local blinding scalar. It reduces wallet CPU work and latency. It does **not**
reduce the on-chain proof bytes, fee rate, or verification requirements.

## Measurements

Source baseline: `06a7ff369569c3a3d95e446578aed737d256ee6e` (8.1.13).
Apple M4 Max, native arm64 Release build. Real current Auth spend proof `0x06`,
cv binding enabled; one warm-process sample per shape. These are pure-builder
and full-validation timings, not phone benchmarks or RPC timings. Randomized
proofs and real transparent input/output shapes can change the serialized size.
Structured results: [measurements](benchmarks/shielded-cost-20260915.json).

| Shape | Tx bytes | Spartan bytes | Range bytes | Encrypted notes | Other bytes | Build | Verify |
|---|---:|---:|---:|---:|---:|---:|---:|
| Shield, 1 output | 46,373 | 43,185 | 2,184 | 757 | 247 | 3.47 s | 1.07 s |
| Transfer, 1 input / 2 outputs | 167,935 | 159,651 | 6,390 | 1,514 | 380 | 11.59 s | 3.96 s |
| Transfer, 2 inputs / 2 outputs | 243,661 | 232,932 | 8,733 | 1,514 | 482 | 16.63 s | 5.81 s |
| Transfer, 4 inputs / 2 outputs | 394,953 | 379,494 | 13,259 | 1,514 | 686 | 26.31 s | 9.36 s |
| Unshield, 1 input | 75,714 | 73,281 | 2,184 | 0 | 249 | 4.97 s | 1.84 s |

The benchmark builds with a fixed fee to isolate proof costs. At an illustrative
1 una/vbyte, the current wallet rule `ceil(rate * vsize) + 16` gives the
following fee estimates for these exact fixtures:

| Shape | Fee, una | Fee, DIN |
|---|---:|---:|
| Shield | 46,387 | 0.00046387 |
| Transfer, 1 input / 2 outputs | 167,950 | 0.00167950 |
| Transfer, 2 inputs / 2 outputs | 243,676 | 0.00243676 |
| Transfer, 4 inputs / 2 outputs | 394,968 | 0.00394968 |
| Unshield | 75,729 | 0.00075729 |

In a controlled fixture with a 70,000,000-una Auth note, the baseline automatic
unshield RPC took **12.767 seconds**, versus **6.871 seconds** in the completed
compatibility run (46.2% lower latency). Both returned **75,745 vbytes and a
75,761-una fee**. Two earlier optimized observations were 7.073 and 6.883 seconds.
These are individual observations on the same host, not a statistical
performance guarantee. The RPC timer includes local mempool admission and
excludes peer relay/mining. The baseline and earlier optimized measurements
preceded later fixture-only failures; those did not affect the completed RPCs.

The v6 bundle counts in base serialization, so proof bytes incur almost their
full size in vbytes. The `aggregated_range_proof` field is currently a container
of individual Borromean proofs; its name does not imply cryptographic aggregation.

### Zcash comparison

Zcash's active ZIP 317 revision 0 uses 5,000 zatoshis times the greater of two
and its logical-action count. That gives a conventional minimum of 0.0001 ZEC;
transactions with more actions, including transparent contributions, cost more.
This is wallet/miner policy, not a universal consensus fee. Orchard uses one
Halo 2 proof for a bundle of actions. Dinero's per-note proofs and byte-based
fee policy therefore differ on both dimensions. DIN and ZEC amounts are not a
dollar-cost comparison. Sources: [ZIP 317](https://zips.z.cash/zip-0317),
[ZIP 224](https://zips.z.cash/zip-0224), checked 2026-09-15.

## Utreexo compatibility contract

This is a wallet-construction change. Consensus, transaction serialization,
proof encoding, txid calculation, Utreexo leaf hashing, accumulator updates,
DNRS and pool template handling retain their existing rules.

For ordinary unshielding, the note value, nullifier, anchor, spend proof,
value commitment, range proof and bundle value balance do not depend on the
transparent payout. With one spend and no shielded outputs, the binding secret
is the spend's `rcv`, already local to the builder. A changed payout requires
a new binding signature. The validator separately checks the explicit fee via
transparent value conservation; the existing envelope sighash does not directly
include the explicit fee.

The fee and payout fields and binding signature are fixed width. The builder
checks that repricing preserves vsize, rejects invalid/overflowing fee rates,
insufficient value and dust, and publishes the transaction only after success.
The runtime marks the note pending-spent afterwards. No provisional txid or
output is submitted. The **final** txid and payout become the Utreexo outpoint
and leaf. A failure leaves the caller's envelope intact.

Private covenant notes remain excluded from this helper: their proofs can bind
transaction context. There is no reusable proof cache or exported binding key.
Explicit-fee calls retain their existing single-build path. Shield and transfer
fee construction is unchanged by this patch.

## Validation and reproduction

New fee tests were run against the interface with the original implementation
first: all three initial tests failed. The implementation then passed them.
Coverage includes real Auth proof validation, raw serialization/txid roundtrip,
final payout/fee conservation, rejection of a copied bundle in the provisional
envelope, recipient/value/fee tampering, dust and fee exhaustion, invalid and
overflowing rates, unchanged minimum fees, and exclusion of private covenants.

The complete `ShieldedValidation` suite passed: **48 cases**, 70.53 seconds.
`CSNShieldedReorgInvertibility` also passed in 104.43 seconds: reorg state
equality with the bridge, nullifier double-spend rejection, a second reorg
using durable undo, and state restoration across the shielded epoch reset.
That existing CSN fixture uses deterministic legacy shielded transactions;
the active Auth wallet path is covered by the relay lifecycle below.

The extended `ShieldedAuthRelayLifecycle` test enables Auth at height 2 and
DNRS at height 3. It exercises locked-wallet discovery, unlock/hydration,
restart and reorg, automatic-fee unshield relay to an independent miner,
canonical forest stability while pending, output proof generation/verification,
matching peer commitments, exact forest restoration on disconnect/reconnect,
and an explicitly selected transparent child spending the unshield output.

**The final mixed-version lifecycle passed**, using the optimized source wallet
and an unchanged baseline 8.1.13 mining peer. The peer admitted and mined both
the unshield and its transparent child; the spent output was then no longer
provable. Both nodes accepted the blocks, and exact Utreexo commitments matched
before and after disconnect/reconnect. Final log: `build-cost/evidence/relay-mixed-version-final.log`.

The batch verification RPC obtains amount/script data from the local wallet
UTXO index. The baseline peer could not verify the other wallet's output through
that RPC (`utxo-not-found`); the fixture therefore verifies on the owning node,
compares the peer's commitment, and makes the peer mine the child spending the
exact output. This limitation predates the patch and is not changed here.

```sh
cmake --build build-cost --target dinerod test_shielded_validation -j10
build-cost/test_shielded_validation --gtest_filter='*AuthResourceMeasurements'
ctest --test-dir build-cost -R '^ShieldedValidation$' --output-on-failure
DINEROD="$PWD/build-cost/dinerod" bash tests/integration/test_shielded_auth_relay_lifecycle.sh
# Compatibility with an unchanged peer:
DINEROD="$PWD/build-cost/dinerod" PEER_DINEROD="$PWD/build-cost/evidence/dinerod-baseline" \
  bash tests/integration/test_shielded_auth_relay_lifecycle.sh
cmake --build build-cost --target shielded_tx_builder -j10
ctest --test-dir build-cost -R '^CSNShieldedReorgInvertibility$' --output-on-failure
```

Local raw logs and baseline binaries are retained in `build-cost/evidence/`.
This patch has not been deployed.

Native Ubuntu 24.04 x86_64 qualification passed at
`5ef4c0ba5277eab9c8e6724d3d725ce76224947b` in
[run 34992010013](https://github.com/DineroLabs/dinero-v8/actions/runs/34992010013).
The automatic-fee RPC observations were **24.767 seconds baseline / 14.159
seconds optimized** (42.8% lower elapsed time), with the same **75,745 vbytes /
75,761-una fee**. The 48-case validation suite, all four stateless reorg legs,
and both baseline/candidate Auth lifecycles against the unchanged 8.1.13 peer
passed. The separate readiness workflow passed 16 suites; protocol and state
vectors passed on Linux x86_64 and arm64. These timings are individual
observations, not performance thresholds.

The `Unshield qualification` GitHub Actions workflow repeats these gates on
native Ubuntu 24.04. It builds the candidate and pinned baseline
`06a7ff369569c3a3d95e446578aed737d256ee6e` with matching headless release flags,
checks the OpenSSL pin and fleet dynamic-library allow-list, runs shielded
validation and CSN reorg tests, then runs both baseline and optimized wallets
against the unchanged baseline mining peer. Commit/binary identities, full
test logs and comparative RPC observations are retained as workflow artifacts.
The integration gate also explicitly runs `GenesisInvariants`,
`BlockTemplateDeterminism` and `MiningTemplateExclusions` against both the
pinned prerequisite and the candidate. A new integration head must pass these
checks before merge.
The workflow has read-only repository permissions and performs no release or
deployment.

## Next cost work

1. Use the one-pass unshield change as the first wallet optimization, then
   profile shield/transfer preparation separately. Transfer fees change a
   shielded change note's value; its old proof cannot simply be reused.
2. Prioritize reducing or aggregating Spartan proof bytes. A hypothetical
   removal of every range-proof byte would save only 2.9% of this unshield
   fixture and 3.8% of the one-input transfer. An actual replacement saves less.
3. Evaluate an action-based or resource-weighted fee policy against measured
   validation CPU and network bandwidth. Lowering wallet fees alone would
   cause existing mempools to reject them and would not remove the resource cost.
4. Any proof-system change needs separate cryptographic review, activation and
   cross-version consensus qualification. Preserve the finalized-output/Utreexo
   contract and test full-node/CSN, pool, reorg and restart behavior before rollout.

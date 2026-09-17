# 60-second block target: implementation contract and qualification plan

Status: target selected by the owner on 2026-09-17; design candidate, not an
activated consensus change. The first target is **60 seconds**. A 30-second
target is future research, outside this change.

Source reviewed: main commit `7287d77864fa328521847519559b9ed1d7240c16`.
This design is isolated from the compact-proof qualification branches. It does
not assign an activation height or change a production parameter.

## Scope and decisions

- Reduce the target interval from 120 to 60 seconds through a coordinated,
  height-activated consensus upgrade. Block arrival remains probabilistic.
- Working economic policy: preserve expected issuance over elapsed target time,
  including the halving schedule and tail issuance. This follows the prior
  recommendation; the owner has separately been asked to confirm this policy.
- Preserve all historical block verdicts, already-issued rewards, transaction
  bytes, Utreexo leaf formats and historical proof verification.
- Keep existing per-block byte, weight and proof-count limits in the initial
  experiment. This doubles their nominal hourly capacity and therefore requires
  fresh resource qualification; it is not a claim that every node can sustain it.
- Use a separate activation setting from compact proofs. A shared future release
  or activation date requires both changes to pass separately and in combination.
- Mainnet activation height, exact ASERT transition algorithm, confirmation
  policies and treatment of time-sensitive block windows remain review items.

## Expected effects at steady hash power

| Property | 120-second target | 60-second target |
| --- | ---: | ---: |
| Expected blocks per day | 720 | 1,440 |
| Initial-epoch reward, preserving issuance | 100 DIN | 50 DIN |
| Initial-epoch expected issuance/day | 72,000 DIN | 72,000 DIN |
| Tail reward, preserving issuance | 1 DIN | 0.5 DIN |
| Tail expected issuance/day | 720 DIN | 720 DIN |
| Full halving epoch in blocks | 1,314,000 | 2,628,000 |
| Wall time represented by 100 blocks | about 200 minutes | about 100 minutes |

These are target-rate calculations, not a promise of wall-clock block production
or a statement of the currently circulating supply. Historical rewards stay
unchanged. The epoch that spans activation needs proportional accounting, not
simply a global replacement of the old halving constant.

## Monetary transition reference model

Let `A >= 1` be the first block under the new target, `L = 1,314,000`, and
`h >= 1` the candidate height. Treat one old block as two progress units and one
new block as one unit. Progress completed *before* the candidate is:

```
u(h) = 2 * min(h - 1, A - 1) + max(h - A, 0)
epoch(h) = floor(u(h) / (2 * L))
old_epoch_reward(h) = max(100 * UNA_PER_DIN >> epoch(h), UNA_PER_DIN)
reward(h) = old_epoch_reward(h)       when h < A
            old_epoch_reward(h) / 2  when h >= A
```

Genesis keeps its historical treatment. Use bounded integer arithmetic and the
existing guard for large shift counts. The current non-tail epoch rewards and
the tail floor are divisible by two in una, so this schedule needs no rounding
policy. Helpers must handle the disabled activation sentinel explicitly, before
arithmetic. Cumulative issuance must integrate the two eras without rewriting
pre-activation history.

For a halving threshold at old completed-block count `K * L > A - 1`, the
first block of that new reward epoch becomes:

```
A + 2 * (K * L - (A - 1))
```

This preserves progress already earned toward the next halving. An activation
at an existing halving boundary must apply both the scheduled halving and the
interval adjustment exactly once.

Independent tests must prove that pairs of new blocks pay precisely one
corresponding old block's subsidy, including activation adjacent to a halving,
the tail transition, large heights and cumulative totals. A literal reference
vector must reject both a global `2 * L` replacement and a reward-only change.

## Difficulty transition

The production ASERT path uses genesis as its anchor and a 43,200-second
half-life. Its expected elapsed time currently multiplies all height progress
from that anchor by one spacing value. Replacing 120 with 60 globally would
reinterpret old history and can produce a large unintended difficulty change.

Required properties of the new transition:

1. For every height below A, preserve exact historical expected bits.
2. At and above A, use an explicitly specified schedule/anchor and integer
   rounding rules. The transition must account for the old 120-second era.
3. Define whether difficulty scales immediately or converges through ASERT.
   Simulate both before choosing; do not silently inherit either behavior.
4. Keep the existing 12-hour half-life and PoW limit for the initial study;
   assess floor saturation and timestamp manipulation. Any change to either
   is a separate explicit decision. A spacing target cannot overcome a binding
   minimum-difficulty limit at insufficient hash power.
5. Derive results from the candidate branch, never mutable global active-tip
   state. Header-only, full-node, CSN, mining and reindex paths must agree.
6. Reorgs across A must restore the old rule and produce identical expected bits
   to a fresh node on that branch, including restart mid-reorg.

Do not implement a guessed new anchor. A piecewise expected-time schedule and
an explicitly normalized activation anchor are candidates requiring independent
vectors and hash-rate/timestamp simulations first.

## Block-count windows and existing contracts

Do not globally double constants: some express block depth, some approximate
wall time, and some are already committed in signed transactions or state.

| Area | Existing semantics | Required disposition |
| --- | --- | --- |
| Coinbase maturity | 100 blocks in full and stateless paths | Decide explicitly whether to retain depth or preserve time; test outputs created on both sides of A |
| Shielded anchor history | 100 recent roots; committed by SHR1/DNRS | Initial prototype retains depth; changing to 200 requires its own state/undo transition and vectors |
| Height-based absolute/relative locks | Signed or consensus-enforced block heights | Existing bytes keep their meaning; inventory accelerated wall-time deadlines, especially contract/channel safety margins |
| Time-based locks and median-time rules | Time units and timestamp history | Preserve time-unit rules; review the shorter real-time span of median-block windows |
| Utreexo proof freshness/mempool refresh | Includes block-based age policies | Identify each call site, exercise online/offline wallet refresh and reorg admission; adjust policy only with a stated rationale |
| Checkpoints, retained tip history, undo and reorg limits | Mostly block counts | Document reduced elapsed-time coverage, storage growth and reconstruction bounds |
| Merchant/exchange confirmations | Application policy, not a consensus guarantee | Set by measured risk/work and propagation, not an unchanged numeral |
| UI/RPC estimates, alerts and pool work | Spacing-derived or hardcoded time assumptions | Report the appropriate era and react to actual next-block rules |

At steady hash power, expected work per new block is roughly half the old
amount after adjustment. Doubling confirmation count matches approximate work,
but does not itself prove equal security under propagation and reorg effects.

## Utreexo and shielded invariants

Faster blocks do not require a new Utreexo leaf or proof encoding. Both full
nodes and CSNs must agree on every block's forest/stump commitments and on the
applicable subsidy and maturity rules. Shielded notes remain outside Utreexo;
unshield transparent outputs enter it using their actual transaction identities.

The qualification chain must mine a shield, transfer, unshield and signed spend
of the unshield output around A; verify inclusion and spentness, historical-root
proof verification, disconnect restoration, restart and complete reindex.
Repeat through partitions and a reorg across both timing and compact-format
boundaries. All nodes must agree on roots, nullifiers, anchors and canonical
state. No skipped validation or cached-only success qualifies this gate.

Two times as many blocks also means more headers, coinbases, commits and proof
updates. Transaction payload storage does not automatically double at unchanged
transaction demand. Measure phone catch-up, bandwidth, idle energy, database
write rates, disk growth and checkpoint replay latency separately.

## Resource and network qualification

The existing hardware report measured an eight-unshield proof workload at
23.261 seconds on its Linux runner, under an earlier runtime. Its 30-second
proof-component budget was chosen against a 120-second interval. These are
historical observations, not measurements of the latest optimized daemon.

Remeasure the exact candidate on representative Linux x86-64, ARM64 and mobile
verification paths. Include cold and warm caches, previously unseen valid
transactions, worst permitted proof mixes, maximum permitted bytes, concurrent
wallet/RPC load, and malformed-input rejection. Measure complete receipt-to-
validated-tip latency as well as proof CPU time; mempool preverification alone
cannot establish safety for adversarial block contents.

Run realistic 120/60-second arrival traces with bursts, network delay, loss,
partitions and hash-rate changes. Immediate `generatetoaddress` regtest blocks
verify state transitions but do not qualify a 60-second live cadence. A private
network with PoW enforcement plus reproducible arrival/load simulation must
show sustainable catch-up, acceptable stale/reorg behavior and representative
latency tails. Select documented acceptance thresholds before judging results;
do not reuse the old 30-second proof budget as proof of adequate new headroom.

## Implementation inventory

This is an initial inventory, not a completed transitive call-site audit.

| Boundary | Source paths to cover |
| --- | --- |
| Network and height-dependent timing | `include/consensus/chainparams.h`, `src/consensus/chainparams_impl.cpp`, `src/consensus/consensus.hpp`, `include/consensus/asert_params.h` |
| Shared header/daemon difficulty | `include/consensus/pow_context.h`, `include/consensus/asert.h`, `src/consensus/asert_canonical.cpp`; audit other ASERT implementations for callers |
| Stateful and stateless subsidy | `include/consensus/subsidy.h`, `include/consensus/stateless_verification.h`, `src/consensus/block_validation.cpp` |
| Templates and reward descriptions | `src/mining/block_assembler.cpp`, pool template/job interfaces |
| Consensus/economics/hashrate reporting | `src/rpc/methods_consensus.cpp`, `src/rpc/methods_economics_context.cpp`, `src/rpc/methods_blockchain_context.cpp` |
| Maturity agreement | `include/consensus/coinbase_maturity.h`, `include/consensus/utreexo_maturity_leaf_activation.h`, block/transaction validation, wallet selection |
| Anchor commitments and undo | `include/consensus/shielded/anchor_history.h`, shielded state persistence, connect/disconnect/reindex |
| Operational timing | Wallet estimates, mobile sync, proof freshness, checkpoints, monitoring and separate `dinero-sv2-pool` repository |

The stateless subsidy helper currently duplicates the monetary constants; it
must not remain on the old schedule while the full validator changes. Likewise,
editing ChainParams::target_spacing alone does not update the separate
Consensus::targetSpacingSec used by the shared difficulty path.

`docs/block-time-economics-lockin.md` describes an obsolete 520-second economic
model and is not authority for this change. Runtime code and verified vectors
are the source of the reviewed 120-second baseline.

## Delivery order and activation gates

1. Review the economic model and block-window dispositions; specify ASERT
   transition and literal independent boundary vectors.
2. Add failing subsidy/difficulty/full-CSN agreement tests before production
   changes; implement shared height-aware helpers with activation disabled on
   mainnet. Permit explicit activation only in isolated test networks.
3. Exercise old/new peers, activation reorgs, crash recovery, restart and reindex.
   Verify the test fails when either the stateful or stateless path is neutered.
4. Complete exact-binary Linux/ARM qualification and loaded network/phone tests.
   Register every new CTest in a CI lane; no silent coverage exemptions.
5. Qualify interaction with the independently reviewed compact-proof change.
6. Choose a release, upgrade window and mainnet height only after the evidence
   is complete. A consensus activation is not safely undone by merely reverting
   a binary after the boundary; rehearse contingency behavior before deployment.

A decision to target 60 seconds authorizes the design and implementation work.
It does not by itself assign A, deploy a binary, enable compact proofs, or make
30-second blocks an approved future activation.

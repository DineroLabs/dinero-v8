# 60-second block target: implementation contract and qualification plan

Status: timing and economic direction selected by the owner on 2026-09-17;
implemented as a dormant qualification candidate, not an activated consensus change. The first target is **60 seconds**. A 30-second
target is future research, outside this change.

Source reviewed: main commit `7287d77864fa328521847519559b9ed1d7240c16`.
This design is isolated from the compact-proof qualification branches. It does
not assign an activation height or change a production parameter.

## Scope and decisions

- Reduce the target interval from 120 to 60 seconds through a coordinated,
  height-activated consensus upgrade. Block arrival remains probabilistic.
- Selected economic policy: retain the 100 DIN initial reward and the
  1,314,000-block halving interval, so complete post-upgrade epochs target about
  2.5 years. Lower the tail floor from 1 DIN to 0.5 DIN at the upgrade. This
  deliberately accelerates early issuance while preserving the tail issuance
  rate per unit time. The owner selected this after the economic comparison.
- Preserve all historical block verdicts, already-issued rewards, transaction
  bytes, Utreexo leaf formats and historical proof verification.
- Keep existing per-block byte, weight and proof-count limits in the initial
  experiment. This doubles their nominal hourly capacity and therefore requires
  fresh resource qualification; it is not a claim that every node can sustain it.
- Use a separate activation setting from compact proofs. A shared future release
  or activation date requires both changes to pass separately and in combination.
- Mainnet activation height, independent review of the ASERT transition,
  confirmation policies and resource/network qualification remain open gates.

## Expected effects at steady hash power

| Property | 120-second target | 60-second target |
| --- | ---: | ---: |
| Expected blocks per day | 720 | 1,440 |
| Initial-epoch reward | 100 DIN | 100 DIN |
| Initial-epoch expected issuance/day | 72,000 DIN | 144,000 DIN |
| Tail reward | 1 DIN | 0.5 DIN |
| Tail expected issuance/day | 720 DIN | 720 DIN |
| Full halving epoch in blocks | 1,314,000 | 1,314,000 |
| Full halving epoch at target rate | about 5 years | about 2.5 years |
| Wall time represented by 100 blocks | about 200 minutes | about 100 minutes |

These are target-rate calculations, not a promise of wall-clock block production
or a statement of the currently circulating supply. Historical rewards stay
unchanged. Halving heights stay fixed: activation does not reset the epoch or
start a fresh 2.5-year countdown. The remaining blocks in the activation epoch
are produced at the new cadence; only a complete subsequent epoch takes about
2.5 years. No future epoch is doubled to 2,628,000 blocks under this decision.

## Monetary transition reference model

Let `A >= 1` be the first block under the new target, `L = 1,314,000`, and
`h >= 1` the candidate height:

```
epoch(h) = floor((h - 1) / L)
base_reward(h) = 100 * UNA_PER_DIN >> epoch(h)
tail_floor(h) = 1 * UNA_PER_DIN      when h < A
                UNA_PER_DIN / 2    when h >= A
reward(h) = max(base_reward(h), tail_floor(h))
```

Genesis keeps its historical treatment. Preserve the existing guard that
returns a zero base reward instead of shifting by 64 or more. An unset/disabled
activation retains the old target and old floor at every height. Resolve the
rule from the candidate height and network, including reorg and replay paths.

There is no extra reward halving at A. In the initial epoch the reward remains
100 DIN; if activation occurs in a later epoch, retain that epoch's applicable
base reward rather than resetting it to 100 DIN. The lower floor first affects
heights where the old 1 DIN minimum would otherwise have applied.

The selected sequence is:

```
100 -> 50 -> 25 -> 12.5 -> 6.25 -> 3.125 -> 1.5625 -> 0.78125 -> 0.5 forever
```

The unchanged first blocks of reward epochs are `K * L + 1`. If A precedes the
first affected floor height, the 0.78125 DIN stage starts at height 9,198,001,
and the 0.5 DIN tail at 10,512,001. In that case pre-tail mining issuance totals
261,773,437.5 DIN, excluding genesis, compared with 260,746,875 DIN before the
old tail. These totals are not an equal-date supply comparison: early issuance
and the start of the tail occur sooner in wall time. If activation were delayed
past those heights, cumulative accounting must include the actual old-floor
rewards already issued; never recompute history with the new floor.

Cumulative issuance is the old schedule through A-1 plus the applicable new
schedule from A onward. Existing balances and previously mined rewards are not
rescaled. Independent tests must pin every halving edge, A-1/A/A+1, activation
at a halving, both floor transitions, disabled activation, large shifts/heights,
and late hypothetical activation. Stateful validators, stateless validators,
miners and supply RPCs must agree on exact una amounts. Negative controls must
catch an accidental 50 DIN reward at A, a doubled halving interval, and applying
the new floor retroactively to pre-activation blocks.

## Difficulty transition

The old rule remains byte-for-byte below A, including its historical anchor
height zero and block-one timestamp derivation. At/above A, resolve the header
at height **A-1 on the candidate's own branch** and use this tuple:

```
anchor.height = A-1
anchor.time   = header[A-1].timestamp
anchor.bits   = header[A-1].difficulty
expected_elapsed(h) = (h - (A-1)) * 60
excess = max(parent_MTP + 1, candidate_time) - anchor.time - expected_elapsed
```

There is no automatic factor-two target reset. ASERT adjusts toward 60 seconds
with the existing 43,200-second half-life. The new reference starts from the
recorded difficulty at the boundary; old elapsed time is never retrospectively
reinterpreted at 60 seconds. Activation at height one uses the genesis header.

Qualification found three defects in the deployed arithmetic. Corrections apply
only at/above A; changing the historical path would reject existing blocks:

- The old compact encoder normalizes by leading bit rather than byte. It can
  increase the encoded target beyond the nominal configured cap. The new path
  uses byte normalization and rounds down.
- The old fractional polynomial shifts the squared and cubed Q16 terms down
  before multiplying their coefficients, producing a discontinuity at whole
  half-lives. The new path uses the full integer powers with the same cubic
  coefficients. The 64-bit polynomial sum remains bounded.
- The old target arithmetic can discard overflow before applying its cap. The
  new path caps before an overflowing left shift and retains the multiplication
  carry until after fixed-point division.

Historical PoW verification intentionally accepts the exact ASERT-required bits,
including outputs above the nominal cap; this is documented in
`src/consensus/header_chain.cpp`. The new arithmetic consistently caps at the
configured target. If the boundary's old target exceeds that cap, the cap can
increase required work at activation. This and insufficient hash power at the
floor are explicit rollout gates, not conditions under which 60-second arrivals
can be promised.

Retaining the old genesis reference while correcting its encoding produced a
roughly 128-fold boundary work jump in two exploratory timing scenarios. The
branch-specific A-1 anchor removes that encoding-induced jump in those scenarios.
The same model still shows different initial adjustment periods, so production
transition and timestamp-manipulation qualification remain required.

Header validation reads the boundary from its own ancestry. Block acceptance
copies it under the selector lock when using the header-only path. Mining/RPC
read it from the active ChainDB. Missing boundary data fails closed instead of
borrowing another branch's anchor or guessing a timestamp. Snapshot-import
and header-backfill availability need explicit operational qualification.

Independent tests cover historical/activated arithmetic, overflow, rounding,
header-only/full/value-context/mining agreement, distinct fork boundaries,
missing context, rollback, and portable 128-bit arithmetic. The old
`pow_asert.hpp` helper is used only by its legacy signed-shift test; the unused
`asert_canonical.cpp` implementation contains placeholders and is not an
alternative production path.

## Block-count windows and existing contracts

Do not globally double constants: some express block depth, some approximate
wall time, and some are already committed in signed transactions or state.

| Area | Existing semantics | Required disposition |
| --- | --- | --- |
| Coinbase maturity | 100 blocks in full and stateless paths | Prototype retains 100; tests pin 99-block rejection and 100-block acceptance on both sides of A |
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

Production subsidy call sites use the network activation height explicitly.
Pure/reference APIs take it as a value; omitting it preserves legacy behavior.

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

The reference stateless helper now delegates to the same subsidy implementation.
The actual daemon validation and mining paths also pass the network activation
height. ChainParams carries that height into the shared difficulty context;
its legacy target-spacing field stays 120 so old blocks retain their rules.

`docs/block-time-economics-lockin.md` describes an obsolete 520-second economic
model and is not authority for this change. Runtime code and verified vectors
are the source of the reviewed 120-second baseline.

## Delivery order and activation gates

1. Review the implementation of the selected economic model and block-window
   dispositions; specify ASERT
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

## Running the dormant implementation

All three compiled networks default to `UINT32_MAX` (disabled). Only regtest
accepts `--consensus-sixty-second-height=<A>`, with `1 <= A < UINT32_MAX`;
`UINT32_MAX` explicitly disables it. Mainnet/testnet overrides and malformed
values are rejected. No production activation height is included in this PR.

The CTest gates `SixtySecondConsensus`, `SixtySecondAsertOracle` and
`SixtySecondAsertTransitionModel` run in the normal Linux test lane.
`SixtySecondActivation` and `SixtySecondShieldedLifecycle` run in the mandatory
serial daemon lane. None is exempted from execution coverage.

Regtest mines instantly and bypasses difficulty. Its passing lifecycle tests
prove state transitions, not a sustainable live 60-second cadence. See the
qualification report for exact results and remaining gates.

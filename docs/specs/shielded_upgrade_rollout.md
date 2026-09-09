# Shielded upgrade release and activation plan

Status: implementation candidate on `codex/shielded-integration`. This document
is the forward-looking plan; the old shielded_activation_plan.md is historical.
No production height or release approval is implied by this plan.

## Release units

1. Land the existing-main epoch-undo repair independently (PR #719). It changes
   storage capture/restore, not consensus root encoding or activation heights.
2. Review and qualify the combined candidate (PR #720) on a pinned commit and
   pinned binary hashes. A green main run is not candidate evidence.
3. Publish qualified artifacts with new upgrades dormant. Inventory validators,
   archival/reindex nodes, snapshot publishers/consumers, miners, wallets and
   mobile clients. Record version, architecture, binary hash and rollback backup
   for every deployed role. Canary first, then each fleet member; compare roots
   at the same height/hash after restart and replay before enabling anything.
4. Select activation heights in a separate reviewed commit after the approvals
   and compatibility checks below. Repeat candidate verification for that commit.

## Compatibility contract

| Boundary | Required action |
|---|---|
| Existing epoch undo | New records retain the eviction journal. Legacy records remain readable but are lossy. Rebuild affected historical undo from block history using the documented reindex path before promising cross-reset rollback. Preserve the original datadir and prove the rebuilt copy by restart, invalidation below the reset and reconnect. A snapshot of active anchors alone cannot reconstruct the journal. |
| DNRS mining | Use daemon-owned `coinbasetxn` unchanged, or `mining.getjob`/`mining.submit`. Header layout remains 128 bytes. Transaction selection, coinbase outputs, parent and ASERT timestamp require a new template. Pools which rebuild coinbases must migrate before activation. |
| Snapshots | Upgrade publishers and consumers to v5 before DNRS activation. A v4 snapshot at an enforced base must be rejected. Check selected-chain ancestry, burial, merkle proof and full SHR1 before state import. Retain full-history fallback. |
| Auth relay | Upgrade sending wallets, relay peers and miners together. The 512,000-byte transaction, 2,048,000-weight, 4-spend/2-output and 8-proof/1,000,000-byte block limits are a coordinated consensus/resource profile. Older peers can reject valid new transactions; do not rely on paths through them. |
| Recipient/view authority | Require 107-byte addresses and the explicit spend/view-key API in every supported caller. A viewing key must never acquire spend authority. Verify restored wallets and recipients, not only newly created senders. |
| Auth epoch cutover | The distinct spend-authority epoch reset must equal its activation and differ from the historical reset. The reset block is shielded-empty. Pre-reset notes are not spendable afterward. Prove the live pool empty or obtain an explicit migration decision before scheduling; never silently strand balances. |
| Hardware wallets | Host capability mocks do not qualify devices. Initial release scope may explicitly exclude shielded Ledger/Trezor support; unsupported capabilities must remain rejected. If devices are advertised, require real firmware and physical-device tests. |
| Downgrade | Before activation, restore a tested backup or follow a verified data-format downgrade path. After activation, older consensus binaries must not validate/mine the upgraded chain. Emergency handling is a reviewed forward fix, not silently disabling enforcement or rolling back network history. |

## Supported mining interfaces

The legacy `MiningCoordinator`/worker prototype and its unregistered miner-control
RPC were never in the CMake source lists and referenced a removed daemon-context
member. They have been retired instead of adding another coinbase assembler.
The supported coordinator is the immutable `mining.getjob`/`mining.submit` path
in methods_mining_v14.cpp. Both it and the registered `getblocktemplate` handler
call BlockAssembler::CreateNewBlock. StateCommitmentMining exercises both with
DNRS enforced, verifies the external block's post-state root, and checks restart.
External miners must preserve DNRS, DNRF, witness commitments and Utreexo data.
The response's statecommitment object describes the canonical output; it is not
permission to rebuild the coinbase or reorder transactions.

## Evidence and independent review

Record exact commit/tree, build configuration, toolchain, binary SHA-256 and CI
run URL for Linux x86-64, Windows x86-64 and both supported macOS architectures.
Execute the complete registered suite, mandatory serial daemon lanes, Gate D,
recipient-spend lifecycle, persisted epoch reorg, live/reindex/replay parity and
release acceptance parity against a separately identified main binary. Preserve
first-failure logs and controls; a passing retry is not a root-cause fix.

The user-designated reviewer must assess proof/key authority, transcript/domain
separation, outgoing encryption, resource accounting, reset/dormancy boundaries,
coinbase prediction vs connect/reindex, snapshot authentication before mutation,
selected-chain burial, and legacy-undo compatibility. Record reviewer identity,
reviewed commit, findings/disposition and explicit sign-off. The user designated Codex as reviewer; its implementation-agent review is
recorded in SHIELDED_INTEGRATION_REVIEW.md and must not be labeled independent.

## Resource qualification

The existing five measured shapes span 46,373–394,953 bytes, including the
167,935-byte recipient-plus-change transfer. Peak proof-process RSS was about
802 MB on Apple M4 Max. This is evidence for that host, not a minimum hardware
specification. On each supported minimum host, run all five shapes plus maximum
block verification, repeat with relay/mining load, and record peak RSS, wall/CPU
time, p50/p95 latency, concurrency and swap/OOM behavior. Include restore, reorg,
and reindex. The release owner must approve memory/latency budgets and supported
concurrency before results can be called passing. Mobile proving is unsupported
until it has its own measured profile; verification-only support is a separate
claim. Keep proof concurrency bounded to the qualified value.

## Decisions required before a height exists

- Completed review on the candidate; identify reviewer independence accurately.
- Minimum supported hardware and measured budget approval.
- Production inventory, canary results and operator rollback drill.
- Burial depths: current 288/48/8 values are provisional. Approve by network,
  with the assumed reorg/finality risk documented; do not treat test constants as
  an economic security decision.
- Empty-pool proof or an explicit reviewed migration plan for the Auth reset.
- Compatibility lead time based on actual fleet/miner/client readiness. Keep
  DNRS activation separate from an epoch reset and avoid an unreviewed overlap.
- A future activation-height commit reviewed against the then-current tip, with
  sufficient deployment margin and all sentinel/order/boundary tests passing.

Until all decisions and evidence exist, mainnet/testnet DNRS and the new Auth
upgrade remain at UINT32_MAX. Existing shielded activation is unchanged.

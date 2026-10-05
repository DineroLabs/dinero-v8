# Typed startup reindex replay

DaemonApp routes a retained runtime delivery log (including a final tip below activation), or an active Orchard tip, through OrchardReindexOwner. Missing retained coverage at an active tip refuses. Ordinary pre-origin reindex retains its existing path. The daemon owns startup's datadir guard and the existing final database/frontier/nullifier promotion journal. Mainnet activation remains unset.

The owner scans exact block frames, independently replays the historical prefix with AssumeUtxoReplayEngine, writes an isolated legacy candidate, and compares its coins, ancestry/work, forest marker, frontier and anchors. Only then does it mark that prefix validated. The closed private prefix is copied and converted using the existing checked storage migration engine. No source ChainDB is migrated or replaced during reconstruction.

Every retained Orchard connect/disconnect passes the selected contextual header gate and existing indexed canonical writer. The writer reconstructs state, coin undo, forest, filters and delivery records. Each reconstructed delivery digest must equal the retained source. Historical transitions after a rollback below activation use a new full genesis replay; the private startup owner then copies exact already-validated delivery records, installing the head last. Consumer cursors and wallet databases are unchanged. Invalidation bits are preserved only for independently replayed inactive headers, and refuse if inconsistent with final active ancestry.

This initial implementation is not yet runtime-qualified. Existing real HTTP shield/send/unshield/restart/reorg/reindex assertions are unchanged and must pass. Cross-boundary replay, malformed retained history, restart promotion, backend-OFF, final full project instrumentation and platform/load qualification remain required. Header ancestry is rebuilt per Orchard event and historical crossings repeat full prefix replay; this has not met large-history performance goals. The inherited inventory and explicit migration/retirement budgets are operational limits, not measured resident-memory bounds. Publication and CI dispatch remain held under the existing excluded-test constraint.

## First diagnostic and next qualification

The first fresh ON daemon/25-target build passed; seven of eight selected CTests passed. The real HTTP lifecycle reached shield/send confirmation, then the preexisting replacement-connect failure recurred before reindex: canonical-write-preparation / Orchard state lookup failed. Typed reindex therefore remains unexecuted in that run. No test assertion or deadline was relaxed.

The next revision compares every persisted historical nullifier with independent replay and compares the serialized forest checkpoint, including historical final tips. It checks ancestry linearly and includes genesis in failed-policy refusal. The HTTP fixture retains all prior assertions, then adds below-activation reindex, historical disconnect/reindex and historical replacement/reactivation/reindex with original signed operations and exact consumer progress. Existing deadlines are unchanged. Constant operation labels carry the original Orchard lookup status through canonical staging and indexed preparation so a recurrence identifies the rejected operation. Those labels do not change the rejection policy or establish a repair.

## Indexed locator diagnostic

The second fresh ON diagnostic again built all 25 targets and passed seven component CTests. The HTTP lifecycle failed before full reindex while preparing the replacement unshield block. Status-preserving diagnostics identify indexed body/undo locator validation with Corruption. The failing individual field is not yet established. Additional constant labels distinguish durable/live metadata, existing body/undo reads and new locator writes while retaining the original refusal predicate and application statements. This is diagnostic work, not a claimed repair; all HTTP assertions and deadlines remain unchanged.

## Incoming index status publication

The third fresh ON diagnostic identifies the replacement refusal as `index/live-status`: the live status differs from the exact persisted status before canonical preparation. Source inspection finds the incoming path ORs the persisted flags into those inferred by `AddBlockIndex`. A later best-header import can overwrite the inferred flags, which makes the result depend on which equal-work sibling is preferred.

The incoming path now assigns the exact status it just persisted, under its existing selected/graph owners. The checked writer still requires exact live/durable agreement and owns canonical validity/undo publication. The service's existing body-to-connected-base candidate rule already admits this stored candidate; no eligibility or refusal guard changes. A new ordinary RawIngress case submits a body while an equal-work lower-hash header-only sibling remains preferred, requiring successful canonical publication and exact durable/live status and locators. All earlier fixture bodies and HTTP deadlines/assertions remain unchanged. Local linked and HTTP results still required; no repaired/release claim from preparation alone.

## Enforced-PoW lifecycle fixture and ingress logger dependency

The incoming-index diagnostic built all 25 targets and passed eight of ten selected CTests. Its HTTP unshield/reorg/readmission sequence passed. The added header-only sibling case reached a missing logger dependency in the synthetic service setup; RawIngress now installs the real LoggerService used by existing fixture precedent, with all assertion bodies unchanged.

The HTTP test reached typed reindex and failed at historical height 1. The recorded block has regtest bits `0x207fffff` and a hash below its target; ordinary non-enforced reindex calls `CheckProofOfWork(..., true)`, which rejects that target against standard `MAX_BITS = 0x1d31ffce`. This is a target-range/profile mismatch, not evidence of unsolved work. The lifecycle fixture now selects the existing enforced-PoW/ASERT profile before generating any history, derives its network magic and binds its marker under the actual datadir lock before startup. Its external config keeps the initial datadir fresh. HTTP checks the reported profile and exact marker on every start, including migration/reindex, and retains the checksum across starts.

Production reindex validation is unchanged. The process-local Orchard schedule remains height 102/branch 1; the existing ConsensusChecksum is not claimed to bind every Orchard parameter. Original HTTP operations/assertions/deadlines and ingress regression assertions remain intact. Fresh linked/runtime qualification is required; these setup changes alone do not establish successful reindex or release readiness. Ordinary skip-PoW regtest reindex support remains a separate compatibility question.

## Explicit timing profile for the real-work lifecycle

The enforced-PoW diagnostic passed all five RawIngress cases, including the preferred-header sibling regression, and nine of ten CTests overall. HTTP reported the enforced profile but generation returned bits `0x02008000` for the first block while the timing activation remained unset. The 60-second RPC timeout and 45-second shutdown timeout both failed; the fixture terminated its own child. No reindex or lifecycle success is claimed for that run.

The next isolated fixture explicitly activates 60-second timing at height 1, before generating its historical prefix. It therefore uses the existing timing-active saturating ASERT implementation, without changing historical consensus or any production validation. Every HTTP start asserts activation height 1 and target spacing 60 in addition to the persisted PoW profile. Orchard remains independently scheduled at 102/branch 1; compact prerequisites and public schedules are unchanged. Existing timing consensus, independent oracle and transition-model tests join the fresh diagnostic unchanged.

This setup qualifies a one-minute real-work history crossing Orchard activation. It does not itself qualify a coupled public activation or two-minute historical transition; those remain separate qualification. Original wallet operations, signed identities, reindex/reorg requirements and deadlines remain unchanged.

## Consistent copy-migration budgets

The one-minute enforced-PoW diagnostic built all 27 targets and passed twelve of thirteen CTests, including the unchanged timing tests and RawIngress regression. HTTP passed shield/send/unshield and restart/reorg/readmission, then rebuilt the historical prefix through height 101. Copy migration correctly refused the caller's contradictory 4MiB batch/32MiB record budgets.

The reindex caller now explicitly budgets a 32MiB batch, equal to its existing maximum record size, with a compile-time consistency check. The 4096-row and ten-million-nullifier ceilings remain; no migration-engine guard, validation, fixture assertion or deadline changes. This increases the configured batch allowance from 4MiB to 32MiB and is not a total resident-memory or production-load qualification. Fresh full/runtime validation remains required; no complete reindex or release claim from this edit.

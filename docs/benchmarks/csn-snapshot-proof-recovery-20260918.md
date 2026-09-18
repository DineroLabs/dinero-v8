# CSN snapshot proof and restart qualification

## Problem and change

A compact stateless node can advance its forest beyond an imported snapshot
without advancing the full in-memory snapshot coin map. Rebuilding the outpoint
position cache from that historical map mistakes spent snapshot coins for missing
current coins and omits newer coins. The startup coverage guard then enters safe
mode even though the validated forest roots are correct.

The batch proof verifier also consulted only the ordinary ChainDB coin table,
although imported coins live in the frozen pre-base store. Generated snapshot
proofs could therefore fail local verification as `utxo-not-found`.

The fix uses ordinary current coins plus snapshot records bound to the exact base
hash and height for CSN startup coverage. Frozen records count only when their
creation-metadata leaf exists in the current forest. Missing **ordinary current**
coins still trigger recovery. Malformed records and an incorrect snapshot binding
fail the rebuild without replacing the previous index.

Single/batch proof generation derives the canonical leaf and resolves its position
in the forest being proved, instead of trusting the separate outpoint cache.
Generation and batch verification share the existing base-bound, live-leaf-checked
snapshot fallback. Caller or wallet metadata never supplies the canonical leaf.
No forest hash, consensus validation rule, storage format or retention policy changes.

## Reproducible regression

`CSNSnapshotProofRestart` is registered in CTest and explicitly selected by the
serial daemon lane of `tests.yml`. Run with a built daemon:

```sh
DINEROD=/absolute/path/to/dinerod python3 tests/integration/test_csn_snapshot_proof_restart.py
```

The offline regtest creates a snapshot at height 130, spends a snapshot coin and
mines eight forward blocks. A fresh `ios_utreexo` CSN imports it, receives the real
blocks, and restarts twice with a checkpoint interval of 1,000. Checks require:

- Identical full-node/CSN height, tip, Utreexo commitment and shielded root.
- No safe mode after forward replay or either restart.
- Successful single proofs and locally verified batch proofs for both pre-base
  and post-base coins.
- Altered sibling rejection by proof verification, with `proof-invalid` rather
  than a missing-coin error.
- Rejection of a spent snapshot coin's old proof and refusal to generate a new
  proof for it, without scheduling recovery for that spent record.

The same regression fails on the pre-patch daemon. `UTXOPositionIndexRebuild`
independently checks exact base binding, a genuinely spent frozen coin, missing
ordinary coin detection, and malformed metadata preserving the previous index.
Existing `UtreexoBatchProofCanonicalCoins` and `SnapshotNullifierRestart` cover
the conventional coin path and nonempty shielded snapshot durability respectively.

## Field-copy result and remaining gates

Native macOS replay used the preserved mainnet snapshot at 112026 and all 447
available forward blocks through 112473, on disposable copies only. The candidate
passes two restarts with safe mode inactive; Utreexo and shielded roots match the
pre-patch replay exactly. Both pre-base and post-base proofs verify, and an altered
sibling fails. The imported nullifier cache retains its one row. All 53 original
evidence files remain hash-identical.

**This is not complete recovery or rollout qualification.** A separate attempt
to disconnect two blocks fails before mutation with
`restore-missing-height-index-at-checkpoint-112026`. The checkpoint lookup cannot
resolve the snapshot base through the ordinary ChainDB height index. The synthetic
sparse-checkpoint probe reproduces that failure at base 130; startup logs also show
missing CSN delta sidecars and fallback to body replay. The tip, roots, safe-mode
state and proof availability remain unchanged after the refused field disconnect.
Fixing only the first metadata lookup would not establish full reorg safety.

The successful registered regression covers forward replay, proofs and restart;
it does not claim snapshot-only sparse-checkpoint disconnect/reconnect coverage.
That failure and its probe are preserved separately for the next recovery change.
Also outstanding: Linux qualification, rebuilt iOS NodeCore/device qualification,
and replay beyond the preserved bodies to the first reported height 112485.
Promoted-snapshot lifecycle proof serving and concurrent proof/reorg stress are
not qualified by this active-snapshot fixture. No phone reset, seed deployment,
release activation or column-family migration is authorized by these results.

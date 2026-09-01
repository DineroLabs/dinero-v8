# Snapshot Candidate Datadir Compatibility — 2026-09-01

## Result

`codex/snapshot-candidate-selection` at `ee284ffa2` must not be used as a
release source. Its snapshot lifecycle change is already represented by the
later merged commit `bd494024c`; the old branch does not contain current
shielded persisted-state readers.

## Reproduction

The candidate daemon was built from its exact commit and started against an
offline APFS copy-on-write clone of a current mainnet datadir. The live datadir
and wallet were never opened by the candidate.

The candidate exited during chainstate initialization at tip 100,036:

```text
ChainDB anchor history blob rejected (code=2); falling back to flat file
shielded anchor history ChainDB blob missing/invalid at/past the epoch reset
height 61000 (tip=100036); refusing the stale pre-reset flat file
Failed to initialize shielded pool state
Failed to initialize Chainstate
```

The datadir contains the v2 anchor-history persistence envelope introduced by
`751fb2461c`. The candidate has only the v1 decoder. Teaching the stale branch
to parse the envelope alone would still omit the matching eviction, rollback,
restart, and reorg semantics, so the safe resolution is to cut releases from a
current branch containing the complete change.

## Permanent release guard

`scripts/check_release_lineage.sh` now rejects a release candidate unless the
mandatory shielded persistence commit is in its ancestry. Local release build
and preflight scripts invoke the guard. macOS and Windows release workflows
fetch full ancestry and run it before building artifacts.

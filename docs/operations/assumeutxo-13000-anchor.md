# Dinero h=13000 Anchor and AssumeUTXO Snapshot

Status: implementation guardrails landed; snapshot artifact generation is a release step.

## Anchor Values

Height: `13000`

Block hash:

```text
0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3
```

Utreexo root:

```text
eca67bc825cadefab2561f48e82a00342016d1f3ad905bb277283d38de0bd54c
```

Chainwork:

```text
0x000000000000000000000000000000000000000000000000000001198ed06efa
```

Snapshot SHA256:

```text
04afcb937b07ccab469dd6ade5151cd06431b30111d813c4392303cc7b1b2426
```

UTXO count: `38700`

Snapshot bytes: `4775358`

Undo preflight:

```text
--rebuild-undo-range=1:13000
final_status=dry_run_complete
already_ok=13000
holes=0
verify_failed=0
missing_metadata=0
blocked=0
```

Verified on 2026-05-03 against Mac, Dell, CN, LA, VA, and MO.

## What This Anchor Does

The h=13000 block hash is a hard checkpoint. A syncing node that is offered a
different block at height 13000 must reject that chain. This protects new nodes
from accidentally joining a fake early fork when peer discovery is still small.

The same checkpoint is also wired into multi-peer header sync, so bad headers are
rejected before full block replay.

## What AssumeUTXO Adds

The checkpoint protects chain identity. AssumeUTXO is the fast-bootstrap layer
on top of that anchor.

Dinero snapshot format v3 includes:

- the full consensus UTXO set at the anchor block
- the serialized Utreexo forest
- the Utreexo root binding, checked against the anchor block header
- the snapshot file SHA256, which is pinned in `AssumeUTXORegistry`

Undo data is not part of the UTXO snapshot format. Undo is archival disconnect
material in `rev*.dat` plus ChainDB metadata. For release hygiene, the snapshot
manifest records an undo audit over the anchor history before the snapshot is
blessed. Full archival nodes still keep full block, undo, and Utreexo data on
disk; AssumeUTXO bootstrappers use the pinned state and validate history in the
background.

## Release Procedure

Use a disposable datadir copy, not a live node datadir.

1. Stop one staging node briefly and copy its datadir, or restore a verified
   backup into a disposable path.
2. Start a private daemon on that copy with networking disabled:

```bash
dinerod \
  --datadir=/tmp/dinero-anchor-13000 \
  --rpcport=31998 \
  --p2pport=31999 \
  --p2p.offline=1
```

3. Generate the fixed-height snapshot:

```bash
DINERO_DATADIR=/tmp/dinero-anchor-13000 \
DINERO_RPCPORT=31998 \
scripts/assumeutxo/generate_anchor_snapshot.sh \
  13000 \
  0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3 \
  /tmp/dinero-snapshots
```

The script rewinds only the disposable copy by invalidating h=13001, verifies the
h=13000 hash, runs the tail undo metadata audit, exports the v3 UTXO+Utreexo
snapshot, writes a manifest, and prints the exact `AssumeUTXOSnapshot(...)`
registry entry.

The tail undo audit defaults to 1024 blocks because `auditundometadata` is a
tip-safety check. A full 1..13000 historical undo proof must use the offline
`--rebuild-undo-range=1:13000` manifest; do not stretch the RPC tail audit into a
whole-history proof.

For the first h=13000 snapshot, the offline undo preflight was run against the
same disposable LA datadir copy after the snapshot export. It reported
`already_ok=13000`, `holes=0`, `verify_failed=0`, `missing_metadata=0`, and
`blocked=0`.

4. Add that printed registry entry to `src/consensus/assume_utxo.cpp`.
5. Rebuild and run the AssumeUTXO lifecycle tests.
6. Publish both files with the release:

- `utxo-snapshot-13000.dat`
- `utxo-snapshot-13000.manifest.json`

7. Sign the release checksums with the Dinero Core release key.

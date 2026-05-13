# Dell Missing-Metadata Recovery

- **Date:** 2026-05-02
- **Host:** Dell Tower (`tower@192.168.1.108`)
- **Range:** `10784..12145`
- **Repair binary:** `2e15b5e4a`
- **Status:** Repaired. Metadata recovery + undo rebuild both green.
- **Backup:** `/home/tower/dell-metadata-undo-prewrite-20260502-133351`

## Summary

Dell's H.3 undo rebuild was blocked by 63 heights with missing
chaindb metadata. These rows were worse than stripped `BLOCK_HAVE_UNDO`
bits: the canonical height index and block metadata were absent, so
the undo rebuilder correctly refused to write.

Commit `2e15b5e4a` extended offline header-metadata recovery to rebuild
missing height-index rows from `blk*.dat` by walking the parent chain.
After deploying that binary to Dell, the dry-run found exactly the
expected 63 recoverable rows. The write pass restored all 63 rows, and
the undo rebuild then completed across the full window.

## Evidence

### Metadata recovery dry-run

Manifest:
`/home/tower/.dinero/recover_header_metadata_dell_10784_12145.dry2.json`

| Metric | Value |
|---|---:|
| `final_status` | `dry_run_complete` |
| `scanned` | 1362 |
| `already_ok` | 1299 |
| `recoverable` | 63 |
| `recovered` | 0 |
| `failed` | 0 |

### Metadata recovery write

Manifest:
`/home/tower/.dinero/recover_header_metadata_dell_10784_12145.write.json`

| Metric | Value |
|---|---:|
| `final_status` | `ok` |
| `scanned` | 1362 |
| `already_ok` | 1299 |
| `recoverable` | 63 |
| `recovered` | 63 |
| `failed` | 0 |

### Undo rebuild

Manifest:
`/home/tower/.dinero/rebuild_undo_dell_10784_12145.json`

| Metric | Value |
|---|---:|
| `final_status` | `ok` |
| `already_ok` | 1 |
| `rebuilt` | 1361 |
| `verify_failed` | 0 |
| `holes` | 0 |
| `skipped` | 0 |
| `missing_metadata` | 0 |
| `blocked` | 0 |

First repaired height: `10784`.
Last repaired height: `12145`.

## Post-repair state

Dell restarted on:

```text
/home/tower/Dinero-Coin/build/dinerod -datadir=/home/tower/.dinero
```

RPC verification:

| Check | Result |
|---|---|
| Tip height | `12145` |
| Tip hash | `000000638d78a9d6f99d73f3e4ad7702728053c06520df07985ef0ad4413a027` |
| `health.checks.tip_undo_present` | `true` |
| `health.checks.peer_count` | `4` |
| `health.checks.safemode_active` | `false` |
| `health.checks.fatal_in_last_5min` | `0` |

`dinero-cli health` still returns `FAILING` because the visible fleet
tip is stale by policy (`tip > 2h stale`). That is not undo-repair
damage; the repair-specific undo check is green.

## Verdict

Dell's separate missing-metadata incident is closed for range
`10784..12145`. The metadata recovery pass restored the 63 missing
height-index/header rows, and the undo rebuilder then restored 1361
undo holes with zero verifier failures.

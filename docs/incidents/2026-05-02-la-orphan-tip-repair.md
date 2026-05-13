# LA Orphan-Tip Undo Repair

- **Date:** 2026-05-02
- **Host:** LA (`root@172.93.160.131`)
- **Range:** `10784..12145`
- **Repair binary:** `2e15b5e4a`
- **Status:** Repaired. Metadata was intact; undo rebuild green.
- **Backup:** `/root/la-orphan-tip-prewrite-20260502-110856`

## Summary

LA was tracked separately from Dell because earlier diagnostics labeled
it as chaindb-orphan-tip damage: the node served the tip, but historical
metadata/undo state for the post-10783 range needed independent proof.

The 2026-05-02 metadata dry-run showed the important distinction:
LA did **not** have Dell-style missing metadata in this range. All 1362
heights were already present in the height/header metadata tables. The
metadata write pass was therefore a clean no-op, and the actual repair
was the undo rebuild.

The undo rebuilder found 243 holes, rebuilt all of them, and ended with
zero verifier failures.

## Evidence

### Metadata recovery dry-run

Manifest:
`/root/.dinero/recover_header_metadata_la_10784_12145.dry.json`

| Metric | Value |
|---|---:|
| `final_status` | `dry_run_complete` |
| `scanned` | 1362 |
| `already_ok` | 1362 |
| `recoverable` | 0 |
| `recovered` | 0 |
| `failed` | 0 |

### Metadata recovery write

Manifest:
`/root/.dinero/recover_header_metadata_la_10784_12145.write.json`

| Metric | Value |
|---|---:|
| `final_status` | `ok` |
| `scanned` | 1362 |
| `already_ok` | 1362 |
| `recoverable` | 0 |
| `recovered` | 0 |
| `failed` | 0 |

This write was intentionally run as a gate, but it made no metadata
changes because there were no recoverable metadata rows.

### Undo rebuild

Manifest:
`/root/.dinero/rebuild_undo_la_10784_12145.json`

| Metric | Value |
|---|---:|
| `final_status` | `ok` |
| `already_ok` | 1119 |
| `rebuilt` | 243 |
| `verify_failed` | 0 |
| `holes` | 0 |
| `skipped` | 0 |
| `missing_metadata` | 0 |
| `blocked` | 0 |

Window:

| Field | Value |
|---|---|
| `window_start` | `10784` |
| `window_end` | `12145` |
| `anchor_height` | `0` |
| `anchor_hash` | `0000000000000000000000000000000000000000000000000000000000000000` |

## Post-repair state

LA restarted through `systemd`:

```text
/root/Dinero-Coin/build/dinerod --datadir /root/Dinero-Coin/data-main --listen --port 20999 --rpc --rpcport 20998 --rpcbind=0.0.0.0 --rpcuser=dinero --rpcpassword=dinerodpi2026 --utreexo-bridge=1
```

RPC verification:

| Check | Result |
|---|---|
| `systemctl is-active dinerod` | `active` |
| Tip height | `12145` |
| Tip hash | `000000638d78a9d6f99d73f3e4ad7702728053c06520df07985ef0ad4413a027` |
| `health.checks.tip_undo_present` | `true` |
| `health.checks.peer_count` | `4` |
| `health.checks.safemode_active` | `false` |
| `health.checks.fatal_in_last_5min` | `0` |

`dinero-cli health` still returns `FAILING` because the visible fleet
tip is stale by policy (`tip > 2h stale`). That is separate from this
repair; the undo-specific check is green.

## Verdict

LA's separate orphan-tip/undo incident is closed for range
`10784..12145`. Metadata was already present, the metadata write gate
was a no-op, and the undo rebuilder restored 243 holes with zero
verifier failures.

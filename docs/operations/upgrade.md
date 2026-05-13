# Dinero Core Upgrade Procedure

**Status:** drafted 2026-05-02. Owner: project (solo operator).

This document is the canonical upgrade procedure for a Dinero Core
node. It describes commands that **actually exist** in the tree
(Phase B–D.4 of the Core 1.0 plan); commands that are still pending
(notably `apt install dinero-core=<version>` from Phase E) are
clearly flagged as future.

The contract this procedure honors is locked at
[`docs/specs/dinero-core-1.0.md`](../specs/dinero-core-1.0.md)
Standard 11. Any deviation in this doc from the spec is a doc bug,
not a procedure choice.

---

## Two modes, one shape

Dinero Core 1.0 ships in two installation modes (spec §1.1, §1.2).
The upgrade procedure has the same six steps in both modes; only
the install verbs differ.

| Step | Packaged-service mode | Manual-user mode |
|------|----------------------|------------------|
| 1. Health pre-flight     | `dinero-cli health`                              | same |
| 2. Backup wallet         | `dinero-backup --datadir=… --output=…`           | same |
| 3. Capture current binary | `dinero-prepare-upgrade --datadir=…`            | same |
| 4. Install new version   | `apt install dinero-core=<version>`  *(Phase E)* | `git pull && cmake --build build && cmake --install build --prefix=/usr/local` |
| 5. Restart daemon        | systemd auto-restarts                            | `sudo systemctl restart dinero` (or your supervisor's equivalent) |
| 6. Verify + soak         | `dinero-cli health` returns OK; soak ≥1h         | same |

Steps 1–3 are recovery preparation (state snapshots before any
write). Step 4 is the only step that actually replaces binaries.
Step 5–6 verify the new binary serves the chain correctly.

Soak ≥1h between upgrades is mandatory on production fleet. A
30-second smoke is acceptable in dev/test only.

---

## Step 1 — Health pre-flight

Refuse to start an upgrade against an unhealthy node. If the node
is already DEGRADED or FAILING for unrelated reasons, fix that
first; an upgrade in the middle of a separate problem doubles the
recovery surface.

```
dinero-cli health
```

Expected: `OK` and exit code 0.

If `DEGRADED <reason>`: investigate the reason first (low peers,
stale tip, recent FATAL). Resolve it, then re-run health. Do not
proceed to Step 2 with a degraded node.

If `FAILING <reason>`: rollback territory. `safemode active` /
`tip undo data missing` / `tip > 2h stale` mean this node should
not be upgraded — it should be repaired or rolled back to a known
good binary first.

---

## Step 2 — Backup wallet + shielded material

Wallet seeds and shielded note material are irrecoverable from the
network. Block + chaindb data is replay-able; back up the
irrecoverable subset only.

```
dinero-backup --datadir=<path> --output=/var/backups/dinero/wallet-$(date +%Y%m%dT%H%M%SZ).tar.gz
```

What gets backed up (declared targets only, hardcoded include list):

```
<datadir>/wallets/
<datadir>/hd_wallet/
<datadir>/blockchain/shielded_frontier.bin
<datadir>/blockchain/shielded_anchor_history.bin
<datadir>/blockchain/shielded_nullifiers.db/
```

The archive is mode `0600`, contains a `manifest.txt` with sha256s
of every file, and is integrity-verified before the final filename
appears. If `dinero-backup` exits non-zero, **do not proceed**.

To verify a backup any time after creation:

```
tar -xzf <archive> -C <verify-dir>
awk -F'\t' '/^[^[#]/ {print $3"  "$1}' <verify-dir>/manifest.txt | \
    (cd <verify-dir> && sha256sum -c)
```

---

## Step 3 — Capture current binary (rollback target)

Capture the running daemon's `/proc/<pid>/exe` to
`<datadir>/binaries/dinerod.live-pre-<commit>-<ts>` so a rollback
can copy back to the install path with one command.

```
dinero-prepare-upgrade --datadir=<path>
```

Expected: exit 0 and a line like

```
✅ rollback binary captured: <datadir>/binaries/dinerod.live-pre-<commit>-<ts>
```

If dinerod is not running (e.g., already stopped for some other
reason), the command exits 0 with a WARN and produces no file. That
is the non-fatal contract: a missing capture does not block the
upgrade. If you want a guaranteed capture, ensure the daemon is
running before this step, or pass `--exe=<path>` pointing at the
on-disk install of the current binary.

`<datadir>/binaries/` is mode `0750` and the captured binary is
mode `0750`. Existing captures are never touched by a fresh run.

---

## Step 4 — Install the new version

### Packaged-service mode (Phase E — pending)

```
sudo apt install dinero-core=<new-version>
```

The `.deb` postinst replaces `/usr/bin/dinerod` and signals systemd.
This path is **NOT YET AVAILABLE** — Phase E ships the `.deb`. Until
then, packaged-mode operators should follow the manual-mode steps
against `/usr/bin/`.

### Manual-user mode

```
cd ~/src/dinero
git fetch origin p2p-fix
git checkout <new-commit-or-tag>
cmake --build build --target dinerod dinero-cli -j$(nproc)
sudo cmake --install build --prefix=/usr/local   # or your existing PREFIX
```

Verify the new binary's commit before restart:

```
/usr/local/bin/dinerod --version | head -1
```

Should print `dinerod <new-short-commit>` matching the commit you
checked out.

---

## Step 5 — Restart the daemon

### Packaged-service mode

`apt install` triggers systemd to restart automatically. Verify:

```
systemctl is-active dinero          # expect: active
systemctl status dinero | head -10
```

### Manual-user mode

```
sudo systemctl restart dinero       # if you ship a unit
# or, if running under nohup / your own supervisor:
<your supervisor's restart command>
```

Daemon takes 3–5 minutes on a populated chain to replay the utreexo
forest and reopen RPC. During that window, `dinero-cli` calls return
"connection refused" — that's expected, not a failure.

---

## Step 6 — Verify + soak

Wait for RPC to come up, then:

```
dinero-cli health           # expect: OK   (exit 0)
```

If `OK`: start the soak window. **Do not declare the upgrade done
for at least 1 hour.** During the soak:

- `dinero-cli getblockcount` should match the rest of the fleet
  (or remain consistent with the rest of the network for an
  external operator).
- `dinero-cli health --json | jq .checks.peer_count` should be ≥ 3.
- `journalctl -u dinero --since "5 minutes ago"` should have zero
  ERROR / FATAL lines.

If `DEGRADED <reason>`: the upgrade may have left the daemon in a
slow-recovery state. Wait. Most degraded states (`tip > 30min stale`,
`peer count below threshold`) clear themselves within 5–15 minutes.
Re-check at 5, 15, 60 minutes. If still DEGRADED at 60 minutes,
treat as failed upgrade — see rollback below.

If `FAILING <reason>`: rollback immediately.

---

## Rollback

Two rollback paths, both quick. Pick based on which install mode
you used.

### Packaged mode (Phase E — pending)

```
sudo apt install dinero-core=<old-version>
sleep 30
dinero-cli health           # expect: OK
```

### Manual mode (works today)

```
sudo systemctl stop dinero
sudo cp <datadir>/binaries/dinerod.live-pre-<commit>-<ts> /usr/local/bin/dinerod
sudo systemctl start dinero
sleep 30
dinero-cli health           # expect: OK
```

If health still fails after rollback: this isn't an upgrade
problem — the underlying chain state is in a bad state independent
of the binary. Capture journal output, stop, and diagnose
chainstate (likely candidates: phantom-undo at tip → run
`dinero-cli health --json` to confirm; partial chaindb corruption
→ candidate for `--reindex-chainstate` on a sandbox copy à la the
Apr 30 fleet repair).

If rollback brings up the old binary cleanly but the chain is
ahead of where the old binary expects: re-rolling forward to the
new binary is fine; the issue may be transient. If health stays
FAILING on the new binary specifically, file the failure and stay
on the old binary until a fix lands.

---

## Troubleshooting

### `dinero-backup` fails with "no irrecoverable wallet material"

The datadir doesn't have `wallets/` or `hd_wallet/`. Either
- The datadir argument is wrong (check the running daemon's
  `--datadir` flag).
- The wallet was never created (fresh node).

In the second case, there's nothing to back up — proceed to Step 3.
But verify the operator intent first; "I'm upgrading a node I
thought had wallets" → wrong datadir, fix that.

### `dinero-prepare-upgrade` exits 0 but produced no file

Means dinerod was not running at capture time. The non-fatal
contract is honored. If you want a capture anyway:

```
dinero-prepare-upgrade --datadir=<path> --exe=/usr/local/bin/dinerod
```

The `--exe` override captures the on-disk binary directly without
needing `/proc/<pid>/exe`.

### Health stays DEGRADED with `peer count below threshold`

The new binary may have a P2P-protocol-version mismatch with the
rest of the network, OR the rest of the fleet hasn't been upgraded
yet. Cross-check a peer's `dinero-cli getpeerinfo` output. If peers
are present but the new binary refuses them: rollback.

### Health says FAILING with `tip undo data missing`

This is the Apr 30 phantom-undo class. The new binary's local chain
state is missing undo bytes at the tip. Root cause is not the
upgrade itself — the chain state was already broken pre-upgrade.
Repair via `dinero-prepare-upgrade --datadir=…` then a sandbox
`--rebuild-undo-range` pass per the Apr 30 incident playbook
([docs/incidents/2026-04-30-undo-rebuilder.md](../incidents/2026-04-30-undo-rebuilder.md)).

### Cookie auth fails after upgrade

The new binary regenerates `.cookie` on startup. If you're a
member of the `dinero` group on a packaged install, this is
transparent. If you got "Unauthorized" from `dinero-cli` on a
manual install:

```
ls -la <datadir>/.cookie
# Should be -rw-r----- owned dinero:dinero (or your run-user)
# If world-readable or wrong owner, the daemon may have been
# started under a different user. Stop and restart cleanly.
```

### Multiple `dinero.live-pre-*` files piling up in binaries/

Expected — captures don't auto-prune. After a successful soak you
can delete captures older than the last known-good rollback target.
Keep at least the most recent two so a "rollback the rollback"
path exists.

---

## Sequence summary (single block, copy-paste-able)

For the manual-user mode happy path on Linux:

```
DATADIR=/var/lib/dinero
BACKUP="/var/backups/dinero/wallet-$(date -u +%Y%m%dT%H%M%SZ).tar.gz"

# 1. Pre-flight
dinero-cli health           || { echo "node unhealthy; abort"; exit 1; }

# 2. Backup
dinero-backup --datadir="${DATADIR}" --output="${BACKUP}"

# 3. Capture rollback binary
dinero-prepare-upgrade --datadir="${DATADIR}"

# 4. Install new version
cd ~/src/dinero && git pull && \
    cmake --build build --target dinerod dinero-cli -j$(nproc) && \
    sudo cmake --install build --prefix=/usr/local

# 5. Restart
sudo systemctl restart dinero
sleep 60                    # forest replay + RPC reopen window

# 6. Verify
dinero-cli health           # expect: OK
echo "Soak now ≥1h before declaring done."
```

If any step fails, do **not** continue. Diagnose, fix, restart from
the failed step.

---

## Decision log

- **2026-05-02:** Drafted. Source: docs/specs/dinero-core-1.0.md
  Standard 11; commands shipped in commits f6ae0e7bf (Phase B
  config loader), 89aae4826 (Phase C install), 5a5ccd868
  (Phase D.4 health), e3b3255d1 (Phase D.2 backup), 38b33aa29
  (Phase D.3 prepare-upgrade). Phase E (`apt install`) flagged as
  pending throughout. Rollback decision tree includes the Apr 30
  phantom-undo class as an explicit FAILING signature.

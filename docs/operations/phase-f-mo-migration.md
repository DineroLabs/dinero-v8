# Phase F — MO migration to packaged-service mode

**Status:** runbook drafted 2026-05-02. **MO has NOT been migrated
yet.** This document is the explicit, rollback-safe procedure that
must be followed in full when the operator is ready.

**Scope:** MO only. CN / LA / VA / Dell are migrated as a separate
sequence after MO has soaked clean for ≥24 h. Half-fleet states
(some packaged, some manual) are deliberate during the soak —
that's how we catch incompatibilities without staking the whole
fleet.

**Hard rule: install from the published GitHub release asset
(`v2.2.5-rc1`), not from a local build.** The point of Phase F is
to prove the release path works for an external operator. Any
shortcut — `scp`-ing a .deb from VA, building locally on MO,
installing from a private CI artifact — defeats the test.

---

## Why this is non-trivial

MO has a known-stable manual fleet config that DineroDPI iOS
depends on:

- Daemon: `/root/Dinero-Coin/build/dinerod`, run as `root`
- Datadir: `/root/.dinero/`
- Service unit: `/etc/systemd/system/dinerod.service`
- RPC: `127.0.0.1:20998`, basic auth `rpcuser=dinero rpcpassword=dinerodpi2026`
- DineroDPI iOS hits this RPC; switching auth schemes breaks the app

The packaged service has different defaults:

- Daemon: `/usr/bin/dinerod`, run as `dinero` system user
- Datadir: `/var/lib/dinero/`
- Service unit: `/lib/systemd/system/dinero.service` (different name!)
- RPC: cookie auth at `/var/lib/dinero/.cookie` by default

The migration **may need to preserve the existing RPC user/password**
so DineroDPI doesn't break — but only if DineroDPI actually needs
them. Cookie auth + localhost bind is strictly more secure and is
the packaged-service default; we only widen that surface if
there's a current dependency. See the decision matrix in
"Configure for DineroDPI continuity" below.

The two units (`dinerod.service` and `dinero.service`) coexist by
name — that's a feature, not a bug, because it lets us start the
new one only after stopping the old one, with no overlap.

---

## Preflight (read-only, no changes)

Capture state so rollback is mechanical, not detective work.

```bash
ssh root@72.18.214.120  # MO
mkdir -p /root/mo-migration-snapshot-$(date -u +%Y%m%dT%H%M%SZ)
cd /root/mo-migration-snapshot-*

# 1. Pre-cutover identity capture — MUST match exactly after the
#    new daemon starts. These three values are the cutover proof.
#    -datadir is explicit so the cli reads the cookie / pid the
#    OLD daemon owns; this matters once /var/lib/dinero/ exists too.
systemctl is-active dinerod > unit-active.before.txt
/root/Dinero-Coin/build/dinero-cli -datadir=/root/.dinero \
    -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 \
    getblockcount > tip-height.before.txt
/root/Dinero-Coin/build/dinero-cli -datadir=/root/.dinero \
    -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 \
    getbestblockhash > tip-hash.before.txt
cd /root/Dinero-Coin && git rev-parse HEAD > /root/mo-migration-snapshot-*/build-commit.txt
cd -

echo "═══ pre-cutover snapshot ═══"
echo "unit-active: $(cat unit-active.before.txt)"
echo "tip-height:  $(cat tip-height.before.txt)"
echo "tip-hash:    $(cat tip-hash.before.txt)"
echo "═══════════════════════════"

# 2. Daemon paths and service state
which dinerod 2>&1 > daemon-path.txt
systemctl is-enabled dinerod > unit-enabled.txt
systemctl status dinerod 2>&1 | head -20 > unit-status.txt
cp /etc/systemd/system/dinerod.service unit.service.snapshot

# 3. Disk space (need ≥10 GB free for atomic datadir copy)
df -h /var /root > diskspace.txt
echo "Datadir size:" >> diskspace.txt
du -sh /root/.dinero >> diskspace.txt

# 4. Wallet/datadir/config backup — REAL backup, not just a snapshot
#    Use the dinero-backup script if present; otherwise tar.
if command -v dinero-backup >/dev/null 2>&1; then
    dinero-backup --datadir /root/.dinero --output backup-pre-phase-f.tar.gz
else
    # Stop daemon BEFORE tarring to get a consistent snapshot
    systemctl stop dinerod
    tar -czf backup-pre-phase-f.tar.gz -C /root .dinero
    systemctl start dinerod
fi
sha256sum backup-pre-phase-f.tar.gz > backup-pre-phase-f.sha256
ls -lh backup-pre-phase-f.tar.gz

# 5. Current binary capture (rollback target)
cp /root/Dinero-Coin/build/dinerod dinerod.live-pre-migration
sha256sum dinerod.live-pre-migration > dinerod.sha256

# 6. Print snapshot summary
echo "═══ MO migration snapshot ═══"
cat tip-height.txt tip-hash.txt build-commit.txt unit-active.txt diskspace.txt
ls -lh
```

**Pass criteria:**
- `tip-height.txt` is a number ≥ current fleet tip
- `unit-active.txt` says `active`
- `df` shows ≥10 GB free on `/var` and `/root`
- `backup-pre-phase-f.tar.gz` exists, size matches the datadir
- `dinerod.live-pre-migration` exists

**Hard fail:** any of the above missing → STOP. Don't migrate a
node you can't roll back.

---

## Verify release artifact (offline-equivalent)

Done **on MO** before any system changes — this proves the binary
arrived intact, signed by our key, before we install it.

```bash
mkdir -p /root/dinero-rc-incoming
cd /root/dinero-rc-incoming

# 1. Download all four release files from the v2.2.5-rc1 GitHub release
BASE=https://github.com/DineroLabs/dinero-releases/releases/download/v2.2.5-rc1
for f in dinero-core_2.2.5-1_amd64.deb dinero-core-release.asc SHA256SUMS SHA256SUMS.asc; do
    curl -sLO "$BASE/$f" || { echo "DOWNLOAD FAILED: $f" >&2; exit 1; }
done
ls -lh

# 2. Import the release-signing key into a FRESH gpg home
#    (avoids any pre-existing trust/keyring state)
export GNUPGHOME=$(mktemp -d)
chmod 700 "$GNUPGHOME"
gpg --import dinero-core-release.asc

# 3. Confirm the fingerprint matches the published one EXACTLY
gpg --fingerprint "Dinero Core Release Signing"
echo
echo "═══ EXPECTED FINGERPRINT ═══"
echo "    4ED3 65CE 6604 B722 D281  EC77 3A61 4979 B8A4 8C02"
echo "═══ STOP if it doesn't match ═══"

# 4. Verify signature on SHA256SUMS
gpg --verify SHA256SUMS.asc SHA256SUMS
# MUST print: gpg: Good signature from "Dinero Core Release Signing"

# 5. Verify checksum
sha256sum -c SHA256SUMS
# MUST print: ./dinero-core_2.2.5-1_amd64.deb: OK

# 6. Belt-and-suspenders: also run the package-contract gate
sudo apt-get install -y binutils python3
# Pull a copy of the verifier from the source repo
curl -sLO https://raw.githubusercontent.com/DineroLabs/Dinero-Coin/p2p-fix/share/scripts/dinero-deb-verify
chmod +x dinero-deb-verify
./dinero-deb-verify --static dinero-core_2.2.5-1_amd64.deb
# MUST print: PASS: dinero-deb-verify — all hard contract checks green.

# Cleanup the throwaway gpg home
rm -rf "$GNUPGHOME"
unset GNUPGHOME
```

**Pass criteria:** all 6 commands clean. Specifically:
- Fingerprint matches `4ED3 65CE 6604 B722 D281  EC77 3A61 4979 B8A4 8C02`
- `Good signature from "Dinero Core Release Signing"`
- `dinero-core_*.deb: OK`
- `dinero-deb-verify --static`: `PASS`

**Hard fail:** any check fails → DO NOT INSTALL. Stop the
migration; investigate. The .deb sitting in `/root/dinero-rc-incoming/`
is harmless until you `dpkg -i` it.

---

## Install (no auto-start)

The .deb's postinst is non-destructive: creates the `dinero` user,
seeds `/etc/dinero/dinero.conf`, ships the systemd unit. It does
**NOT** auto-enable or auto-start (verified by Phase E.3 gate 6).
The old `dinerod.service` is also untouched at this point —
both units coexist, only the old one is running.

```bash
cd /root/dinero-rc-incoming
sudo dpkg -i dinero-core_2.2.5-1_amd64.deb

# Confirm install landed everything where expected
ls -la /usr/bin/dinerod /usr/bin/dinero-cli
ls -la /lib/systemd/system/dinero.service /usr/lib/systemd/system/dinero.service 2>&1 | head -2
ls -la /var/lib/dinero/                         # owned dinero:dinero, mode 0750
ls -la /etc/dinero/dinero.conf                  # owned root:dinero, mode 0640
getent passwd dinero                            # /usr/sbin/nologin shell

# Confirm new unit is NOT enabled and NOT running yet
systemctl is-enabled dinero || true             # disabled
systemctl is-active  dinero || true             # inactive

# Confirm OLD unit is still running (we haven't touched it yet)
systemctl is-active dinerod                     # active
```

**Pass criteria:**
- `/usr/bin/dinerod` exists, mode 0755
- `dinero` user exists with `nologin` shell
- `/var/lib/dinero/` exists, mode `0750`, owner `dinero:dinero`
- `/etc/dinero/dinero.conf` exists, owner `root:dinero`, mode `0640`
- New unit reports `disabled` + `inactive`
- Old unit reports `active`

**Hard fail rollback:** If any of the above is wrong (e.g. the new
unit is somehow active, the dinero user wasn't created, the conf
seeded incorrectly):
```bash
sudo apt-get purge -y dinero-core           # postrm wipes new state
# Old daemon was untouched, so MO is back to its pre-install state
```

---

## Configure for DineroDPI continuity (CONDITIONAL — least-privilege default)

**Default:** the packaged daemon uses cookie auth at
`/var/lib/dinero/.cookie` and binds RPC to localhost only. That is
strictly more secure than static rpcuser/rpcpassword, and it is
sufficient if no off-host client needs to reach the RPC.

**Only patch `/etc/dinero/dinero.conf`** with rpcuser/rpcpassword
if there is an actual current dependency. The operator's
checklist BEFORE editing:

1. Open the iOS DineroDPI app. Does it currently connect to MO?
2. If yes — does it use static credentials, or has it been moved
   to cookie auth / a different node?
3. Is MO meant to be reachable over the network, or only by
   processes running ON MO (in which case `rpcbind=127.0.0.1` is
   correct and `rpcallowip` is irrelevant)?

**Decision matrix:**

| DineroDPI needs MO? | Off-host access? | Configure |
|---|---|---|
| No | (n/a) | **Don't patch.** Keep cookie auth, default localhost bind. |
| Yes — local-only | No | Add rpcuser/rpcpassword/rpcport, keep `rpcbind=127.0.0.1`, no rpcallowip. |
| Yes — off-host (DPI not on MO) | Yes | Add rpcuser/rpcpassword/rpcport AND deliberate `rpcbind=<MO LAN IP>` AND specific `rpcallowip=<DPI IP>`. NEVER `rpcbind=0.0.0.0` — that exposes RPC to the public network. |

**Never** broaden `rpcbind` to `0.0.0.0` "for nostalgia." The
packaged-service contract is least-privilege by default; widening
the bind is a security trade-off that requires a current
justification, not a habit from the manual-fleet era.

If you do need to patch (case 2 or 3 above):

```bash
sudo tee /etc/dinero/dinero.conf > /dev/null <<'EOF'
# Dinero Core packaged-service config — MO node.
# Phase F migration. Preserves DineroDPI iOS RPC contract.

# RPC: same credentials + port DineroDPI has been using
rpcuser=dinero
rpcpassword=dinerodpi2026
rpcport=20998
rpcbind=127.0.0.1               # localhost only — see decision matrix above
# rpcallowip=...                # uncomment + restrict only if off-host access required

# P2P: default port (20999), seed nodes already in dinero.conf.example
EOF

sudo chown root:dinero /etc/dinero/dinero.conf
sudo chmod 0640 /etc/dinero/dinero.conf
ls -la /etc/dinero/dinero.conf  # MUST be 0640 root:dinero
```

**Pass criteria** (only when patching):
- mode `0640`, owner `root:dinero`
- `rpcbind` is `127.0.0.1` UNLESS off-host access was deliberately
  decided (decision matrix row 3)
- `rpcallowip` is absent OR a specific IP, never `0.0.0.0/0`
- contents include `rpcuser=dinero` and `rpcpassword=dinerodpi2026`

**Skipping this section is the safest pass criterion:** if cookie
auth covers DineroDPI's actual need, `/etc/dinero/dinero.conf` can
remain at the postinst-seeded default (just `addnode=` lines) and
no static-password attack surface is opened.

**Hard fail rollback:** if perms wrong, `chmod`/`chown` again. If
content wrong, re-edit. If you patched in error and want to revert
to cookie auth, restore the postinst-seeded default with:
```bash
sudo cp /usr/share/doc/dinero/dinero.conf.example /etc/dinero/dinero.conf
sudo chown root:dinero /etc/dinero/dinero.conf
sudo chmod 0640 /etc/dinero/dinero.conf
```
(if the example was gzipped by dh_compress, `zcat` it first).

---

## Data migration (atomic, with rollback preserved)

This is the highest-risk step. Approach: **copy** the datadir
(don't move it) so the original at `/root/.dinero/` is preserved
as a rollback. Only delete the original after MO has soaked clean
for ≥24 h on the packaged service.

```bash
# 1. Stop the OLD daemon — datadir must not be live-written during copy
sudo systemctl stop dinerod
# Wait for the daemon to fully shut down (avoid corrupting RocksDB)
while pgrep -f /root/Dinero-Coin/build/dinerod >/dev/null; do
    echo "waiting for dinerod to exit..."
    sleep 2
done

# 2. Copy datadir → /var/lib/dinero/
#    --reflink=auto is fast on btrfs/xfs; falls back to plain copy on ext4.
#    /var/lib/dinero/ is currently empty (just created by postinst).
sudo cp -a --reflink=auto /root/.dinero/. /var/lib/dinero/

# 3. Fix ownership — postinst created the dir as dinero:dinero,
#    but the copied files come in with their original (root) ownership.
sudo chown -R dinero:dinero /var/lib/dinero/

# 4. Fix top-level mode (cp -a may have widened it from postinst's 0750)
sudo chmod 0750 /var/lib/dinero/

# 5. Sanity check
sudo -u dinero ls -la /var/lib/dinero/ | head -10
sudo -u dinero stat -c '%a %U:%G' /var/lib/dinero/
du -sh /root/.dinero /var/lib/dinero/  # should be the same size
```

**Pass criteria:**
- `/var/lib/dinero/` size matches `/root/.dinero/` size (within rounding)
- All files in `/var/lib/dinero/` owned `dinero:dinero`
- Top-level mode `750`
- `dinero` user can `ls` the dir without sudo (proves group membership works)

**Hard fail rollback:**
```bash
sudo rm -rf /var/lib/dinero/*           # nuke the bad copy
sudo systemctl start dinerod            # bring old daemon back up
# Investigate; retry copy; or abort migration entirely.
```

---

## Start the packaged service

```bash
# 1. Disable the old unit so it doesn't fight us at boot
sudo systemctl disable dinerod
# (we leave the unit FILE in place — it's part of the rollback path)

# 2. Enable and start the packaged service
sudo systemctl enable --now dinero

# 3. Watch startup
sudo journalctl -u dinero -f --since "1 minute ago"
# Press Ctrl+C once you see "Loaded best block" / IBD/sync activity
```

**Pass criteria (within 60 seconds of start):**
- `systemctl is-active dinero` → `active`
- No `[FATAL]` or `[ERROR]` in journal
- `dinero-cli health` returns `OK` or `DEGRADED <reason>` — `FAILING` is a hard fail
  ```bash
  sudo -u dinero dinero-cli -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 health
  ```
- **Explicit before/after cutover comparison.** This is the proof
  that the new daemon adopted the existing chain state — not that
  it spun up empty or rewound:

  ```bash
  cd /root/mo-migration-snapshot-*

  # Capture the same three values the preflight captured
  systemctl is-active dinero > unit-active.after.txt
  sudo -u dinero dinero-cli -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 \
      getblockcount > tip-height.after.txt
  sudo -u dinero dinero-cli -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 \
      getbestblockhash > tip-hash.after.txt

  # Compare. tip-height MUST be ≥ before (chain may have advanced
  # while we were copying); tip-hash MUST match if before == after,
  # otherwise the new tip MUST be a successor of the old tip.
  echo "═══ before ═══"
  cat unit-active.before.txt tip-height.before.txt tip-hash.before.txt
  echo "═══ after ═══"
  cat unit-active.after.txt tip-height.after.txt tip-hash.after.txt
  echo "═══ diff ═══"
  diff -u tip-hash.before.txt tip-hash.after.txt && echo "tip-hash UNCHANGED (clean cutover)" \
      || echo "tip-hash CHANGED — verify it's a forward extension, not a rewind"
  ```

  - `unit-active`: `inactive` → `active` ✓
  - `tip-height`: after ≥ before (NEVER less; a rewind is a hard fail)
  - `tip-hash`:
    - **identical** if no new blocks arrived during the migration → ideal
    - **different but height ≥ before** → confirm the new hash is a
      forward extension by walking back from the new tip:
      ```bash
      H_BEFORE=$(cat tip-height.before.txt)
      sudo -u dinero dinero-cli -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 \
          getblockhash $H_BEFORE
      ```
      That hash MUST match `tip-hash.before.txt`. If it doesn't,
      the new daemon is on a different chain — hard fail rollback.

**Hard fail rollback** (any of the above fails, OR the daemon
crashloops):

```bash
# 1. Stop the new service
sudo systemctl stop dinero
sudo systemctl disable dinero

# 2. Re-enable + start the old unit — old datadir at /root/.dinero/ is intact
sudo systemctl enable dinerod
sudo systemctl start dinerod

# 3. Confirm we're back
sudo systemctl is-active dinerod
/root/Dinero-Coin/build/dinero-cli -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 getblockcount

# 4. Check DineroDPI iOS still works (it's hitting 127.0.0.1:20998
#    either way, so the rollback is transparent to the app)
```

The new datadir at `/var/lib/dinero/` stays on disk during
rollback — gives you a sandbox to debug the migration failure
without re-doing the copy on retry. Delete it manually only after
the issue is understood.

---

## Soak (≥1 h clean before declaring success)

Watch for the next hour:

```bash
# Stream the journal in one terminal:
sudo journalctl -u dinero -f

# In another terminal, every 5 min:
sudo -u dinero dinero-cli -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 getblockcount
sudo -u dinero dinero-cli -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 getpeerinfo | jq 'length'
sudo -u dinero dinero-cli -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 health
sudo -u dinero dinero-cli -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 safemode.status 2>/dev/null
```

**Pass criteria over the soak:**
- Tip advances or matches the fleet (no stall)
- Each new height is a strict descendant of `tip-hash.before.txt`
  (the cutover proof from the Start phase) — never a fork off
  some earlier height
- ≥3 peers connected
- `health` returns `OK` consistently
- No `[FATAL]`, `[ERROR]`, `assert failed`, or `invariant` lines
- Safe mode never triggers
- DineroDPI iOS app continues to work (operator visually verifies)

**Hard fail rollback:** if anything above fails during soak, follow
the start-step rollback procedure above.

---

## Cleanup (do NOT run until ≥24 h soak clean)

After MO has been clean for at least 24 hours on the packaged
service, **and** at least one DineroDPI session has gone through
unchanged:

```bash
# 1. Verify health one more time
sudo -u dinero dinero-cli -rpcuser=dinero -rpcpassword=dinerodpi2026 -rpcport=20998 health

# 2. Move the rollback datadir aside (don't delete yet — keep for 7 days)
sudo mv /root/.dinero /root/.dinero.pre-phase-f-$(date -u +%Y%m%d)

# 3. Move the old binary aside
sudo mv /root/Dinero-Coin/build/dinerod /root/dinerod.pre-phase-f-$(date -u +%Y%m%d)

# 4. Move the old service unit aside
sudo mv /etc/systemd/system/dinerod.service /etc/systemd/system/dinerod.service.pre-phase-f-$(date -u +%Y%m%d)
sudo systemctl daemon-reload

# 5. After 7 more days of clean operation, the rollback artifacts
#    can be deleted. Until then, treat them as protected.
```

The 7-day window catches "everything looks fine for a day, then
something breaks at the next epoch boundary / RPC client / chain
event" — a class of bug we've seen before and don't want to be
caught flat-footed on.

---

## Promotion to fleet

After MO has soaked **clean for 7 days**, the packaged-service
mode is proven for one node and we can migrate the rest. Order:

1. CN (similar profile to MO, also on cookie-or-rpcuser flexibility)
2. LA (most-touched-by-pool node, but signed-stable)
3. VA (general-purpose)
4. Dell (last — it's the one with known-pre-existing data damage,
   so we want every other node packaged-stable before we touch it)

Each follows this same runbook. **Do not parallelize** — one node
at a time, ≥1 h soak between starts so a regression in one doesn't
cascade.

---

## What this runbook does NOT cover

- **Cross-version migrations** that change the chain state format.
  Today's RC is `2.2.5-1` and matches what MO currently runs at
  `25ee9cec6`. If a future RC bumps the chain format (e.g. utreexo
  forest format change), the migration becomes "stop, dpkg -i, run
  reindex" — different runbook.
- **Mining-related re-config.** MO is not a mining node; this
  runbook doesn't try to translate a manual `--gen` or pool
  config into the packaged service.
- **Multi-node simultaneous migration.** Strictly sequential.
- **Apt-managed upgrades.** Until we ship an apt repo (post-1.0),
  every Dinero Core upgrade is a manual `dpkg -i` of the new .deb,
  re-running the verify steps.

---

## Decision log

- **2026-05-02:** Initial runbook drafted. MO has not been migrated
  yet; the runbook is the precondition. First migration scheduled
  for the operator's next available window. v2.2.5-rc1 is the
  release candidate this runbook installs.
- **2026-05-02 (operator review):** Added explicit before/after
  cutover-proof gate. Preflight now snapshots
  `unit-active.before.txt`, `tip-height.before.txt`,
  `tip-hash.before.txt`. Start phase captures the same three
  `.after.txt` files and demands `is-active` flip + tip ≥ before
  + tip-hash identity (or strict forward-extension verified by
  `getblockhash <height-before>`). Soak phase enforces continuous
  forward-only descent from the pre-cutover tip. A rewind or fork
  is a hard-fail rollback at any point.
- **2026-05-02 (security tightening):** RPC config section made
  CONDITIONAL with an explicit decision matrix. Default is cookie
  auth + `rpcbind=127.0.0.1` (least-privilege). rpcuser/rpcpassword
  is added only when DineroDPI (or another off-daemon RPC client)
  has a current need. `rpcbind=0.0.0.0` is explicitly banned for
  "nostalgia" — the packaged-service contract is least-privilege
  by default, and widening the bind requires a specific reason
  recorded at the time. Found while preparing MO migration: MO's
  current `/etc/dinero/dinero.conf` does NOT have rpcuser/rpcpassword,
  so DineroDPI is either already using cookie auth, has been
  redirected, or is broken — the matrix forces the operator to
  decide which before re-opening the static-password path.

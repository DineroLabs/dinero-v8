# Dinero Core — 1.0 Contract

**Status:** LOCKED 2026-05-01. Owner: project (solo operator).
**Drafted:** 2026-05-01.
**Source plan:** [`docs/operations/core-discipline-plan.md`](../operations/core-discipline-plan.md).

This document is the contract every later phase of the Core Discipline
Plan tests against. **Modifying any value below is a contract change,
not an editing change.** A contract change requires deliberate
re-evaluation against the operator-experience promise; an editing
change is a typo fix. Treat them differently.

The promise: a third-party operator on a fresh Ubuntu 24.04 LTS VPS can
install Dinero Core, reach the fleet tip, and run a healthy full node
without contacting the maintainer. Everything below exists to make
that promise true.

---

## Decision summary

- **Two installation modes**: packaged-service (FHS-compliant `.deb`)
  and manual-user (tarball + `make install`). Same binary, same
  config schema, same health-check schema — only defaults and
  packaging differ.
- **Twelve standards** lock paths, names, schemas, and SLAs. Eleven
  cover the install/run/upgrade contract; the twelfth covers
  dependencies (bundled-by-default, no operator-side compilation).
- **Release-time gates** are mechanical checks (`ldd`, `readelf`,
  `dpkg -I`, version-info command) that a candidate build must pass.
  Failure of any gate fails the release.
- **OpenSSL CVE response SLA**: 7 days from public disclosure for
  critical/important issues. The operator has no other path to a
  fix; this SLA is the contract that replaces the distro security
  team for vendored crypto.

---

## Standard 1–11 — Packaged-service mode

Apt-installable `.deb`. Runs as a dedicated `dinero` system user.
FHS-compliant. This is the primary target for "Dinero Core 1.0."

| # | Standard | Decision |
|---|---|---|
| 1 | Data directory | `/var/lib/dinero/` (mode `0750`, owned `dinero:dinero`) |
| 2 | Config file | `/etc/dinero/dinero.conf` (owner `root:dinero`, mode `0640`) |
| 3 | Daemon binary | `/usr/bin/dinerod` |
| 4 | CLI binary | `/usr/bin/dinero-cli` |
| 5 | systemd unit | `/lib/systemd/system/dinero.service` |
| 6 | Log destination | systemd journal; rotation at `/etc/systemd/journald.conf.d/dinero.conf` (7d / 1GB cap) |
| 7 | Install path | apt-managed `.deb` (signed, with `SHA256SUMS.asc`) |
| 8 | Backup target | `/var/lib/dinero/{wallets,hd_wallet,blockchain/shielded_*}` |
| 9 | Rollback target | `/var/lib/dinero/binaries/dinerod.live-pre-<commit>-<ts>`; package itself rollbacks via `apt install dinero-core=<old-version>` |
| 10 | Health-check command | stable command (name picked in Phase D.4); `OK / DEGRADED / FAILING` schema, exit 0/1, `--json` for monitoring |
| 11 | Upgrade procedure | `docs/operations/upgrade.md`; capture rollback binary → install new package → wait for health OK → soak ≥1h |

**Why these values:**

1. **`/var/lib/dinero/`** — FHS reserves `/var/lib/<package>/` for variable
   state owned by a system service. Debian packaging policy expects
   packaged daemons to write here, not `/usr/local/` (admin-installed
   software outside the package manager) or `/etc/` (configuration
   only). Operators expect this path; deviating breaks monitoring
   conventions and feels amateurish.
2. **`/etc/dinero/dinero.conf` owner `root:dinero` mode `0640`** —
   `/etc/<package>/` is FHS-canonical for system-wide configuration.
   Owner `root` (root writes; the operator's normal `apt`/`vi`
   workflow), group `dinero` (the daemon reads). Mode `0640` =
   owner read/write, group read, world none. The daemon can read
   any RPC password / static credentials in the file; the world
   cannot. Standard `root:<service-group>` convention for packaged
   daemons that need to read sensitive config.
3. **`/usr/bin/dinerod`** — Packaged daemons live under `/usr/bin/`,
   not `/usr/local/bin/`. `/usr/local/` is reserved for software the
   sysadmin installed outside the package manager; a `.deb` that
   writes there is wrong by Debian policy.
4. **`/usr/bin/dinero-cli`** — Same rationale; CLI co-located with
   daemon.
5. **`/lib/systemd/system/dinero.service`** — Package-shipped units
   live here (or under `/usr/lib/systemd/system/` on systems that
   have moved). `/etc/systemd/system/` is reserved for local admin
   overrides; units the package owns shouldn't go there, otherwise
   `dpkg --verify` reports false-positive corruption when an operator
   adds a drop-in.
6. **systemd journal default, file opt-in** — Packaged Linux daemons
   log to journal so `journalctl -u dinero` works out-of-box; tooling
   like Promtail/Loki picks up journal automatically. The 7d / 1GB
   rotation cap prevents the `/var/log` blow-up observed on the
   pre-1.0 fleet (MO hit 11 GB during the Apr 30 incident response).
   File logging stays opt-in via `debug.log_file = <path>` in
   `dinero.conf` for operators who want one.
7. **`.deb` over alternatives** — Ubuntu/Debian is the default Linux
   server platform. `.deb` is the native artifact. AppImage, flatpak,
   and snap each carry tradeoffs (sandboxing, dependency duplication,
   distribution lock-in) inappropriate for a daemon. `.rpm` and
   Arch packages are post-1.0.
8. **Backup target = wallet + shielded state, not chain state** —
   Wallet seeds and shielded note material are irrecoverable from
   the network. Block + chaindb data is replay-able from peers.
   Backing up everything is wasteful and slow; backing up the right
   things is the operational discipline.
9. **`/var/lib/dinero/binaries/dinerod.live-pre-<commit>-<ts>`** —
   Lives inside `/var/lib/dinero/` so it's owned by the `dinero`
   user and survives package removal (which preserves data dir on
   non-purge). Naming captures both the binary it replaced and the
   timestamp of the upgrade event for forensic clarity.
10. **`OK / DEGRADED / FAILING` health schema** — Three states are
    the minimum to distinguish "working," "working with degraded
    surface," and "not working." `OK / FAIL` collapses too much;
    five states would be over-engineered. Exit 0/1 lets shell-driven
    monitoring (`if dinero-cli health; then ...`) work without
    parsing. `--json` keeps the structured output stable for
    Prometheus exporters, Datadog checks, etc.
11. **`docs/operations/upgrade.md`** — One canonical document, one
    canonical sequence per mode, with rollback commands. The
    procedure is encoded in a doc, not in operator memory; that
    transition is the entire point of Core discipline.

---

## Standards 1–11 — Manual-user mode

`make install` from a tarball or source checkout. Runs as the
invoking user. Conventional Bitcoin-Core-style home-rooted state.

| # | Standard | Decision |
|---|---|---|
| 1 | Data directory | `~/.dinero/` |
| 2 | Config file | `~/.dinero/dinero.conf` |
| 3 | Daemon binary | `${CMAKE_INSTALL_PREFIX}/bin/dinerod` (default `/usr/local/bin/dinerod`) |
| 4 | CLI binary | `${CMAKE_INSTALL_PREFIX}/bin/dinero-cli` |
| 5 | systemd unit | not installed; sample at `share/systemd/dinero.service.example` |
| 6 | Log destination | stderr (foreground); optional file via config `debug.log_file = <path>` |
| 7 | Install path | tarball + `install.sh`, or `git clone && make install` |
| 8 | Backup target | `~/.dinero/{wallets,hd_wallet,blockchain/shielded_*}` |
| 9 | Rollback target | `~/.dinero/binaries/dinerod.live-pre-<commit>-<ts>` (operator-managed) |
| 10 | Health-check command | identical to packaged mode |
| 11 | Upgrade procedure | identical schema to packaged mode; manual `make install` instead of `apt install` |

**Why these values diverge from packaged mode:**

- **`~/.dinero/`** mirrors `~/.bitcoin/`, `~/.litecoin/`, `~/.monero/`
  — every other cryptocurrency uses home-rooted state for user-run
  nodes. Operators familiar with Bitcoin Core expect this.
- **`/usr/local/bin/`** is the FHS-canonical location for
  software installed outside the package manager. `make install`
  from source is exactly that case.
- **Sample systemd unit, not installed** — a manual-mode install
  may not want systemd at all (containers, supervisor, runit,
  s6, plain `nohup` for dev). The sample at
  `share/systemd/dinero.service.example` is documentation, not
  policy.
- **stderr default** — manual-mode operators run interactively or
  under a process manager that captures stdout/stderr. Forcing
  systemd-journal output here would be surprising.

---

## Cross-mode invariants

The mode is purely a *defaults and packaging* decision. The daemon
itself does not branch on mode.

- **Daemon code is identical.** No `#ifdef PACKAGED`, no runtime mode
  detection.
- **Config schema is identical.** `dinero.conf` keys are the same in
  both modes. The path to the file is the only difference.
- **Health-check schema is identical.** External monitoring tools
  work against either install.
- **CLI ergonomics are identical.** `dinero-cli health` works the
  same way regardless of mode.
- **Cookie file is at `<datadir>/.cookie`** in both modes. CLI
  resolves it automatically.

---

## Cookie access — the `dinero` group convention

In packaged-service mode, the daemon runs as user `dinero`. The
cookie at `/var/lib/dinero/.cookie` is therefore owned `dinero:dinero`
mode `0640` — the daemon writes it, members of the `dinero` group
read it.

**Operators who need CLI access join the `dinero` group:**

```
sudo usermod -a -G dinero <operator-username>
# log out and back in so the new group takes effect
dinero-cli health
```

This matches Bitcoin Core packaging convention (`bitcoin` group on
Debian/Ubuntu) and is the standard ergonomic compromise: CLI commands
work without `sudo`, but only group members can issue them.

**Trust boundary**: anyone in the `dinero` group can issue any RPC,
including wallet operations like `wallet.sendtoaddress`. This is
intentional — the operator is also the wallet owner. For
multi-operator deployments where monitoring scrapers should NOT have
wallet access, run the scraper under a service account that is
explicitly NOT in the `dinero` group, and use the static `rpc.user`
+ `rpc.password` config keys (see Standard 12 §"Bundled-library
version reporting" below — same auth surface) scoped via firewall or
RPC whitelist for read-only methods.

In manual-user mode this question doesn't arise: the daemon, cookie,
and CLI all run as the same invoking user, so the cookie at
`~/.dinero/.cookie` is mode `0600` owned by that user, and
`dinero-cli` reads it directly.

---

## Standard 12 — Dependency rule

**Bundled-by-default.** The `.deb`'s `Depends:` field declares only
base-system packages. The operator never installs RocksDB, Boost,
Crypto++, OpenSSL, abseil, gRPC, protobuf, or any other library
manually. They install the package and run the service. Period.

### Build rules

- **Static-link**: small libraries with stable ABIs and no
  licensing constraints (Crypto++, abseil, jsoncpp, libsecp256k1,
  Boost components, vendored OpenSSL via
  `scripts/build-openssl-vendored.sh`).
- **Bundled-shared**: large libraries where static-linking bloats
  the binary unacceptably (RocksDB primarily; gRPC and protobuf
  TBD during Phase E based on size). Shipped as versioned `.so`
  files at `/usr/lib/dinero/`, resolved via `DT_RUNPATH`.
- **System-linked, honestly declared**: `libc6`, `libstdc++6`,
  `libgcc-s1`, `libz1`, `systemd`, `adduser`. These appear in the
  `.deb`'s `Depends:` field. Nothing else.

### `DT_RUNPATH` not `DT_RPATH`

Build flag: `-Wl,--enable-new-dtags -Wl,-rpath,/usr/lib/dinero`.

`DT_RUNPATH` (newer ELF tag, default on systems with binutils 2.x)
is searched **after** `LD_LIBRARY_PATH`; `DT_RPATH` (older) is
searched **before**. Choosing `DT_RUNPATH` lets a debugging operator
override the bundled `librocksdb.so` for diagnosis without rebuilding
the binary, while still preventing accidental shadowing by any
system-installed RocksDB at `/usr/lib/x86_64-linux-gnu/`.

Verification at release time:

```
readelf -d /usr/bin/dinerod | grep -E 'RPATH|RUNPATH'
```

Must show `RUNPATH`, not `RPATH`. If the wrong tag appears, the
build is wrong and the release fails.

### Honest `Depends:`

```
Depends: libc6 (>= 2.38), libstdc++6, libgcc-s1, adduser
```

Even libraries part of every minimal Ubuntu install in practice are
declared when the built artifact actually links to them. Apt resolves
them automatically; the operator never installs them manually. The
principle: **a short truthful `Depends:` is better than a short
pretend `Depends:`**. Declare what you link to.

### Bundled-library version reporting

A stable machine-readable command (exact name picked in Phase D.4 —
candidates: `dinero-cli getversioninfo`, `dinero-cli diagnostics
version --json`, or `dinero-cli version --json`) returns:

```json
{
  "dinerod": {
    "version": "2.2.5",
    "commit": "0786b9e10",
    "build_date": "2026-05-08T12:34:56Z"
  },
  "bundled_libs": {
    "rocksdb":  {"version": "8.10.0",     "sha256": "abc123...", "linkage": "shared-bundled"},
    "openssl":  {"version": "3.3.2",      "sha256": "def456...", "linkage": "static"},
    "cryptopp": {"version": "8.9.0",      "sha256": "...",       "linkage": "static"},
    "abseil":   {"version": "20240722.0", "sha256": "...",       "linkage": "static"}
  }
}
```

The command name is flexible; the **schema is the contract**.
External monitoring tools and security responders use this to detect
CVE-vulnerable versions without binary archaeology.

### OpenSSL CVE response SLA

Vendored static OpenSSL means an `apt upgrade` of the operator's
system OpenSSL **does not** patch the running daemon. This SLA
replaces the distro security team:

- **Critical / Important CVE**: patched release within **7 days of
  public disclosure**, published with a security advisory note at
  `dinero-releases`.
- **Informational CVE**: bundled into the next regular release.
- **Triage**: within 48 hours of disclosure, the maintainer
  classifies whether the affected code path is reachable by
  Dinero's actual usage.
- **Subscription**: maintainer subscribed to OpenSSL, Crypto++, and
  RocksDB upstream security advisory mailing lists.

Documented at [`docs/security/cve-response.md`](../security/cve-response.md).
Phase E.5 ships this doc — landed 2026-05-02 alongside the E.3
packaging-contract closure. The doc covers SLA timing, triage
workflow, upstream subscription list, disclosure format,
maintainer-absence mitigations, and review cadence.

---

## Standard 10 — Health-check schema

**Command**: `dinero-cli health` (also `health --json`).

**Output (text)**: one of `OK`, `DEGRADED <reason>`, `FAILING <reason>`.

**Exit code**: `0` on `OK`, `1` on `DEGRADED`, `2` on `FAILING`.

Granular exit codes preserve the shell-simple "non-zero is bad" check
(`if dinero-cli health; then ...`) while letting monitoring scripts
distinguish degraded warnings from outright failures
(`if [ $? -ge 2 ]` for hard alerts; `if [ $? -ge 1 ]` for any drift).
Bitcoin Core's `bitcoin-cli` and most systemd health-checks follow
this convention.

**Output (JSON)**:

```json
{
  "status": "OK",
  "exit_code": 0,
  "checks": {
    "tip_height": 10783,
    "tip_age_seconds": 142,
    "tip_age_threshold_seconds": 1800,
    "safemode_active": false,
    "peer_count": 6,
    "peer_threshold": 3,
    "tip_undo_present": true,
    "fatal_in_last_5min": 0
  }
}
```

`exit_code` mirrors the shell exit code (`0` = OK, `1` = DEGRADED,
`2` = FAILING) so consumers parsing the JSON don't have to map
status strings to severity.

**Internal checks** (any failure → `FAILING`; any threshold-edge →
`DEGRADED`):

- **Tip-not-stale**: `now - tip_block_time < 30 min`. Catches a node
  that has fallen off the fleet.
- **Safe mode inactive**: `safemode.status.active == false`.
- **Peer count ≥ 3**: a node with fewer than 3 peers is degraded
  even if otherwise healthy.
- **Tip undo data present**: `getblockheader(tip).undo_size > 0`.
  Specifically catches the phantom-undo regression class
  (Apr 30 LA incident). **Limitation**: only catches the failure
  if the missing undo includes the tip itself; historical holes
  in older heights are invisible to this check. A future
  `undo_audit_holes_remaining` field — fed by a startup audit
  (Phase D follow-up, post-1.0) — will surface historical holes
  without requiring `dinero-cli` to scan every height on every
  invocation. Documented here so the gap is explicit, not
  forgotten.
- **No FATAL in last 5 min of journal**: a freshly-fataled node may
  appear OK by every other measure but is about to die.

Schema is committed in this spec. Adding a check is a contract
change. Removing a check is a contract change. Reordering or
renaming JSON keys is a contract change. **Future fields may be
added** (e.g., `undo_audit_holes_remaining`) but never removed
once shipped — operators write monitoring against this schema.

---

## Standard 11 — Upgrade procedure

Lives in full at [`docs/operations/upgrade.md`](../operations/upgrade.md)
once Phase D.5 ships it. The canonical sequence:

**Packaged mode:**
1. `dinero-cli prepare-upgrade` (captures rollback binary).
2. `apt install dinero-core=<new-version>`.
3. systemd auto-restart.
4. `dinero-cli health` returns `OK`.
5. Soak ≥1h.
6. Done.

Rollback: `apt install dinero-core=<old-version>`.

**Manual mode:**
1. `dinero-cli prepare-upgrade`.
2. `git pull && cmake --build build && make install`.
3. `systemctl restart dinero` (or restart by other means).
4. `dinero-cli health` returns `OK`.
5. Soak ≥1h.
6. Done.

Rollback: copy `~/.dinero/binaries/dinerod.live-pre-<commit>-<ts>`
back to the install path, restart.

---

## Release-time gates

A candidate build is not a release until ALL of these pass on a
clean Ubuntu 24.04 LTS image:

```
# 1. Self-contained — no surprise dynamic deps
ldd /usr/bin/dinerod
  → ONLY system libs (libc, libstdc++, libpthread, libdl, libm, libz)
    + bundled .so files from /usr/lib/dinero/

# 2. RUNPATH discipline — not RPATH
readelf -d /usr/bin/dinerod | grep -E 'RPATH|RUNPATH'
  → RUNPATH present, RPATH absent

# 3. Honest Depends: — only base-system packages
dpkg -I dinero-core_*.deb | grep Depends
  → libc6, libstdc++6, libgcc-s1, adduser
  → no librocksdb-dev, no libssl-dev, no libboost-*, no Crypto++

# 4. Version reporting works
dinero-cli <version-info-command> --json | jq .bundled_libs
  → JSON object with rocksdb / openssl / cryptopp / abseil entries

# 5. Smoke test on fresh image (operator experience)
dpkg -i dinero-core_*.deb && systemctl enable --now dinero
sudo usermod -a -G dinero $USER && newgrp dinero
sleep 60 && dinero-cli health
  → OK

# 6. Cookie permissions correct
ls -l /var/lib/dinero/.cookie
  → -rw-r----- 1 dinero dinero ... (mode 0640, owner dinero, group dinero)

# 7. Config permissions correct
ls -l /etc/dinero/dinero.conf
  → -rw-r----- 1 root dinero ... (mode 0640, owner root, group dinero)
```

CI runs gates 1–4 on every build artifact and gate 5 on every
release tag. Failure of any gate fails the release build.

---

## Today's reality (verification footnote)

`ldd` of the production Linux binary at LA fleet node (commit
`3056607b9`, 2026-05-01):

```
libOpenCL.so.1, libudev.so.1                          (GPU mining backend)
libprotobuf.so.32, libgrpc++.so.1.51, libgrpc.so.29,
libgpr.so.29, libupb.so.29, libaddress_sorting.so.29  (gRPC stack)
libcrypto.so.3, libssl.so.3                           (SYSTEM OpenSSL,
                                                       not vendored)
libabsl_*.so.20220623 (50+ libs)                      (system abseil)
libstdc++.so.6, libm.so.6, libgcc_s.so.1, libc.so.6   (base system)
libcap.so.2, libz.so.1, libcares.so.2, libre2.so.10   (system utility)
```

**The current production binary depends on ~80 system shared libraries.**
The 1.0 contract requires this to drop to the base-system list plus
files under `/usr/lib/dinero/`. The bundling pass in Phase E is real
work, not a tweak. This footnote exists so future readers know what
state Phase A locked the contract from.

---

## Out of scope for 1.0

Anything not listed in Standards 1–12 is out of scope. Specifically
deferred:

- Reproducible / deterministic builds (signed-by-author + checksums
  for 1.0; deterministic rebuild by third parties is v2 of the
  release process).
- Apt repository serving the `.deb` (1.0 ships `dpkg -i`-installable
  artifacts; apt repo is post-1.0).
- Multi-distro support beyond Ubuntu/Debian (RPM/Fedora/Arch
  post-1.0).
- GUI installer (CLI install is sufficient for "competent operator").
- Auto-update.
- HSM / Yubikey integration.
- Prometheus/Grafana dashboards (the `health --json` schema enables
  these but doesn't ship them).
- macOS dinero-qt datadir migration (Mac stays at
  `~/Library/Application Support/Dinero/` per macOS convention).
- Android client packaging.
- **gRPC server / Lightning IPC over gRPC.** The `src/grpc/`
  blockchain + mempool + wallet services are dev-mode only. Two
  unmet preconditions for shipping in public Core: (a) the current
  server binds 0.0.0.0 with insecure credentials and has a TLS/mTLS
  TODO; (b) Lightning is not shipping in 1.0 so the only consumer
  of these services is internal dev. Packaged builds set
  `DINERO_RELEASE=ON` which excludes the gRPC stack entirely
  (`ENABLE_GRPC=OFF` + `DISABLE_GRPC` define + raw-socket
  Lightning IPC). Revisit when both preconditions are met:
  TLS/mTLS lands AND Lightning ships. The verifier's allowlist
  Depends gate will hard-fail any future packaged build that
  re-introduces the gRPC stack to the runtime surface.

Adding to this spec means changing the contract. Adding to this
section means saying "not yet" loudly and durably.

---

## Risk inventory

- **Bundling pass (Phase E) discovers an upstream ABI conflict** —
  e.g., a static-linked Crypto++ collides with a system-linked
  abseil. Mitigation: bundle abseil too if needed. Trade-off
  toward bigger `.deb` is accepted in §1.4.
- **`glibc 2.38` minimum excludes Ubuntu 22.04 and older** —
  accepted; Ubuntu 24.04 LTS and newer is the 1.0 target. A
  separate 22.04-compatible package can be added later if operator
  demand justifies the support burden.
- **CVE SLA gets stretched** by a maintainer absence — the SLA is
  a public commitment, not a hope. If operator availability is
  uncertain, document it and either revise the SLA or stage a
  backup-maintainer process. Don't quietly miss the window.
- **Health-check thresholds wrong on first calibration** — peer
  count ≥3 may be too aggressive on early-network nodes; tip-age
  threshold 30min may be too tight during low-volume periods.
  Phase D.4 sets conservative initial values; Phase F's fleet
  migration provides calibration data; revisit before declaring
  1.0 done.
- **External operator finds a doc gap during the Phase G test** —
  expected outcome, not failure. Iterate the doc, retry. 1.0
  doesn't ship until the fresh-VPS doc holds without operator
  contact.

---

## Decision log

- **2026-05-01:** Drafted. Source: discipline plan §1.1, §1.2, §1.4,
  §2, §3, §4, §6, §7. Standards 10 and 11 added at operator request
  to capture health-check and upgrade procedure as first-class
  contract obligations. RUNPATH-not-RPATH discipline and honest
  `Depends:` added at operator request. CVE SLA committed at 7 days.
  Today's-reality footnote added with verified Linux production
  `ldd` output (LA fleet node, commit `3056607b9`).
- **2026-05-01 (pre-lock review):** Three explicit decisions added
  per operator review:
  1. Config file ownership locked as `root:dinero` mode `0640`
     (was unspecified).
  2. Cookie access in packaged-service mode locked to the **`dinero`
     group convention**: cookie owned `dinero:dinero` mode `0640`,
     operators join the `dinero` group for CLI access. Matches
     Bitcoin Core packaging practice. Trust boundary explicit:
     group membership = full RPC including wallet operations.
  3. Health-check exit codes locked as **granular** (`OK=0`,
     `DEGRADED=1`, `FAILING=2`) rather than binary
     (`OK=0`, anything-else=1). Preserves shell-simple
     non-zero-is-bad while letting monitoring distinguish warning
     from failure. JSON output includes `exit_code` field for
     parsers.
  4. Future field `undo_audit_holes_remaining` flagged for the
     health-check schema, fed by a startup audit, to catch
     historical undo holes (not just the tip). Documented as a
     post-1.0 follow-up.
- **2026-05-01 (LOCKED):** Operator locked the contract. Phase A
  complete. Phase B (config-file loader), Phase C (CMake install
  target), and Phase D (logging/backup/rollback/health-check/upgrade
  policies) are now unblocked and may proceed in any order. Phase E
  (`.deb` packaging) follows after B-C-D. Phase F (fleet migration)
  follows E. Phase G (external operator test) follows F.
  Subsequent modifications to this document are contract changes,
  not editing changes.
- **2026-05-02 (E.4 runbook landed):** **Phase E.4 — release-signing
  runbook at [`docs/security/release-signing.md`](../security/release-signing.md)
  + placeholder publication wiring** (`contrib/keys/dinero-core-release.asc`,
  `contrib/keys/RELEASE-KEY-FINGERPRINT.txt`,
  `share/scripts/dinero-sign-release`) shipped. Key policy locked:
  `Dinero Core Release Signing <haydarevich69@icloud.com>`,
  Ed25519 primary cert-only + Ed25519 signing subkey (2y expiry),
  primary kept offline, subkey on signing host. Initial signed
  artifact: `SHA256SUMS.asc`.
- **2026-05-02 (E.4 LIVE):** **Operator generated the actual key.**
  Fingerprint:
  `4ED3 65CE 6604 B722 D281  EC77 3A61 4979 B8A4 8C02`.
  Public key + fingerprint published in `contrib/keys/`. Revocation
  certificate held offline by operator (not committed). Signing
  script flips automatically to live mode. E.6 final fully unblocked.
- **2026-05-02 (E.5 shipped):** **Phase E.5 — CVE response policy at
  [`docs/security/cve-response.md`](../security/cve-response.md)
  shipped.** 7-day SLA from public disclosure for critical/important
  CVEs in any `bundled_libs` entry, 48-hour triage, explicit upstream
  subscription list, disclosure format, maintainer-absence
  mitigations, annual review cadence. The SLA is the public contract
  that replaces "your distro's security team" for vendored libraries.
- **2026-05-02 (E.3 CLOSED):** **Phase E.3 packaging contract: CLOSED.**
  The `.deb` is statically disciplined, dependency-clean, layout-
  checked. `dinero-deb-verify --static <deb>` and
  `dinero-deb-verify --installed` both PASS with zero SKIPs across
  all 8 hard categories: ldd surface, DT_RUNPATH, Depends allowlist,
  install layout, file modes, postinst+postrm sanity, service unit,
  runtime contract (`dinero-cli health` exit-code matrix +
  `dinero-cli version --json` bundled-libs schema). End-to-end
  validated on Ubuntu 24.04 chroot. Final artifact: 7.6 MB `.deb`,
  19 MB `dinerod`, 4 NEEDED entries (libc, libstdc++, libgcc_s,
  libm), Depends: `libc6, libgcc-s1, libstdc++6, adduser`.
  Verifier runs locally — no CI dependency. **E.6 release-gate
  foundation: substantially complete, NOT fully closed.** The
  verifier portion of E.6 is done; the release ceremony (dedicated
  `dinero-core` GPG key, signed `SHA256SUMS.asc`, CVE response
  policy doc at `docs/security/cve-response.md`, final artifact
  publication flow) remains. Next ordering: E.4/E.5 (signing
  infrastructure + CVE policy) → E.6 final (build + sign + publish
  RC) → Phase F (fleet migration, MO first as staging, soak, then
  full fleet).
- **2026-05-02 (operator contract change):** **Ubuntu 24.04 LTS and
  newer is the Core 1.0 Linux baseline.** Earlier drafts targeted
  Ubuntu 22.04 / `libc6 >= 2.35`; the live fleet is entirely Ubuntu
  24.04, and the signed rc2 package correctly declares
  `libc6 >= 2.38`. Treat 22.04 support as a post-1.0 compatibility
  build, not a blocker for Core 1.0.

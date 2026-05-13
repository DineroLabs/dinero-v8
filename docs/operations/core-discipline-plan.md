# Dinero Core — Discipline & Release Plan

**Status:** DRAFT — planning only, no execution
**Drafted:** 2026-05-01
**Premise:** Dinero works as *operated infrastructure*. The next maturity step is to make it *adoptable software* — a Linux full node a competent third-party operator can install, run, monitor, back up, and recover **without contacting the maintainer**.

---

## 0. The distinction this document is built around

> **Fleet repaired** means *your* network is healthy.
> **Core released** means *other people* can safely join it.

Today, all five Linux nodes work because *you* know how to build, deploy, repair, monitor, and restart Dinero. None of that knowledge is encoded in software. This document plans the encoding.

What this means in practice:

- A fresh Ubuntu 24.04 LTS VPS, with no prior knowledge of Dinero, must be able to follow a single document and reach `getblockcount = <fleet tip>` in under 30 minutes of attended time.
- That document describes *one* path. Not "the way LA does it" or "the way MO does it." One.
- Everything supporting that document — installer, paths, config schema, service unit, log location, backup script, rollback procedure — is the actual deliverable.

This is "boring and standardized" by design. Excitement here is a failure mode.

---

## 1. The contract — the eleven standards, two modes

Linux daemon packaging has two well-established conventions: the **packaged service** mode (FHS-compliant `.deb`/`.rpm` install, dedicated service user, `/var/lib`-rooted state) and the **manual user** mode (tarball or `git clone + make install`, runs as the invoking user, home-rooted state). Conflating them produces a Frankenstein layout that feels amateurish to Linux operators on one side and overcomplicated to Mac/dev users on the other.

Dinero Core 1.0 supports **both modes**. Each mode has its own row, but the daemon code path is identical — only the *defaults* and packaging differ.

### 1.1 Packaged service mode (the primary target — what `apt install dinero-core` produces)

| # | Standard | Decision |
|---|---|---|
| 1 | Data directory | `/var/lib/dinero/` |
| 2 | Config file | `/etc/dinero/dinero.conf` |
| 3 | Daemon binary | `/usr/bin/dinerod` |
| 4 | CLI binary | `/usr/bin/dinero-cli` |
| 5 | systemd unit | `/lib/systemd/system/dinero.service` (shipped by the package) |
| 6 | Log destination | systemd journal (default); journald rotation policy at `/etc/systemd/journald.conf.d/dinero.conf` shipped by the package |
| 7 | Install path | apt-managed `.deb` (primary) |
| 8 | Backup target | `/var/lib/dinero/{wallets,hd_wallet,blockchain/shielded_*}` — declared, scripted, tested |
| 9 | Rollback target | Pre-restart binary backup at `/var/lib/dinero/binaries/dinerod.live-pre-<commit>-<ts>`; package itself rollbacks via `apt install dinero-core=<old-version>` |
| 10 | Health-check command | `dinero-cli health` returns a single-line status string with exit code 0/1; documented stable schema |
| 11 | Upgrade procedure | Documented sequence: capture rollback binary → `apt install` new version → wait for tip-match → `dinero-cli health` returns OK → soak ≥1h |

**Service user**: `dinero` (created by package postinst, system user, no shell, no home dir other than `/var/lib/dinero/`).
**File ownership**: `/var/lib/dinero/` owned `dinero:dinero`, mode `0750`. `/etc/dinero/dinero.conf` mode `0640` (group-readable for `dinero` user; secrets in this file justify keeping it out of world-readable mode).

### 1.2 Manual user mode (the dev/tarball fallback — what `make install` from source produces)

| # | Standard | Decision |
|---|---|---|
| 1 | Data directory | `~/.dinero/` |
| 2 | Config file | `~/.dinero/dinero.conf` |
| 3 | Daemon binary | wherever `make install PREFIX=...` places it (default `/usr/local/bin/dinerod`) |
| 4 | CLI binary | same prefix |
| 5 | systemd unit | not installed by default; sample at `share/systemd/dinero.service.example` for the operator to copy and customize |
| 6 | Log destination | stderr (foreground); optional `~/.dinero/debug.log` via config key `debug.log_file` |
| 7 | Install path | tarball + `make install` |
| 8 | Backup target | `~/.dinero/{wallets,hd_wallet,blockchain/shielded_*}` |
| 9 | Rollback target | `~/.dinero/binaries/dinerod.live-pre-<commit>-<ts>` (operator-managed; not auto-captured) |
| 10 | Health-check command | `dinero-cli health` (same command, same schema) |
| 11 | Upgrade procedure | Documented manual sequence: stop daemon → backup binary → `make install` → start → verify → soak |

**Service user**: invoking user (typically the operator's login user, `root` on the existing fleet, `tower` on Dell).

### 1.3 What's the same across both modes

- **Daemon code is identical.** The mode is purely a packaging-and-defaults decision, not a code-fork.
- **Config schema is identical.** `dinero.conf` keys are the same. The only difference is which path the daemon looks at by default.
- **Health-check schema is identical.** External monitoring tools work against either mode.
- **CLI ergonomics are identical.** `dinero-cli health` works the same way regardless of which mode the daemon was installed via.

### 1.4 Dependency strategy — the operator never compiles anything

The whole point of a `.deb` is that the operator hosts a full node, not a build environment. They should not need to know that Dinero internally uses RocksDB, Boost, Crypto++, abseil, or OpenSSL — and they should certainly not need to install those libraries themselves.

**Standard 12 — Bundled-by-default dependency model.** The `.deb` is built such that an operator's machine needs only a minimal base system to run it:

- Linux x86_64
- systemd
- glibc (ABI-compatible with Ubuntu 24.04 LTS minimum — this is the published baseline)
- libstdc++ (from the same glibc baseline)
- A handful of small utility libraries (`libpthread`, `libdl`, `libm`, `libz`) which are part of any Linux base install
- Disk space and open ports as documented in the fresh-VPS doc

That's the entire dependency surface visible to the operator.

Everything else — RocksDB, Boost, Crypto++, OpenSSL, abseil, gRPC if used, etc. — ships **statically linked into `dinerod` and `dinero-cli` binaries**, OR shipped as bundled `.so` files inside the `.deb` under `/usr/lib/dinero/` and resolved via the binary's `RPATH` / `RUNPATH`.

The build choice (static vs. bundled-shared) is per-library:

- **Static-link**: small libraries with stable ABIs and no licensing constraints (Crypto++, abseil, our own vendored OpenSSL build via `scripts/build-openssl-vendored.sh`).
- **Bundled-shared**: large libraries where static-linking would significantly bloat the binary (RocksDB) — shipped as a versioned `.so` file inside `/usr/lib/dinero/`, resolved via the binary's `DT_RUNPATH` (NOT `DT_RPATH`).
- **System-linked**: a short, deliberate list of base-system libraries. Everything system-linked is honestly declared in the `.deb`'s `Depends:` field, not assumed.

**`DT_RUNPATH` vs `DT_RPATH` — why this matters.** Both are mechanisms for a binary to tell the dynamic linker "look in this directory for `.so` files I depend on." The crucial difference: `DT_RPATH` (older) takes precedence over `LD_LIBRARY_PATH`, while `DT_RUNPATH` (newer, ELF default since binutils 2.x) is searched *after* `LD_LIBRARY_PATH`. Both are searched **before** the system default `/usr/lib/`, `/lib/`, etc. — meaning a `librocksdb.so` accidentally installed on the operator's system at `/usr/lib/x86_64-linux-gnu/librocksdb.so` will **not** be picked up by `dinerod` as long as `/usr/lib/dinero/` is set as `DT_RUNPATH` and contains a matching SONAME.

Build flag: `-Wl,--enable-new-dtags -Wl,-rpath,/usr/lib/dinero` (the `--enable-new-dtags` is what produces `DT_RUNPATH` instead of the legacy `DT_RPATH`). Verified at release time by `readelf -d /usr/bin/dinerod | grep -E 'RPATH|RUNPATH'` — must show `RUNPATH` and not `RPATH`.

**Honest `Depends:` declaration.** Even libraries we treat as "base-system" get declared. The minimal `.deb` `Depends:` is:

```
Depends: libc6 (>= 2.38), libstdc++6, libgcc-s1, adduser
```

If future builds link against `libz1` or another base package, it gets
declared. The operator never installs these manually — apt resolves
them — but the package is honest about what it links to. **A short
truthful `Depends:` is better than a short pretend `Depends:`.**

**Version reporting.** A stable, machine-readable command — exact name TBD during Phase D.4 (candidates: `dinero-cli getversioninfo`, `dinero-cli diagnostics version --json`, or simply `dinero-cli version --json`) — reports the bundled library versions and hashes. The contract is the *output schema* and its stability across releases, not the command name. The schema:

```json
{
  "dinerod": {
    "version": "2.2.5",
    "commit": "0786b9e10",
    "build_date": "2026-05-08T12:34:56Z"
  },
  "bundled_libs": {
    "rocksdb": {"version": "8.10.0", "sha256": "abc123..."},
    "openssl": {"version": "3.3.2", "sha256": "def456...", "linkage": "static"},
    "cryptopp": {"version": "8.9.0", "sha256": "...", "linkage": "static"},
    "abseil": {"version": "20240722.0", "sha256": "...", "linkage": "static"}
  }
}
```

This lets operators (and security responders) verify at a glance whether their running node has a CVE-vulnerable library version without scraping `ldd` and binary metadata.

**Why bundle by default rather than declare apt-style `Depends:`**:

- Distro-packaged `librocksdb-dev` versions vary across otherwise-supported systems. Pinning the exact version via apt-`Depends` either over-constrains the install (rejecting otherwise-fine systems) or risks a silent ABI mismatch.
- Operators on different supported Ubuntu 24.04 patch levels should get the same Dinero binary behavior. Bundling makes that promise; system-linking does not.
- Early-stage software with a small operator base benefits from boring uniformity over distro-native elegance.

Trade-off accepted: the `.deb` is larger (~80-150 MB vs. the ~30 MB a system-linked build would produce). That's fine. Disk is cheap; "it works on every supported VPS" is not.

**Future**: as Dinero Core matures and the operator base grows, individual libraries can be moved from bundled to system-`Depends` per release. Each such move is a separate, deliberate change with its own risk evaluation, not a default. 1.0 ships maximally bundled.

### 1.5 Summary contract

**Anything not in tables §1.1, §1.2, or rule §1.4 is out of scope for "Dinero Core 1.0."** Adding to this list is a feature decision and goes through normal change control, not a midnight Slack message.

---

## 2. Today's reality, gap by gap

A truthful inventory before the planning, so we don't pretend we're starting clean.

### 2.1 Datadir (Standard 1)

| Server | Datadir | Standard? |
|---|---|---|
| Dell | `/home/tower/.dinero/` | ✓ (modulo user) |
| LA, VA, MO, CN | `/root/Dinero-Coin/data-main/` | ✗ |

**Gap**: 4 servers tie datadir to source-tree path. Recovery scripts on each hardcode the bad path.

### 2.2 Config file (Standard 2)

**Gap**: There is no config-file loader today. All configuration is via CLI flags in the systemd ExecStart. `src/daemon/services/config_service.cpp` has a `GetString` API but no file-loader implementation. `dinero.conf` exists only as a comment in `test_ip_configuration.cpp`.

This is the biggest single piece of new code in the plan.

### 2.3 Daemon and CLI binaries (Standards 3, 4)

| Server | Binary path | Standard? |
|---|---|---|
| All Linux | `/root/Dinero-Coin/build/dinerod` | ✗ — in source-tree build dir |
| Dell | `/home/tower/Dinero-Coin/build/dinerod` | ✗ |
| Mac | `/Users/haydarevich/src/dinero/build/dinerod` | (dev only — fine) |

**Gap**: No production server has the binary at the standard location. The packaged-service standard is `/usr/bin/`; the manual-user standard is `/usr/local/bin/` (or wherever `make install PREFIX=...` lands it). There is no `make install` target wired into CMake, only build-in-place.

### 2.4 systemd unit (Standard 5)

| Server | Unit path | Shape |
|---|---|---|
| LA, VA, CN | `/etc/systemd/system/dinerod.service` | cookie auth, no rpcuser, runs as `root` |
| MO | `/etc/systemd/system/dinerod.service` | cookie auth + `--rpcuser/--rpcpassword` for iOS, runs as `root` |
| Dell | (none) | `nohup` shell-job, runs as `tower` |

**Gaps**:
- Service is named `dinerod.service` everywhere; the standard is `dinero.service`.
- Unit path is `/etc/systemd/system/` (admin-installed convention); the packaged-service standard is `/lib/systemd/system/` (package-shipped) with `/etc/systemd/system/` reserved for local overrides.
- All Linux servers run as `root`; the packaged-service standard runs as the dedicated `dinero` user.
- Dell has no service unit at all.
- MO's static credentials are inline in the ExecStart — fine functionally, but they should live in `dinero.conf` (mode `0640`) long-term.

### 2.5 Log destination (Standard 6)

| Server | Log destination |
|---|---|
| LA, VA, MO, CN | systemd journal (default for systemd-managed processes) |
| Dell | `/tmp/dinerod.log` (set by `nohup ... > /tmp/dinerod.log`) |

The daemon code (`src/common/logger.cpp:30`) supports an optional file destination opened in append mode. No env-var or flag exposes the path today; it's set by service-init code.

**Gap**: `/tmp/dinerod.log` on Dell survives no reboot. Linux servers' journal grows unbounded if `/etc/systemd/journald.conf` defaults are unchanged (already saw `/var/log` at 11 GB on MO during yesterday's repair). No journald rotation policy is documented.

### 2.6 Install path (Standard 7) and dependency strategy (Standard 12)

**Gap**: Zero packaging. No `.deb`, no AppImage, no flatpak, no `install.sh`. To run Dinero on a fresh VPS today, a third-party operator must:

1. Clone the source tree (which is a 1.4 GB+ repo).
2. Install ~40 build dependencies (CMake, libssl, abseil, RocksDB, jq, Boost, Crypto++, ...).
3. Build vendored OpenSSL via `scripts/build-openssl-vendored.sh`.
4. Run a 9000+-line `CMakeLists.txt` build (~3 min on EPYC, longer on a small VPS).
5. Manually create `~/.dinero/`, write a systemd unit, manage start/stop.

This is what Bitcoin Core looked like in 2010. It has to stop looking like this.

**Today's dependency surface** (what the build actually links against, per `ldd build/dinerod` on the existing fleet):

- **System libs** (kept system-linked in 1.0): `libc`, `libstdc++`, `libpthread`, `libdl`, `libm`, `libz` — all base-system, available on any supported VPS.
- **Vendored static** (already statically linked): OpenSSL (via `scripts/build-openssl-vendored.sh`), Crypto++, abseil — these become part of `dinerod` itself; no operator action needed.
- **Currently dynamically linked, must be bundled in the `.deb`**: RocksDB (large; `~50 MB` shared library), Boost (system thread/filesystem), jsoncpp, libsecp256k1.

**What changes in 1.0**: `cmake --install build` produces a binary whose only `ldd`-visible dependencies are the base-system libs. RocksDB and the rest either become statically linked into the binary or ship as `.so` files under `/usr/lib/dinero/` resolved via `RUNPATH`. The `.deb`'s `Depends:` field declares ONLY base-system packages (`libc6 (>= 2.38)` for the Ubuntu 24.04 baseline, plus whatever small base packages the built artifact honestly requires).

A third-party operator's experience becomes:

```bash
sudo dpkg -i dinero-core_2.2.5_amd64.deb        # apt resolves libc6, libz1, systemd, adduser if absent
sudo systemctl enable --now dinero
dinero-cli health                                # OK
dinero-cli <version-info-command> --json | jq .bundled_libs    # see exact library versions running
```

Six lines. No `apt install` of build dependencies. No compilation. And the operator (or a security responder) can introspect the bundled library versions without dissecting the binary.

### 2.7 Backup story (Standard 8)

**Gap**: There is no documented backup procedure. `wallets/` and `hd_wallet/` contain irrecoverable seed material. `blockchain/shielded_*` files are required for shielded-pool spend authorization. None of this is automated; none is tested.

The current "backup" is whatever ad-hoc `cp` commands you've run while debugging. That's not a backup story.

### 2.8 Rollback story (Standard 9)

**Today this works, in fragments.** During the Apr 30 chainstate-publication hardening pass, we captured per-node `dinerod.live-pre-<commit>` binaries from `/proc/<pid>/exe` before each restart. During yesterday's hole-repair, we created `chaindb.pre-rebuild-swap-<ts>` and `rev00000.dat.pre-rebuild-swap-<ts>` artifacts.

**Gap**: These were captured manually during specific operations. There is no policy that says "every binary upgrade captures a rollback artifact at this path with this naming." It's tribal — *I* know to do it. A new operator wouldn't.

### 2.9 Fresh-VPS doc

**Gap**: It does not exist.

### 2.10 Health-check (Standard 10)

**Gap**: No `dinero-cli health` command. Operators today compose three or four RPC calls (`getblockcount`, `getbestblockhash`, `safemode.status`, `getpeerinfo | jq length`) and eyeball the results. That's exactly the tribal-knowledge pattern Core discipline is meant to eliminate.

The required schema is a single command that returns one of: `OK`, `DEGRADED`, `FAILING`, with a stable JSON output for monitoring tools and a stable exit code (0 = OK, 1 = anything else). Internal checks at minimum: tip not stale (within N minutes of header time), safe mode inactive, peer count ≥ threshold, undo data present at tip.

### 2.11 Upgrade procedure (Standard 11)

**Gap**: There is no documented upgrade procedure. The sequence is *known* in operator memory — we executed it correctly during yesterday's CN binary upgrade and the Apr 30 fleet rotation — but it lives in `MEMORY.md` and Slack scrollback, not in the repo.

The required artifact is one document: "to upgrade Dinero Core, do these N steps in this order." Steps include: capture rollback binary, apt install (or build+install) new version, restart, wait for health-check OK, soak ≥1h, document outcome. With a documented rollback command for each step.

---

## 3. Phased plan

The work is sequenced so each phase is **independently shippable**, leaves the fleet in a working state, and produces a testable artifact. Skip-ahead is forbidden.

### Phase A — Lock the contract (1 day, no fleet changes)

The only deliverable is a written specification of the eleven standards in both packaged-service and manual-user modes. No code changes, no fleet changes. The point is to commit on paper before any tooling depends on the values.

**Deliverable**: `docs/spec/dinero-core-1.0.md` — a single document whose entire content is:

- The two tables from §1.1 and §1.2.
- Per row, 1-3 sentences explaining *why this value, not another* (e.g., "data lives in `/var/lib/dinero/` because FHS reserves `/var/lib/<package>/` for variable state owned by a system service, and Debian packaging policy expects packaged daemons to write here, not to `/usr/local/` or `/etc/`").
- The "what's the same across both modes" guarantees from §1.3.
- The cross-mode invariants: same daemon code, same config schema, same health-check schema, same CLI ergonomics.

**Test**: The operator can read the spec front-to-back in 5 minutes and answer all of these without scrolling: "where does the cookie file live in packaged mode?" / "where in manual mode?" / "what command do I run to check if a node is healthy?" / "what's the upgrade procedure?"

### Phase B — Config-file loader (2-3 days, no fleet changes)

Without this, every other phase has to choose between "encode the config in the systemd ExecStart" (the current scar) or "wait." Phase B unblocks B-onward.

**Scope**:

- A `LoadConfigFile(const std::string& path)` function in `ConfigService` that reads INI-style key=value pairs.
- CLI flag `--conf=<path>` overrides the default.
- Default path: `<datadir>/dinero.conf`.
- File missing is **not** an error — daemon proceeds with CLI/default values. (Bitcoin Core convention.)
- All config keys currently passed as CLI flags become recognized config-file keys. CLI flags continue to override file values. (Standard precedence: CLI > file > defaults.)

**Deliverable**:

- Code in `src/daemon/services/config_service.cpp`.
- Unit tests for parser (comments, blank lines, unicode, malformed lines, duplicate keys).
- Example file at `share/dinero.conf.example` documenting every key.

**Test**: `dinerod --datadir=/tmp/test-datadir` reads `/tmp/test-datadir/dinero.conf` if present. CLI flags override. File-missing doesn't error.

### Phase C — Install target + binary path (1 day code, 1 hour fleet)

CMake `install` target so binaries land at `${CMAKE_INSTALL_PREFIX}/bin/`. `CMAKE_INSTALL_PREFIX` defaults to `/usr/local` (manual-user mode); the `.deb` build sets it to `/usr` (packaged-service mode).

**Scope**:

- CMake `install(TARGETS dinerod dinero-cli ...)` rules respecting `CMAKE_INSTALL_PREFIX`.
- `make install` copies binaries + config example + systemd unit template.
- systemd unit template at `share/systemd/dinero.service` with **paths configured at install time** — the package build substitutes `/usr/bin/dinerod` and `User=dinero`, the manual install leaves the unit as a `.example` for the operator to copy and customize.
- Manual-mode default `make install` lands binaries at `/usr/local/bin/`, leaves systemd unit as `.example` only (manual users may not want systemd).

**Deliverable**:

- CMake changes.
- Tested on a fresh Ubuntu VM: `cmake --install build --prefix /usr/local` produces a working `/usr/local/bin/dinerod`. `cmake --install build --prefix /usr` produces a working `/usr/bin/dinerod` (used by Phase E).

**Fleet impact**: None yet. The fleet keeps running from `/root/Dinero-Coin/build/dinerod` until Phase F.

### Phase D — Logging, backup, rollback, health-check, upgrade procedure (3 days, no fleet changes)

Five pieces. Each is small in scope but real in surface area.

**D.1 — Log policy**
- Default to systemd journal (already true for packaged-service mode).
- journald retention policy at `/etc/systemd/journald.conf.d/dinero.conf` — shipped by the `.deb`. Default: 7 days max age, 1 GB max size.
- Optional `debug.log_file = <path>` config key for non-systemd users (and as opt-in for packaged-service users who want a file copy).

**D.2 — Backup policy**
- Script: `share/scripts/dinero-backup` that tars the datadir's `{wallets,hd_wallet,blockchain/shielded_anchor_history.bin,blockchain/shielded_frontier.bin,blockchain/shielded_nullifiers.db}` to a timestamped archive.
- Path-aware: respects `dinero.conf` `datadir` key, falls back to mode-appropriate default (`/var/lib/dinero/` packaged, `~/.dinero/` manual).
- Documents what's NOT backed up (chaindb, blocks/) and why (replay-able from peers).
- Tested by a fresh-VPS install + restore round-trip.

**D.3 — Rollback policy**
- Pre-restart hook captures `/proc/<pid>/exe` to `<datadir>/binaries/dinerod.live-pre-<commit>-<ts>` before any planned restart.
- For packaged-service mode, this is a `dinero-cli prepare-upgrade` command (or equivalent shell helper) that the operator runs *before* `apt install` of a new version. The package itself rolls back via `apt install dinero-core=<old-version>`.
- For manual mode, the same hook is documented as a step in the manual upgrade procedure.

**D.4 — Health-check command (Standard 10)**
- `dinero-cli health` prints a single-line human-readable status (`OK`, `DEGRADED`, `FAILING`) and exits with code 0/1.
- `dinero-cli health --json` prints structured output for monitoring tools. Schema is documented and stable.
- Internal checks: tip not stale (`now - tip_block_time < 30 min` or chain has not advanced for 30+ min on the rest of the fleet — needs a heuristic), safe mode inactive, peer count ≥ 3, `getblockheader $TIP` returns a non-null `undo_size` (catches the phantom-undo regression class), no FATAL in last 5 min of journal.
- Schema is implemented as a regular RPC call (`health` method) with the CLI command being a thin wrapper. Same schema works for monitoring scrapers.

**D.5 — Upgrade procedure (Standard 11)**
- One document at `docs/operations/upgrade.md` with one canonical sequence per mode:
  - **Packaged mode**: `dinero-cli prepare-upgrade` → `apt install dinero-core=<new>` → wait for systemd restart → `dinero-cli health` returns OK → soak ≥1h → done. Rollback: `apt install dinero-core=<old>`.
  - **Manual mode**: `dinero-cli prepare-upgrade` → `git pull && cmake --build && make install` → `systemctl restart dinero` (or restart by other means) → `dinero-cli health` returns OK → soak ≥1h → done. Rollback: copy `dinerod.live-pre-<commit>-<ts>` back to install path, restart.
- Includes a "what to do if health-check returns DEGRADED post-upgrade" troubleshooting section.

**Deliverables**: 5 scripts/commands + their tests + 2 doc fragments (`upgrade.md`, plus a section in the spec).

### Phase E — `.deb` packaging + install script + signed checksums (4-5 days, no fleet changes)

> **2026-05-02 — E.3 packaging contract: CLOSED.** `.deb` is
> statically disciplined, dependency-clean, layout-checked.
> `dinero-deb-verify --static` and `--installed` both PASS with
> zero SKIPs across 8 hard categories. Final: 7.6 MB `.deb`,
> 19 MB `dinerod`, 4 NEEDED, base-system Depends only.
> See `docs/specs/dinero-core-1.0.md` decision log + commit
> `2d742f3a4`.
>
> **E.6 release-gate foundation: substantially complete, NOT
> fully closed.** Verifier portion done; release ceremony
> (dedicated GPG key, signed `SHA256SUMS.asc`, CVE policy doc,
> publication flow) remains. Next ordering: E.4/E.5 (signing +
> CVE) → E.6 final (build + sign + publish RC) → Phase F.
>
> **2026-05-02 — E.4 runbook + E.5 CVE policy: shipped.**
> Release-signing runbook at `docs/security/release-signing.md`,
> placeholder key publication at `contrib/keys/`, signing helper
> at `share/scripts/dinero-sign-release` (dry-run until operator
> generates the key). CVE response policy at
> `docs/security/cve-response.md` — 7-day SLA committed publicly.
> E.6 final remaining gate: actual key generation (operator-owned,
> one-time) → real RC build/sign/publish.
>
> **2026-05-02 — E.4 key LIVE + E.6 final CLOSED.**
> Operator generated the dedicated signing key
> (fingerprint `4ED3 65CE 6604 B722 D281  EC77 3A61 4979 B8A4 8C02`).
> First RC published at
> `https://github.com/DineroLabs/dinero-releases/releases/tag/v2.2.5-rc1`.
> All 6 ceremony gates green; external verification confirmed.
> Phase F (MO migration) runbook drafted at
> `docs/operations/phase-f-mo-migration.md` — explicit + rollback-
> safe + DineroDPI-continuity-aware. MO migration NOT executed yet;
> awaiting operator window.

The user-facing artifact. FHS-compliant. Self-contained dependencies.

**Scope**:

- **Bundling pass first.** Before the `.deb` rules are written, switch the production build of `dinerod` and `dinero-cli` to the bundled-by-default model from §1.4:
  - Static-link Crypto++, abseil, OpenSSL (already vendored static), Boost components in use, jsoncpp, libsecp256k1.
  - RocksDB shipped as a versioned `.so` under `/usr/lib/dinero/librocksdb.so.<version>`; `dinerod` linked with `-Wl,-rpath,/usr/lib/dinero` so it resolves at runtime without `LD_LIBRARY_PATH` games.
  - **Verification**: `ldd build/dinerod` on a fresh build shows ONLY system libs (`libc`, `libstdc++`, `libpthread`, `libdl`, `libm`, `libz`) plus the bundled `librocksdb.so` resolved from `/usr/lib/dinero/`.
- `debian/` directory with rules to build a `.deb` containing:
  - `/usr/bin/dinerod`
  - `/usr/bin/dinero-cli`
  - `/usr/lib/dinero/librocksdb.so.<version>` (and any other bundled `.so` files)
  - `/lib/systemd/system/dinero.service` (shipped by the package; disabled by default — user runs `systemctl enable --now dinero`)
  - `/etc/dinero/dinero.conf.example` (mode `0644`, world-readable; the operator copies to `dinero.conf` and adjusts mode to `0640` if adding secrets)
  - `/etc/systemd/journald.conf.d/dinero.conf` (rotation policy from D.1)
  - `/usr/share/doc/dinero-core/...` (changelog, license, link to docs)
  - `/usr/share/man/man1/dinerod.1.gz`, `/usr/share/man/man1/dinero-cli.1.gz`
- **`Depends:` declares ONLY base-system packages** — `libc6 (>= 2.38)` (Ubuntu 24.04 baseline), `libstdc++6`, `libgcc-s1`, `adduser`, and any other base package the actual link/install surface honestly requires. **No `librocksdb-dev`, no `libssl-dev`, no `libboost-*`, no Crypto++.** That's the entire promise of Standard 12.
- Postinst creates the `dinero` system user (no shell, no home dir other than `/var/lib/dinero/`), creates `/var/lib/dinero/` mode `0750` owned `dinero:dinero`, runs `systemctl daemon-reload`.
- Postrm on `purge` removes `/var/lib/dinero/`. On `remove` (non-purge), preserves it (chain data + wallet seeds are not garbage to lose on package removal).
- Tarball + `install.sh` as the non-Debian fallback. The script is a thin wrapper that detects whether systemd is present and offers a packaged-service-style install or a manual-user install. The tarball contains the same self-contained binaries as the `.deb` — no compile step on the operator's machine, ever.
- Build the `.deb` in CI on every tag, on a clean Ubuntu 24.04 LTS image. CI artifact: a single `.deb` plus a signed `SHA256SUMS.asc`.
- CI also runs a smoke test: `dpkg -i` on a fresh container, `systemctl enable --now dinero`, wait for `dinero-cli health = OK`. Failure of the smoke test fails the release build.
- **Signed checksums** for every released artifact (`.deb`, tarball, source archive). Detached `.asc` signatures published alongside, signed with the dedicated `dinero-core` GPG key.

**Vendored crypto CVE response policy** (also a Phase E deliverable, lives at `docs/security/cve-response.md`):

- Subscribe maintainer to upstream OpenSSL, Crypto++, RocksDB security advisory mailing lists.
- When a CVE lands that affects a bundled library version: triage within 48 hours, classify as critical / important / informational based on whether the affected code path is reachable by Dinero.
- For critical or important CVEs: rebuild with patched library, tag a patch release within **7 days of public disclosure**, publish via `dinero-releases` repo with a security advisory note.
- For informational CVEs: bundle into the next regular release.
- The response policy is documented in the spec; the SLA is committed publicly.
- **Why this matters for vendored crypto specifically**: when OpenSSL is system-linked, an operator running `apt upgrade` automatically gets the patched system OpenSSL. When it's vendored static, that update path doesn't exist — the operator gets fixes by upgrading `dinero-core` itself. The 7-day SLA is the contract that replaces "your distro's security team" as the patching mechanism.

**Deliverable**: A signed `.deb` file + signed checksums file at `dinero-releases` repo for v2.1.29. Installable via `dpkg -i`. Apt-installable once we set up an apt repo (Phase G+ — out of scope of 1.0).

**Test**: On a fresh Ubuntu 24.04 LTS VPS:
```bash
wget https://github.com/DineroLabs/dinero-releases/releases/download/v2.1.29/dinero-core_2.1.29_amd64.deb
wget https://github.com/DineroLabs/dinero-releases/releases/download/v2.1.29/SHA256SUMS.asc
gpg --verify SHA256SUMS.asc                          # signature valid
sha256sum -c SHA256SUMS                              # checksums match
sudo dpkg -i dinero-core_2.1.29_amd64.deb
sudo systemctl enable --now dinero
sudo -u dinero dinero-cli health                     # expect: OK
sudo -u dinero dinero-cli getblockcount              # expect: a number
```

### Phase F — Migrate the existing fleet to the new contract (4-6 hours, attended)

Now and only now do we touch LA/VA/MO/CN/Dell. The migration is to **packaged-service mode** on all five servers — i.e., the same artifact a third-party operator runs. Half-measures (manual-user mode on the fleet) defeat the purpose of Phase E.

The migration follows the [datadir-migration-plan.md.draft](datadir-migration-plan.md.draft) for the data-move pattern (atomic `mv`, sequential CN→LA→VA→MO with ≥1h soak), but the actual operations on each server are:

1. `apt install ./dinero-core_<version>_amd64.deb` (creates `dinero` user, `/var/lib/dinero/`, ships `/lib/systemd/system/dinero.service`).
2. Stop the existing `dinerod.service`, atomically `mv /root/Dinero-Coin/data-main/* /var/lib/dinero/`, `chown -R dinero:dinero /var/lib/dinero/`.
3. Translate the existing inline CLI flags from the old systemd unit into `/etc/dinero/dinero.conf` keys. (Phase B's config loader is a hard dependency here — without it, this step is impossible.)
4. Disable old `dinerod.service`, enable + start new `dinero.service`.
5. `dinero-cli health` returns OK → soak ≥1h.

Dell migration is the same flow — `apt install` the `.deb`, migrate from `nohup`-as-`tower` to `dinero.service`-as-`dinero` user, datadir move from `/home/tower/.dinero/` to `/var/lib/dinero/`. **Dell is no longer special.**

**Deliverable**: All 5 nodes running off the identical packaged artifact. The `MEMORY.md` Server Deployment section collapses to one bullet: "All 5 nodes on Dinero Core 1.0 contract via `dinero-core_<version>` package — see `docs/spec/dinero-core-1.0.md` and `docs/operations/upgrade.md`."

### Phase G — Fresh-VPS doc + first external operator test (1 day)

The actual proof-of-graduation.

**Deliverable**: `docs/operations/fresh-vps.md` with a literal copy-pasteable command sequence. Section headings:

- Prerequisites (Ubuntu 24.04 LTS, 4 GB RAM, 50 GB disk, port 20999 reachable)
- Install
- First-run config
- Verify sync
- Add to monitoring (optional)
- Common operations (start, stop, restart, log inspection, backup, restore, upgrade, rollback)
- Troubleshooting (with the top-5 known failure modes)

**Test**: A friend who has never touched Dinero takes a fresh VPS, follows the doc, and reaches `getblockcount = <fleet tip>` in <30 minutes without contacting you. If they need to contact you: the doc has a gap, and we fix the doc, not improvise the answer.

This is the moment "Dinero Core" actually means something.

---

## 4. What "Core 1.0" explicitly does NOT include

Items that are *related* to Core discipline but are independent work, not gating:

- **Reproducible / deterministic builds.** v2 of the release process. Until then, builds are signed-by-author with published checksums but not bit-for-bit reproducible by third parties. Signed checksums are NOT deferred — they ship in Phase E.
- **Apt repo at `apt.dinero.coin`** (or wherever). Phase E ships a `.deb` installable via `dpkg -i`; serving it via apt is Phase G+.
- **Multi-distro support beyond Ubuntu/Debian.** RPM/Fedora/Arch are post-1.0.
- **GUI installer.** Out of scope; CLI install is sufficient for "competent operator."
- **Auto-update.** Operators run `apt upgrade` on their own cadence. Auto-update is a security policy decision that needs its own doc.
- **HSM / Yubikey integration for cookie auth.** Out of scope.
- **Metrics scraping and Prometheus integration.** The daemon already exposes some metrics; productionizing them is a separate project.
- **Migration of Mac wallet (`dinero-qt`) datadir** to match. The Mac path stays at `~/Library/Application Support/Dinero/` per macOS convention.
- **iOS/Android client packaging.** DineroDPI iOS already shipping; Android is a separate decision.

Anything not explicitly listed in §1's nine standards is in this section by default.

---

## 5. Sequencing logic and dependencies

```
A (spec) ─── locks decisions ──────┐
                                    │
B (config file loader) ──┬──────────┤
                          │          │
C (install target) ───────┤          │
                          │          │
D (log/backup/rollback) ──┤          │
                          │          │
                          ▼          ▼
                          E (.deb packaging)
                                    │
                                    ▼
                          F (fleet migration)
                                    │
                                    ▼
                          G (fresh-VPS doc + external test)
```

**Strict ordering rules:**

- F (fleet migration) MUST come after E (.deb packaging). Rationale: the migration target is "running off the same artifact a third-party operator runs." If E hasn't shipped, F is migrating to a still-shifting target.
- G (external operator test) MUST come after F. Rationale: if your own fleet doesn't run on the contract, an external operator can't be expected to either.
- A (spec) MUST come first. Rationale: every later phase tests against the spec; if the spec moves, all tests move.
- B, C, D can be done in any order or in parallel after A. They have no inter-dependencies.

**Total time estimate**: 12-15 days of focused engineering for a solo operator, spread over 4-6 weeks of calendar time. Calendar slack accounts for soak windows, external-operator availability, and the inevitable "while I was in there I noticed..." cleanups.

---

## 6. Risks, decision points, and accepted tradeoffs

| Risk | Mitigation | Accepted? |
|---|---|---|
| Phase B (config loader) discovers config-key namespace mess | Inventory all current CLI flags first; bucket into `rpc.*`, `p2p.*`, `wallet.*`, etc. | Yes — the namespace cleanup IS the work |
| `.deb` packaging needs maintainer GPG key | Generate a separate `dinero-core` release/package signing key, distinct from the macOS Developer ID. Conflating them is bad cryptographic hygiene. | Yes — needs you to do once |
| External operator test fails | The doc has a gap; iterate. Don't declare 1.0 until clean | Yes |
| Fleet migration soak surfaces a regression | Rollback per Phase F (packaged: `apt install dinero-core=<old>`; manual: copy backup binary, restart); postpone 1.0 | Yes |
| MO's static credentials need to live in config file | Phase B handles this — `rpc.user` / `rpc.password` keys in `/etc/dinero/dinero.conf` with mode `0640`. Cookie auth still works alongside, unchanged. | Yes |
| Dell user is `tower`, not `root` | Phase F migrates Dell to packaged-service mode along with everyone else: datadir becomes `/var/lib/dinero/`, service runs as `dinero` user. Wallet on `tower`'s home dir, if any, gets migrated as part of the cutover. | Yes — Dell is no longer special after Phase F |
| Reproducible builds get pulled forward by an external request | Defer; ship signed-by-author + signed checksums for 1.0 | Yes |
| `/var/lib/dinero/` permissions wrong on first install | Postinst tested on fresh VM as part of Phase E's deliverable. CI runs the postinst on a clean Ubuntu image and asserts ownership/mode. | Yes |
| Health-check thresholds wrong (false DEGRADED on healthy node) | Conservative thresholds in Phase D.4 first pass; tune after fleet migration provides a stable population to calibrate against. Document threshold values in the spec. | Yes |
| Upgrade procedure works for binary-only changes but not for config-schema changes | Phase D.5's procedure applies to binary upgrades. Config-schema migrations need a separate doc; flagged as a follow-up risk for 1.1. | Yes — flagged, not solved in 1.0 |
| Bundled `.deb` is large (~80-150 MB) | Trade-off accepted in §1.4. Disk is cheap; uniformity is not. CI smoke-test ensures the bundle actually works on a fresh image. | Yes — explicit choice |
| Bundled-shared `librocksdb.so` collides with system-installed `librocksdb` | Bundle lives at `/usr/lib/dinero/`, NOT `/usr/lib/`. Resolved via binary `RUNPATH` only. System `librocksdb` (if present) is invisible to `dinerod`. | Yes — handled by RUNPATH |
| Statically-linked OpenSSL forks security CVE response | Documented response process at `docs/security/cve-response.md` is a Phase E deliverable, NOT a follow-up doc. **7-day SLA from public CVE disclosure** for critical/important. Maintainer subscribed to upstream advisory lists. `dinero-cli version --json` reports the bundled OpenSSL version + sha256 so operators can audit. Operators get fixes via `apt install dinero-core=<new>`, not via system OpenSSL update. | Yes — accepted, with **publicly committed SLA** |
| Bundled `librocksdb.so` accidentally shadowed by a system-installed RocksDB | Build with `-Wl,--enable-new-dtags -Wl,-rpath,/usr/lib/dinero` to produce `DT_RUNPATH` (not `DT_RPATH`). Verified at release time by `readelf -d /usr/bin/dinerod \| grep RUNPATH`. System `librocksdb` (if any) is invisible because `/usr/lib/dinero/` is searched first. | Yes — `RUNPATH` discipline enforced at release time |
| `glibc` ABI on operator's distro is older than 2.38 | Build target is Ubuntu 24.04 LTS minimum. Older systems (Ubuntu 22.04, Ubuntu 20.04, Debian 11) will see `dpkg: dependency problems`. Documented in fresh-VPS doc as a hard prerequisite. | Yes — Ubuntu 22.04 and older are not supported in 1.0 |

**Single biggest gotcha**: Phase B (config-file loader) is a code change to a piece of code that hasn't seen new feature work in months. Touching it requires the same scrutiny we gave the rebuilder yesterday — read the existing code, write tests against current behavior, then add the feature. Don't yolo.

---

## 7. What's done when

The transition from "operated infrastructure" to "Dinero Core 1.0" is complete when ALL of these are true:

- [ ] All twelve standards from §1 (both modes plus the dependency rule §1.4) are implemented, not just specified.
- [ ] `ldd /usr/bin/dinerod` on a fresh `dpkg -i` install shows ONLY base-system libs + `/usr/lib/dinero/librocksdb.so` — no surprise dependencies.
- [ ] `readelf -d /usr/bin/dinerod | grep -E 'RPATH\|RUNPATH'` shows `RUNPATH` (not `RPATH`) — confirms `LD_LIBRARY_PATH` overrides work and `/usr/lib/dinero/` shadowing isn't accidental.
- [ ] `dpkg -I dinero-core_*.deb | grep Depends` shows ONLY base-system packages (`libc6`, `libstdc++6`, `libgcc-s1`, `libz1`, `systemd`, `adduser`). Honest, not pretended.
- [ ] A stable machine-readable version command (exact name picked in Phase D.4) reports bundled library versions and SHA256 hashes for `rocksdb`, `openssl`, `cryptopp`, `abseil`. Schema is documented and committed-to.
- [ ] `docs/security/cve-response.md` exists with the 7-day SLA committed.
- [ ] All five fleet nodes run from the packaged-service contract — same `.deb`, same paths, same `dinero` user, same systemd unit.
- [ ] A signed `.deb` exists at `dinero-releases` for the current release, with a published `SHA256SUMS.asc`.
- [ ] `dinero-cli health` works from `dpkg -i` to `health` without manual intervention beyond the Phase G doc.
- [ ] `docs/operations/upgrade.md` exists and has been used at least once on the live fleet.
- [ ] `docs/operations/fresh-vps.md` exists and is provably correct (someone other than you ran through it).
- [ ] An external operator has done a fresh-VPS install successfully without contacting you.
- [ ] Memory has been compacted: dozens of operator-tribal-knowledge entries collapse into "see `docs/spec/dinero-core-1.0.md`."

When that's all true: **other people can safely join the network**. That's the line.

---

## 8. Sign-off space

- Plan reviewed by operator: ____________________ Date: __________
- Phase A target start: __________
- Phase A target completion: __________
- Phase G target completion (Core 1.0 release): __________
- Final outcome: ____________________

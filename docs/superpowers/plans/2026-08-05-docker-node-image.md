# Official Docker Node Image — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Status: EXECUTED.** This plan has been implemented and then corrected by a final
> whole-branch review. The full file listings that its steps used to contain were
> removed afterwards — several of them carried bugs the review found — and replaced by
> pointers. **The shipped files are authoritative; the steps below are a record of how
> the work was done, not text to copy.** See the post-review correction block in Global
> Constraints, and `.superpowers/sdd/2026-08-05-docker-node-image/final-fix-report.md`.

**Goal:** Let a stranger run a validating Dinero node with one `docker run`, syncing to the tip in minutes via a bundled AssumeUTXO snapshot.

**Architecture:** Two-stage Dockerfile. Stage 1 downloads the official v8.1.1 release bundle and the AssumeUTXO snapshot from GitHub Releases and verifies both against published SHA256SUMS. Stage 2 is `debian:13-slim` (bookworm's glibc 2.36 is too old) with the one shared library `dinerod` actually needs. An entrypoint script arms the snapshot on **every** start — see the post-review correction below; the original "only on a fresh datadir" design bricked volumes — then `exec`s dinerod as PID 1.

**Tech Stack:** Docker (BuildKit), debian:13-slim, POSIX shell, GitHub Actions.

## Global Constraints

All values below were verified empirically against the shipped v8.1.1 artifacts. Do not substitute assumptions.

- **Release bundle:** `dinero-linux-x86_64-8.1.1.tar.gz` — contains BOTH binaries at `dinero-linux-x86_64-8.1.1/dinerod` and `dinero-linux-x86_64-8.1.1/dinero-cli`.
- **Do NOT use** `dinero-core-*.tar.gz` / `dinero-cli-*.tar.gz` — they are **not covered by any published checksum**.
- **`SHA256SUMS-linux-x86_64-8.1.1` has exactly 3 entries:** the bundle tarball, and the two binaries by their in-tarball paths. So run `sha256sum -c` **after** extracting, when all three paths exist.
- **Snapshot:** `dinero-assumeutxo-73035-v4.dat` (27 MB) + `dinero-assumeutxo-73035-v4.manifest.json`, verified by `SHA256SUMS-assumeutxo-73035` (2 entries).
- **Height 73035 IS a compiled-in trust anchor in v8.1.1** (`src/consensus/assume_utxo.cpp:100`), so the daemon will accept this snapshot. Do not swap in a different height.
- **⚠️ CORRECTED 2026-08-05 (both were plan errors, found during Task 1):**
  - **Runtime base is `debian:13-slim`, NOT `debian:12-slim`.** The bundle binary requires
    **GLIBC_2.38** and **GLIBCXX_3.4.32**; Debian 12 (bookworm) ships glibc 2.36 and cannot
    run it — the image builds and then dies with `GLIBC_2.38 not found`. Debian 13 (trixie)
    ships glibc 2.41 / GCC 14. `ubuntu:24.04` also works (glibc 2.39) if a base change is
    ever needed.
  - **Runtime deps are `libudev.so.1` ONLY** (plus libstdc++/libm/libgcc_s/libc from the
    base). `libminiupnpc`/`libnatpmp` are **NOT** in the bundle binary's DT_NEEDED. The
    original list was read from `dinero-core-8.1.1/dinerod`; the plan then switched to
    `dinero-linux-x86_64-8.1.1.tar.gz`, and those are **different builds**. Verified on the
    bundle binary. Debian package: `libudev1`.
  - **distroless still will NOT work** — it lacks `libudev.so.1` and ships an older glibc.
- **No libssl/libsqlite3 dependency** — statically linked.
- **Ports:** RPC `20998`, P2P `20999`, WS `21001` (`include/crypto/config.h:11-12`).
- **~~Fresh-datadir rule, copied from `qt/src/main.cpp:105-107`~~ — WRONG, see the post-review correction below.** The snapshot is passed on every start; the daemon's own preconditions decide.
- **Snapshot args:** `--assumeutxo_snapshot=<path>` **and** `--assumeutxo_forward_connect=1`. Omitting the second holds the tip at the snapshot base for the whole background replay (PR #393) — that shipped as a real user-facing bug.
- **amd64 only.** Release assets are linux-x86_64. The dev machine is **Apple Silicon (aarch64)**, so every local docker command MUST pass `--platform linux/amd64` and will run under emulation (slow but functional).
- **Working directory:** `/Users/haydarevich/src/dinero-v8-docker` (git worktree, branch `feat/docker-node-image`). Do NOT touch `/Users/haydarevich/src/dinero-v8` — it has unrelated uncommitted work on another branch.
- **Do not modify** the root `.dockerignore`. (`ops/Dockerfile` was deleted in a follow-up
  commit as dead, unbuildable code — nothing ever built it.)
- **Do not push.** The operator pushes.
- **⚠️ FOUND IN TASK 2 RUNTIME VERIFICATION (both were plan errors):**
  - **`dinero-cli` REQUIRES `-datadir=/data`.** Without it, it looks in `$HOME/.dinero`
    and fails. Every documented `dinero-cli` invocation must pass it.
  - **`docker stop` needs a longer grace period.** Clean shutdown takes ~11.8s; Docker's
    default is 10s, so the default `docker stop` SIGKILLs the daemon (exit 137) mid-shutdown
    on every stop. The documented run command uses `--stop-timeout 60`. A Dockerfile cannot
    set this — it must be on the run command or in the compose file.
- **⚠️ FOUND IN FINAL WHOLE-BRANCH REVIEW (all shipped; this plan's original text was wrong):**
  - **Arm the snapshot UNCONDITIONALLY.** The "only on a fresh datadir" rule below was
    transplanted from the Qt app, which predates the daemon's restart-restore path. The
    daemon creates `/data/blocks` and `/data/blockchain` seconds into startup, minutes
    before the import, so the rule reads "existing datadir" for the entire window in which
    it matters; the daemon then finds persisted AssumeUTXO metadata, no configured
    snapshot path, and exits 2 on that start and every later one — an unrecoverable volume.
    Reproduced and fixed; see `.superpowers/sdd/2026-08-05-docker-node-image/final-fix-report.md`.
  - **Bundle the manifest as `<snapshot>.manifest.json`, under the manifest's own name.**
    `/opt/dinero/snapshot.manifest.json` was never read. The manifest declares
    `snapshot_file: "mainnet-snapshot.dat"` and the daemon's trust gate compares that to
    the on-disk filename, so the snapshot ships as `/opt/dinero/mainnet-snapshot.dat` with
    `/opt/dinero/mainnet-snapshot.dat.manifest.json` beside it. Naming it
    `snapshot.dat.manifest.json` would hard-fail the load, not fix the warning.
  - **Pin the snapshot's release separately** (`ARG SNAPSHOT_RELEASE=v8.1.1`). No workflow
    produces the assumeutxo assets; they were a one-off upload, so fetching them from
    `v${DINERO_VERSION}` 404s on the next release. The checksum file's height is derived
    from `SNAPSHOT_NAME` rather than hardcoded.
  - **Do NOT `EXPOSE` 20998.** `docker run -P` publishes every EXPOSEd port on 0.0.0.0 and
    `dinerod` has no `rpcallowip` gate.
  - **The artifacts are not signed.** SHA256SUMS only, over the same channel. Say
    "verified against the published SHA256SUMS".
  - **The documented run command also needs `--log-opt max-size=50m --log-opt max-file=3`.**
    Measured ~0.96 GB of logs per hour; the default `json-file` driver never rotates.

---

### Task 1: Dockerfile + entrypoint

**Files:**
- Create: `Dockerfile` (repo root)
- Create: `docker-entrypoint.sh` (repo root)

**Interfaces:**
- Produces: image with `/usr/local/bin/dinerod`, `/usr/local/bin/dinero-cli`, `/opt/dinero/mainnet-snapshot.dat`, `/opt/dinero/mainnet-snapshot.dat.manifest.json` (corrected — see above), entrypoint `/usr/local/bin/docker-entrypoint.sh`, `VOLUME /data`, user `dinero` (uid 10001).
- Produces: build args `DINERO_VERSION` (default `8.1.1`), `SNAPSHOT_RELEASE` (default `v8.1.1`), `SNAPSHOT_NAME` (default `dinero-assumeutxo-73035-v4`) and `SNAPSHOT_INSTALL_NAME` (default `mainnet-snapshot.dat`).

- [ ] **Step 1: Write `docker-entrypoint.sh`**

> **The full listing that used to sit here has been removed.** It contained the
> fresh-datadir arming rule that the final review found bricks volumes (see the
> post-review correction in Global Constraints), and leaving a copyable copy of it in the
> plan is exactly how that bug would come back. The shipped file is
> [`docker-entrypoint.sh`](../../../docker-entrypoint.sh) — read it there; it is the only
> copy. In summary it sets `-datadir`, `-printtoconsole=1`, `-listen=1`, `-port=20999`,
> `-rpcbind=${DINERO_RPCBIND:-127.0.0.1}` (loopback by default — pre-merge correction 5;
> binding `0.0.0.0` exposed RPC to every other container on the same Docker network and
> the daemon has no `rpcallowip` gate), `-rpcport=20998`, appends
> `--assumeutxo_snapshot=$DINERO_SNAPSHOT` and `--assumeutxo_forward_connect=1` whenever
> the bundled snapshot file exists, prints ONE startup line derived from the final argv
> (never the argument vector itself — pre-merge correction 4: a user's `-rpcpassword=`
> would otherwise land verbatim in `docker logs`), and `exec`s `dinerod` so it is PID 1.

- [ ] **Step 2: Write `Dockerfile`**

> **The full listing that used to sit here has been removed** for the same reason: it
> carried the "SIGNED release artifacts" claim, `EXPOSE ... 20998/tcp`, the unused
> `/opt/dinero/snapshot.manifest.json` path and the snapshot fetched from
> `v${DINERO_VERSION}` — all four corrected in the final review. The shipped file is
> [`Dockerfile`](../../../Dockerfile); it is the only copy. Shape: two stages on
> `debian:13-slim`; stage 1 curls the release bundle from `v${DINERO_VERSION}` and the
> snapshot + manifest + checksums from `${SNAPSHOT_RELEASE}`, verifies both SHA256SUMS
> files against the published names, and asserts the manifest's `snapshot_file` matches
> `SNAPSHOT_INSTALL_NAME`; stage 2 installs `libudev1`, copies the two binaries and the
> snapshot pair, sets `DINERO_SNAPSHOT`, `VOLUME /data`, `EXPOSE 20999/tcp` only, and runs
> as uid 10001.

- [ ] **Step 3: Build it**

```bash
cd /Users/haydarevich/src/dinero-v8-docker
docker build --platform linux/amd64 -t dinerod:test .
```

Expected: build succeeds. If `apt-get install` fails on a package name, that is a real finding — report the exact name rather than guessing a substitute. Then confirm the binary actually RUNS: `docker run --rm --platform linux/amd64 --entrypoint /usr/local/bin/dinerod dinerod:test --version`. A build that succeeds but cannot execute (glibc too old) is a failure.

- [ ] **Step 4: Prove the checksum gate actually fires**

A verification step that cannot fail is worthless. Confirm it rejects a bad artifact:

```bash
docker build --platform linux/amd64 --build-arg SNAPSHOT_NAME=dinero-assumeutxo-73035-v4 \
  --build-arg DINERO_VERSION=8.1.0 -t dinerod:badver . 2>&1 | tail -5
```

Expected: FAILS (either the download 404s or the checksum mismatches). Either is an acceptable demonstration that a wrong/absent artifact does not silently produce an image. Record which occurred.

- [ ] **Step 5: Commit**

```bash
cd /Users/haydarevich/src/dinero-v8-docker
git add Dockerfile docker-entrypoint.sh
git commit -m "feat(docker): official full-node image with bundled AssumeUTXO snapshot"
git show --stat --format= HEAD
```

---

### Task 2: Verify the image actually runs a node

**Files:** none (runtime verification only)

**Interfaces:**
- Consumes: `dinerod:test` image from Task 1.

This is the task that matters. An official image that builds but does not sync, does not find peers, or re-syncs on every restart is worse than no image at all.

- [ ] **Step 1: Start a fresh node**

```bash
docker volume rm -f dinero-test-data 2>/dev/null || true
docker rm -f dinero-test 2>/dev/null || true
docker run -d --platform linux/amd64 --name dinero-test \
  -v dinero-test-data:/data -p 20999:20999 dinerod:test
sleep 20
docker logs dinero-test 2>&1 | head -30
```

Expected: log contains `[entrypoint] datadir=/data rpcbind_default=127.0.0.1 assumeutxo_snapshot=yes assumeutxo_forward_connect=yes` (the entrypoint no longer echoes the argument vector — pre-merge correction 4; the flags themselves are asserted mechanically by the workflow's "Restart-regression check" step, which captures the argv `dinerod` actually receives). On a fresh datadir the daemon then logs `[snapshot] pending — base height 73035 ...`, and `[LoadSnapshot] Manifest trust gate passed: /opt/dinero/mainnet-snapshot.dat.manifest.json` when it imports (never `No snapshot manifest configured`).

- [ ] **Step 2: Confirm the snapshot was accepted, not rejected**

```bash
sleep 60
docker exec dinero-test dinero-cli getblockcount
docker exec dinero-test dinero-cli getblockchaininfo | grep -i "assumeutxo\|blocks"
```

Expected: `getblockcount` at or above **73035** (the snapshot base) rather than near 0. A count near 0 means the snapshot was rejected and it is syncing from genesis — investigate and report, do not proceed.

- [ ] **Step 3: Confirm peer connectivity — this is what the old ops/Dockerfile would fail**

```bash
docker exec dinero-test dinero-cli getconnectioncount
```

Expected: greater than 0, proving DNS seed discovery works from a clean container. If 0, wait another 60s and retry once; if still 0, that is a real defect — report it.

- [ ] **Step 4: Restart safety — the entrypoint's core logic**

```bash
docker stop -t 90 dinero-test && docker start dinero-test
sleep 60
docker inspect dinero-test --format '{{.State.Status}} {{.State.ExitCode}}'
docker logs dinero-test 2>&1 | grep -E "AssumeUTXO restore|existing datadir|LoadSnapshot"
```

Expected: still `running`. On a datadir already past the base:
`[snapshot] existing datadir (height N > 0) — NOT auto-loading snapshot` and **no**
`LoadSnapshot` lines — it must not re-import. Mid-sync it may instead log
`[AssumeUTXO restore] ... rehydrating from configured snapshot`, which is correct and is
the whole point of passing the flag every time.

**It must NEVER log** `Persisted AssumeUTXO metadata exists, but ... no
assumeutxo_snapshot path is configured`, and must never exit 2 — that state is
unrecoverable without deleting the volume.

The reproducing window is **not** "before the import": a restart in the first seconds is
benign. It is the minutes *after* `[LoadSnapshot]` completes but before the consensus
UTXO/forest state is durably at the base — the signature is
`utxo-tip=...@0` versus `snapshot=...@73035` on the next boot. Restart repeatedly across
the first few minutes, and to prove the failure mode is real, start `dinerod` on that
same datadir *without* `--assumeutxo_snapshot` and watch it exit 2.

- [ ] **Step 5: Data survives container removal**

```bash
docker rm -f dinero-test
docker run -d --platform linux/amd64 --name dinero-test2 -v dinero-test-data:/data dinerod:test
sleep 25
docker logs dinero-test2 2>&1 | grep entrypoint
docker exec dinero-test2 dinero-cli getblockcount
```

Expected: "existing datadir" again, and a block count at least as high as before — the named volume persisted.

- [ ] **Step 6: Clean shutdown (PID 1 signal handling)**

```bash
time docker stop dinero-test2
```

Expected: exits in a few seconds, not the 10s SIGKILL timeout. A 10s stop means signals are not reaching dinerod.

- [ ] **Step 7: Tear down and record results**

```bash
docker rm -f dinero-test dinero-test2 2>/dev/null || true
docker volume rm -f dinero-test-data
```

Record every command's actual output in the task report. No commit — this task produces evidence, not code.

---

### Task 3: CI workflow to publish on release

**Files:**
- Create: `.github/workflows/docker-publish.yml`

**Interfaces:**
- Consumes: `Dockerfile` and `docker-entrypoint.sh` from Task 1.

- [ ] **Step 1: Write the workflow**

> **The full workflow listing that used to sit here has been removed.** It had no
> `v8.*` trigger guard (so it went red on every `dinerodpi-v*` release), pushed
> `:latest` unconditionally (so a prerelease or a dispatch of an older version could move
> it backwards), and went straight from build to `push: true` with no run check. The
> shipped file is [`.github/workflows/docker-publish.yml`](../../../.github/workflows/docker-publish.yml);
> it is the only copy. Shape: job-level
> `if: github.event_name == 'workflow_dispatch' || startsWith(github.event.release.tag_name, 'v8.')`,
> a resolve step that also decides whether `:latest` is emitted, GHCR login, optional
> Docker Hub login, build with `load: true`, a run check that actually starts the image,
> then build + push.

- [ ] **Step 2: Validate the YAML parses**

```bash
cd /Users/haydarevich/src/dinero-v8-docker
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/docker-publish.yml')); print('YAML OK')"
```

Expected: `YAML OK`. If PyYAML is missing: `python3 -m pip install --user pyyaml`.

- [ ] **Step 3: Confirm the empty-tag trick does not emit a blank tag line**

The `&&/||` expressions render to `''` when Docker Hub is disabled. `build-push-action` ignores empty tag lines, but confirm the rendered value by eye: with `enabled=false`, the tags block must contain only the two `ghcr.io/...` lines plus empty strings. Note this explicitly in the report; if unsure, restructure to build the tag list in the "Resolve version" step instead.

- [ ] **Step 4: Commit**

```bash
cd /Users/haydarevich/src/dinero-v8-docker
git add .github/workflows/docker-publish.yml
git commit -m "ci(docker): publish node image to GHCR and Docker Hub on release"
git show --stat --format= HEAD
```

---

### Task 4: Put the one-liner where people will find it

**Files:**
- Modify: `README.md` (repo root)

**Interfaces:**
- Consumes: the published image name from Task 3.

`docker` currently appears **zero times** in `README.md`. The image is worthless if nobody discovers it.

- [ ] **Step 1: Find the right insertion point**

```bash
cd /Users/haydarevich/src/dinero-v8-docker
grep -n "^## " README.md | head -20
```

Insert a `## Run a Node` section immediately BEFORE the first section that discusses building from source or downloading releases. If a node/running section already exists, add the Docker block at the top of it rather than creating a duplicate section. Report which you chose.

- [ ] **Step 2: Add the section**

> **The README snippet that used to sit here has been removed.** It claimed the images
> are already published (they are not — that is an operator action) and lacked the log
> rotation flags. The shipped text is in [`README.md`](../../../README.md) under
> "Run a Node"; it is the only copy.

- [ ] **Step 3: Verify the code fences survived**

```bash
cd /Users/haydarevich/src/dinero-v8-docker
grep -c '```' README.md
sed -n "/^## Run a Node/,/^## /p" README.md | head -30
```

Expected: an even count of fences, and the section renders as written. A nested-fence mistake here silently mangles the README on GitHub.

- [ ] **Step 4: Commit**

```bash
cd /Users/haydarevich/src/dinero-v8-docker
git add README.md
git commit -m "docs: add the docker run one-liner to the README"
git show --stat --format= HEAD
```

---

## Self-Review

**Spec coverage:** "Install the official release" → Task 1 Step 2 (fetch stage). "Bundle the snapshot" → Task 1 Step 2. "Arm the snapshot unconditionally" (spec, as corrected) → Task 1 Step 1 + Task 2 Step 4. "Good network citizen" → entrypoint `-listen=1`, `EXPOSE 20999`, Task 2 Step 3. "Files" table → Tasks 1/3/4. "Publishing to both registries" → Task 3. "The one-liner" → Task 4. Spec testing items 1-7 → Task 1 Steps 3-4 and Task 2 Steps 1-6.

**Placeholder scan:** none. Every step carries the actual file content or command.

**Type consistency:** `docker-entrypoint.sh` is created at repo root in Task 1 and `COPY`d from the build context in the same Dockerfile — consistent. Image name `dinerod:test` is produced in Task 1 Step 3 and consumed throughout Task 2. Build args `DINERO_VERSION`/`SNAPSHOT_NAME` are declared before the first `FROM` and re-declared inside each stage that uses them, which is required by Docker's scoping rules.

**Known soft spot, flagged deliberately:** Task 3 Step 3 covers the conditional Docker Hub tags. The `format()`/`||` pattern is the fiddliest part of the plan and the least verifiable without actually running the workflow. If it looks wrong, building the tag list as a shell-computed output in the "Resolve version" step is the cleaner fallback.

# Official Docker Node Image — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a stranger run a validating Dinero node with one `docker run`, syncing to the tip in minutes via a bundled AssumeUTXO snapshot.

**Architecture:** Two-stage Dockerfile. Stage 1 downloads the official v8.1.1 release bundle and the AssumeUTXO snapshot from GitHub Releases and verifies both against published SHA256SUMS. Stage 2 is `debian:13-slim` (bookworm's glibc 2.36 is too old) with the one shared library `dinerod` actually needs. An entrypoint script arms the snapshot **only** on a fresh datadir, then `exec`s dinerod as PID 1.

**Tech Stack:** Docker (BuildKit), debian:12-slim, POSIX shell, GitHub Actions.

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
- **Fresh-datadir rule, copied from `qt/src/main.cpp:105-107`:** fresh = `datadir/blocks` does NOT exist AND `datadir/blockchain` does NOT exist. On a non-fresh datadir the snapshot MUST NOT be passed.
- **Snapshot args:** `--assumeutxo_snapshot=<path>` **and** `--assumeutxo_forward_connect=1`. Omitting the second holds the tip at the snapshot base for the whole background replay (PR #393) — that shipped as a real user-facing bug.
- **amd64 only.** Release assets are linux-x86_64. The dev machine is **Apple Silicon (aarch64)**, so every local docker command MUST pass `--platform linux/amd64` and will run under emulation (slow but functional).
- **Working directory:** `/Users/haydarevich/src/dinero-v8-docker` (git worktree, branch `feat/docker-node-image`). Do NOT touch `/Users/haydarevich/src/dinero-v8` — it has unrelated uncommitted work on another branch.
- **Do not modify** `ops/Dockerfile` (referenced by `scripts/release-build.sh`) or `.dockerignore`.
- **Do not push.** The operator pushes.

---

### Task 1: Dockerfile + entrypoint

**Files:**
- Create: `Dockerfile` (repo root)
- Create: `docker-entrypoint.sh` (repo root)

**Interfaces:**
- Produces: image with `/usr/local/bin/dinerod`, `/usr/local/bin/dinero-cli`, `/opt/dinero/snapshot.dat`, `/opt/dinero/snapshot.manifest.json`, entrypoint `/usr/local/bin/docker-entrypoint.sh`, `VOLUME /data`, user `dinero` (uid 10001).
- Produces: build args `DINERO_VERSION` (default `8.1.1`) and `SNAPSHOT_NAME` (default `dinero-assumeutxo-73035-v4`).

- [ ] **Step 1: Write `docker-entrypoint.sh`**

```sh
#!/bin/sh
# Entrypoint for the official Dinero node image.
#
# Arms the bundled AssumeUTXO snapshot ONLY on a fresh datadir. The rule and the
# reason are copied from qt/src/main.cpp:105 — "on an existing datadir we must NOT
# pass the snapshot (the node is past it)". --assumeutxo_forward_connect=1 must
# accompany it or the active tip is held at the snapshot base for the whole
# genesis->base replay (PR #393), which surfaces to users as 0 confirmations for hours.
set -eu

DATADIR="${DINERO_DATADIR:-/data}"
SNAPSHOT="/opt/dinero/snapshot.dat"

# Base flags: serve the network (listen), talk RPC inside the container only.
set -- \
    -datadir="$DATADIR" \
    -printtoconsole=1 \
    -listen=1 \
    -port=20999 \
    -rpcbind=0.0.0.0 \
    -rpcport=20998 \
    "$@"

if [ ! -d "$DATADIR/blocks" ] && [ ! -d "$DATADIR/blockchain" ]; then
    if [ -f "$SNAPSHOT" ]; then
        echo "[entrypoint] fresh datadir — fast-syncing from bundled AssumeUTXO snapshot"
        set -- "$@" "--assumeutxo_snapshot=$SNAPSHOT" "--assumeutxo_forward_connect=1"
    else
        echo "[entrypoint] fresh datadir but no bundled snapshot — syncing from genesis"
    fi
else
    echo "[entrypoint] existing datadir — not arming snapshot (node is past it)"
fi

echo "[entrypoint] exec dinerod $*"
exec dinerod "$@"
```

- [ ] **Step 2: Write `Dockerfile`**

```dockerfile
# syntax=docker/dockerfile:1
#
# Official Dinero full-node image.
#
# Installs the SIGNED release artifacts rather than compiling, so the container ships
# byte-identical binaries to a manual install and the build takes seconds. Bundles the
# AssumeUTXO snapshot so a fresh node reaches the tip in minutes with no multi-gigabyte
# UTXO database — the project's headline claim, true on first run.
#
#   docker run -d --name dinero -v dinero-data:/data -p 20999:20999 dinerolabs/dinerod

ARG DINERO_VERSION=8.1.1
ARG SNAPSHOT_NAME=dinero-assumeutxo-73035-v4

# ---------- stage 1: fetch + verify ----------
FROM debian:13-slim AS fetch
ARG DINERO_VERSION
ARG SNAPSHOT_NAME

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/dl
RUN set -eux; \
    BASE="https://github.com/DineroLabs/dinero-v8/releases/download/v${DINERO_VERSION}"; \
    curl -fsSL -O "${BASE}/dinero-linux-x86_64-${DINERO_VERSION}.tar.gz"; \
    curl -fsSL -O "${BASE}/SHA256SUMS-linux-x86_64-${DINERO_VERSION}"; \
    curl -fsSL -O "${BASE}/${SNAPSHOT_NAME}.dat"; \
    curl -fsSL -O "${BASE}/${SNAPSHOT_NAME}.manifest.json"; \
    curl -fsSL -O "${BASE}/SHA256SUMS-assumeutxo-73035"; \
    tar xzf "dinero-linux-x86_64-${DINERO_VERSION}.tar.gz"; \
    # verified AFTER extraction: the checksum file names the two binaries by their
    # in-tarball paths, so all three entries only resolve once unpacked
    sha256sum -c "SHA256SUMS-linux-x86_64-${DINERO_VERSION}"; \
    sha256sum -c "SHA256SUMS-assumeutxo-73035"; \
    mkdir -p /out; \
    cp "dinero-linux-x86_64-${DINERO_VERSION}/dinerod"     /out/dinerod; \
    cp "dinero-linux-x86_64-${DINERO_VERSION}/dinero-cli"  /out/dinero-cli; \
    cp "${SNAPSHOT_NAME}.dat"           /out/snapshot.dat; \
    cp "${SNAPSHOT_NAME}.manifest.json" /out/snapshot.manifest.json; \
    chmod +x /out/dinerod /out/dinero-cli

# ---------- stage 2: runtime ----------
FROM debian:13-slim
ARG DINERO_VERSION

LABEL org.opencontainers.image.title="Dinero Full Node"
LABEL org.opencontainers.image.description="Post-quantum, Utreexo-native proof-of-work full node"
LABEL org.opencontainers.image.source="https://github.com/DineroLabs/dinero-v8"
LABEL org.opencontainers.image.documentation="https://github.com/DineroLabs/dinero-v8/blob/dinero-main/README.md"
LABEL org.opencontainers.image.licenses="MIT"
LABEL org.opencontainers.image.version="${DINERO_VERSION}"

# dinerod's only DT_NEEDED beyond libc/libstdc++ is libudev. debian:13 (not 12) is
# required: the binary needs GLIBC_2.38 / GLIBCXX_3.4.32; bookworm ships glibc 2.36.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libudev1 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --create-home --home-dir /data dinero

COPY --from=fetch /out/dinerod                   /usr/local/bin/dinerod
COPY --from=fetch /out/dinero-cli                /usr/local/bin/dinero-cli
COPY --from=fetch /out/snapshot.dat              /opt/dinero/snapshot.dat
COPY --from=fetch /out/snapshot.manifest.json    /opt/dinero/snapshot.manifest.json
COPY docker-entrypoint.sh                        /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

VOLUME /data
# 20999 = P2P (publish this to serve the network). 20998 = RPC — deliberately NOT in
# the documented run command so nobody exposes RPC to the internet by copy-paste.
EXPOSE 20999/tcp 20998/tcp

USER dinero
WORKDIR /data
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
```

- [ ] **Step 3: Build it**

```bash
cd /Users/haydarevich/src/dinero-v8-docker
docker build --platform linux/amd64 -t dinerod:test .
```

Expected: build succeeds. If `apt-get install` fails on a package name, that is a real finding — report the exact name rather than guessing a substitute.

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

Expected: log contains `[entrypoint] fresh datadir — fast-syncing from bundled AssumeUTXO snapshot`, and the exec line shows both `--assumeutxo_snapshot=` and `--assumeutxo_forward_connect=1`.

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
docker restart dinero-test
sleep 20
docker logs dinero-test 2>&1 | tail -20 | grep entrypoint
```

Expected: `[entrypoint] existing datadir — not arming snapshot (node is past it)`. If it says "fresh datadir" again, the fresh-detection is broken and the node would restart sync forever — stop and report.

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

```yaml
name: Publish Docker image

on:
  release:
    types: [published]
  workflow_dispatch:
    inputs:
      version:
        description: "Dinero version to build (e.g. 8.1.1)"
        required: true

permissions:
  contents: read
  packages: write

jobs:
  publish:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Resolve version
        id: v
        run: |
          if [ -n "${{ github.event.inputs.version }}" ]; then
            V="${{ github.event.inputs.version }}"
          else
            V="${GITHUB_REF_NAME#v}"
          fi
          echo "version=$V" >> "$GITHUB_OUTPUT"
          echo "Building Dinero $V"

      - uses: docker/setup-buildx-action@v3

      - name: Log in to GHCR
        uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}

      # Docker Hub is optional: a fork or a secretless run still publishes to GHCR.
      - name: Check Docker Hub credentials
        id: dh
        run: |
          if [ -n "${{ secrets.DOCKERHUB_TOKEN }}" ]; then
            echo "enabled=true" >> "$GITHUB_OUTPUT"
          else
            echo "enabled=false" >> "$GITHUB_OUTPUT"
            echo "::notice::DOCKERHUB_TOKEN not set — publishing to GHCR only"
          fi

      - name: Log in to Docker Hub
        if: steps.dh.outputs.enabled == 'true'
        uses: docker/login-action@v3
        with:
          username: ${{ secrets.DOCKERHUB_USERNAME }}
          password: ${{ secrets.DOCKERHUB_TOKEN }}

      - name: Build and push
        uses: docker/build-push-action@v6
        with:
          context: .
          platforms: linux/amd64
          push: true
          build-args: |
            DINERO_VERSION=${{ steps.v.outputs.version }}
          tags: |
            ghcr.io/dinerolabs/dinero-v8:${{ steps.v.outputs.version }}
            ghcr.io/dinerolabs/dinero-v8:latest
            ${{ steps.dh.outputs.enabled == 'true' && format('dinerolabs/dinerod:{0}', steps.v.outputs.version) || '' }}
            ${{ steps.dh.outputs.enabled == 'true' && 'dinerolabs/dinerod:latest' || '' }}
```

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

```markdown
## Run a Node

```bash
docker run -d --name dinero -v dinero-data:/data -p 20999:20999 dinerolabs/dinerod
```

That is a full validating node. It fast-syncs from a bundled AssumeUTXO snapshot, so
it reaches the chain tip in minutes rather than replaying from genesis — and because
validation is Utreexo-native, it does so without a multi-gigabyte UTXO database on disk.

Check on it:

```bash
docker exec dinero dinero-cli getblockcount
docker exec dinero dinero-cli getconnectioncount
```

Port `20999` is P2P — publishing it lets your node accept inbound peers and serve the
network. RPC (`20998`) is deliberately **not** published above; only expose it if you
understand the consequences.

Chain data lives in the `dinero-data` volume and survives `docker rm`. Images are
published to Docker Hub (`dinerolabs/dinerod`) and GHCR
(`ghcr.io/dinerolabs/dinero-v8`). amd64 only for now.
```

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

**Spec coverage:** "Install the official release" → Task 1 Step 2 (fetch stage). "Bundle the snapshot" → Task 1 Step 2. "Arm only on fresh datadir" → Task 1 Step 1 + Task 2 Step 4. "Good network citizen" → entrypoint `-listen=1`, `EXPOSE 20999`, Task 2 Step 3. "Files" table → Tasks 1/3/4. "Publishing to both registries" → Task 3. "The one-liner" → Task 4. Spec testing items 1-7 → Task 1 Steps 3-4 and Task 2 Steps 1-6.

**Placeholder scan:** none. Every step carries the actual file content or command.

**Type consistency:** `docker-entrypoint.sh` is created at repo root in Task 1 and `COPY`d from the build context in the same Dockerfile — consistent. Image name `dinerod:test` is produced in Task 1 Step 3 and consumed throughout Task 2. Build args `DINERO_VERSION`/`SNAPSHOT_NAME` are declared before the first `FROM` and re-declared inside each stage that uses them, which is required by Docker's scoping rules.

**Known soft spot, flagged deliberately:** Task 3 Step 3 covers the conditional Docker Hub tags. The `format()`/`||` pattern is the fiddliest part of the plan and the least verifiable without actually running the workflow. If it looks wrong, building the tag list as a shell-computed output in the "Resolve version" step is the cleaner fallback.

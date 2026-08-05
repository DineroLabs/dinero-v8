# Official Docker image for the Dinero full node

**Date:** 2026-08-05
**Status:** design, pending approval
**Goal:** let a stranger run a validating Dinero node with one command, to lower the
friction on node adoption.

## Problem

Running a Dinero node today means downloading a platform tarball, extracting it,
choosing a datadir, and knowing which flags to pass. `docker` appears **zero times**
in `README.md`, and `dinerolabs.org` says "download" ~60 times and "run a node" twice.
The project's headline technical pitch — a full validating node with no multi-gigabyte
UTXO database, synced in minutes via AssumeUTXO — is not reachable in one command.

### The existing `ops/Dockerfile` does not solve this, and cannot build

- It copies `/src/build/bin/dinerod`. Binaries land in `build/`, **not `build/bin/`**
  (verified: every `build-*/` directory in the tree contains `dinerod` at its root).
  Both the `strip` at line 28 and the `COPY` at line 46 reference a path that has
  never existed.
- Its defaults are for a locked-down internal ops container, and they are the
  opposite of what a public node image needs:
  - `-listen=0` — never accepts inbound connections. Creates leech nodes that consume
    peer capacity without serving the network.
  - `-dnsseed=0` — with no `-addnode`/`-seednode` either, a fresh container has **no
    way to discover peers at all**.
  - P2P port 20999 is not exposed (only RPC 20998 and WS 21001).
  - No `VOLUME`, so chain data lives in the container layer and is destroyed by
    `docker rm`.
  - `dinero-cli` is absent, so an operator cannot inspect their own node.
  - `HEALTHCHECK` runs `dinerod --healthcheck`, which is not a dinerod flag (the only
    `healthcheck` symbols in the tree are C++ methods on the vault signing backend).
    The container would report unhealthy forever.
  - Metadata is stale: `version="v1.0.0"`, source `github.com/dinero/dinero`.

`ops/Dockerfile` is referenced by `scripts/release-build.sh`, so it is **left in place
and untouched**. This spec adds a separate, purpose-built image.

## Approach

### Install the official release; do not compile

The v8.1.1 release publishes `dinero-linux-x86_64-8.1.1.tar.gz` (14.9 MB), which
contains **both** `dinerod` and `dinero-cli`.

**Use that bundle, not the separate `dinero-core`/`dinero-cli` tarballs.** Verified
empirically: `SHA256SUMS-linux-x86_64-8.1.1` contains exactly **3 entries** — the
bundle tarball, `dinero-linux-x86_64-8.1.1/dinerod`, and
`dinero-linux-x86_64-8.1.1/dinero-cli`. The `dinero-core-*` and `dinero-cli-*`
tarballs are **not covered by any published checksum**, so they cannot be verified.
The bundle is one download, both binaries, and checksum-verifiable.

Downloading and checksum-verifying that beats compiling in-image:

- builds in seconds instead of a long C++ compile
- final image is small (~50 MB) with no build toolchain in it
- ships **the exact signed artifacts** a human would download, so the container and
  the manual install are the same binaries
- the SHA256 verification is a real supply-chain check, and it fails the build loudly

### Runtime base must be debian-slim, NOT distroless

`dinerod`'s `DT_NEEDED` list, read from the shipped v8.1.1 ELF, is:

    libminiupnpc.so.17  libnatpmp.so.1  libudev.so.1
    libstdc++.so.6  libm.so.6  libgcc_s.so.1  libc.so.6  ld-linux-x86-64.so.2

`gcr.io/distroless/cc` supplies only the last five. **A distroless image would build
successfully and then fail to start**, which is why `ops/Dockerfile`'s distroless base
is not reused. Runtime is `debian:12-slim` with `libminiupnpc17`, `libnatpmp1` and
`libudev1` installed, running as a non-root user.

Note there is no `libssl` or `libsqlite3` dependency — those are linked statically —
so the runtime layer stays small.

Trade-off: the image can only be built for platforms with published release assets.
Assets are **linux-x86_64 only**, so this image is **amd64-only**. arm64 (Raspberry Pi,
Apple Silicon) would require source-building and is deliberately out of scope; add it
if anyone asks.

### Bundle the AssumeUTXO snapshot

`dinero-assumeutxo-73035-v4.dat` is **27 MB** — small enough to bake in, with
`SHA256SUMS-assumeutxo-73035` published alongside for verification. Bundling makes the
headline claim true out of the box: a fresh `docker run` reaches the tip in minutes
without a multi-gigabyte UTXO database, with no extra steps and no first-run download.

### Arm the snapshot ONLY on a fresh datadir — this needs an entrypoint script

From `qt/src/main.cpp:105`: *"On an existing datadir we must NOT pass the snapshot
(the node is past it)."* And per PR #393, arming a snapshot without
`--assumeutxo_forward_connect=1` holds the active tip at the snapshot base for the
whole genesis→base validation, so a fresh node shows 0 confirmations for hours — the
exact bug that shipped to Qt and DineroDPI users.

So a static `CMD` is not sufficient. `docker-entrypoint.sh`:

1. If `/data` contains no existing chainstate → pass
   `--assumeutxo_snapshot=/opt/dinero/snapshot.dat --assumeutxo_forward_connect=1`
2. If `/data` is already initialised → pass neither
3. `exec` dinerod so it is PID 1 and receives signals (clean `docker stop`)

### Be a good network citizen by default

- `-listen=1`, and **expose 20999/tcp** so the node accepts inbound peers
- DNS seeding left **on** so peer discovery works out of the box
- RPC bound to `0.0.0.0` **only inside the container**, and not published by the
  documented one-liner — the README command maps the P2P port only, so RPC is not
  exposed to the internet by an operator copying a command they did not read
- `VOLUME /data` so chain data survives `docker rm`
- `dinero-cli` included, so `docker exec <c> dinero-cli getblockcount` works

## Files

| File | Purpose |
|---|---|
| `Dockerfile` (repo root) | The image. Root so `docker build .` works and people find it. |
| `docker-entrypoint.sh` | Fresh-datadir detection and snapshot arming. |
| `.github/workflows/docker-publish.yml` | Build + push on release tag. |
| `README.md` (edit) | The one-liner, in the node-running section. |

`ops/Dockerfile` and `.dockerignore` are not modified.

## Publishing

Both registries, from one workflow:

- **Docker Hub** `dinerolabs/dinerod` — the README one-liner; shortest and most
  familiar, and the registry people actually search.
- **GHCR** `ghcr.io/dinerolabs/dinero-v8` — always-available mirror, authenticates
  with the existing `GITHUB_TOKEN`.

Triggered on published release, tagged with both the release version and `latest`.
Without CI the published image goes stale the moment the next version ships and
someone has to remember to rebuild; that is exactly how an official image loses trust.

Docker Hub requires `DOCKERHUB_USERNAME` and `DOCKERHUB_TOKEN` repository secrets.
The workflow must skip the Docker Hub push (not fail the job) when they are absent, so
a fork or a secretless run still publishes to GHCR.

## The one-liner

```bash
docker run -d --name dinero -v dinero-data:/data -p 20999:20999 dinerolabs/dinerod
```

Named volume rather than a bind mount: the image runs as non-root, and a bind-mounted
host directory owned by root fails with permission denied — the most common first-run
failure for containerised nodes. A named volume inherits the image's ownership.

## Testing

Verification is the point here — an official image that does not actually sync is worse
than none.

1. `docker build .` succeeds, and fails loudly on a deliberately corrupted checksum.
2. Fresh run reaches the tip: `getblockcount` climbs past the snapshot base (73035)
   within minutes, and `getblockchaininfo` reports `assumeutxo_active`.
3. **Peer connectivity** — `getconnectioncount` > 0, proving DNS seeding works from a
   clean container. This is what the existing `ops/Dockerfile` would fail.
4. **Restart safety** — stop and restart the container; confirm the snapshot is NOT
   re-armed on the now-populated datadir, and the node resumes rather than restarting
   sync. This is the entrypoint's core logic and the one most likely to regress.
5. Data survives `docker rm` + re-run against the same named volume.
6. `docker exec dinero dinero-cli getblockcount` works.
7. `docker stop` exits promptly and cleanly (signal handling / PID 1).

## Out of scope

arm64; mining in the image; the Qt wallet; publishing a `docker-compose.yml`; changing
`ops/Dockerfile`; anything on dinerolabs.org.

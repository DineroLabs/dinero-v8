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

**`ops/Dockerfile` was deleted** (2026-08-05). Investigation showed nothing built it:
`scripts/release-build.sh` only *printed* `docker build -f ops/Dockerfile .` as a
suggested next step, and no workflow referenced it. Fixing its `build/bin/dinerod`
path would still have left a distroless image missing `libudev.so.1`, so it would
have built and then failed to start. The script now points at this image instead.

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
- the image stays small with no build toolchain in it: **141 MB unpacked**, measured
  with `du -sb / --exclude=/proc --exclude=/sys --exclude=/data` inside a running
  container, of which ~65 MB is Dinero's own content on top of `debian:13-slim`
  (33 MB `dinerod`, 27 MB snapshot, 4.7 MB apt layer). Two other numbers are visible
  for the same build and neither is the unpacked size: `docker images` displays
  ~212 MB, and `docker inspect --format '{{.Size}}'` reports 59.5 MB — the spread is
  BuildKit attestation-manifest accounting, not three different images.
- ships **the exact release artifacts** a human would download, so the container and
  the manual install are the same binaries
- the SHA256 verification fails the build loudly

Do not describe these as *signed* artifacts. There are no signatures: no cosign, gpg
or sigstore step exists in any workflow, and the repo publishes no attestations. What
exists is `SHA256SUMS`, fetched over the same TLS connection from the same host as the
artifacts it covers — a same-channel checksum, which detects corruption and a partial
upload but not a compromised release host.

The snapshot is the genuinely stronger half of that asymmetry, and it is worth stating
explicitly: `dinerod` carries a **compiled-in trust anchor** for the height-73035
snapshot (`src/consensus/assume_utxo.cpp`), so the snapshot's SHA256 and base block
hash are verified against a constant inside the binary — independently of the channel
the file arrived over. The binaries themselves have same-channel checksums only.

### Runtime base must be debian-slim, NOT distroless

`dinerod`'s `DT_NEEDED` list, read from the shipped v8.1.1 ELF, is:

    libudev.so.1
    libstdc++.so.6  libm.so.6  libgcc_s.so.1  libc.so.6  ld-linux-x86-64.so.2

`gcr.io/distroless/cc` supplies everything except `libudev.so.1`. **A distroless image
would build successfully and then fail to start**, which is why `ops/Dockerfile`'s
distroless base is not reused. Runtime is `debian:13-slim` with `libudev1` installed,
running as a non-root user.

`debian:13`, not `debian:12`: the released binary needs `GLIBC_2.38` /
`GLIBCXX_3.4.32`, which debian:12's glibc 2.36 does not provide.

There is **no** `libminiupnpc`/`libnatpmp` dependency — the v8.1.1 Linux release lane
pins the flag set that previously leaked those in (see `linux-release.yml`), so do not
install them "just in case"; they are dead weight in the runtime layer.

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

**Bundle the manifest as the snapshot's sibling, under the manifest's own filename.**
The daemon probes `<snapshot_path>.manifest.json`, and `ValidateSnapshotManifestPreflight`
compares the on-disk filename with the manifest's `snapshot_file` field. The published
manifest declares `snapshot_file: "mainnet-snapshot.dat"`, so the image installs the
snapshot as `/opt/dinero/mainnet-snapshot.dat` with the manifest at
`/opt/dinero/mainnet-snapshot.dat.manifest.json`. Any other naming either silently
downgrades to `[WARNING] No snapshot manifest configured` (manifest not found) or hard-
fails the load with `Snapshot manifest snapshot_file mismatch` (manifest found, name
wrong). The install name is an `ARG` and the build asserts it equals the manifest's
`snapshot_file`, so drift fails the build instead of every fresh container.

**Pin the snapshot's source release independently of the daemon version.** The
assumeutxo assets (`.dat`, `.manifest.json`, `SHA256SUMS-assumeutxo-73035`) were a
one-off upload to v8.1.1; no workflow produces them, and v8.0.18 has none. Fetching
them from `v${DINERO_VERSION}` would therefore 404 on the next release. `ARG
SNAPSHOT_RELEASE=v8.1.1` fetches them from the release that actually has them, while
the binaries still come from the version being built. The checksum file's height is
derived from `SNAPSHOT_NAME` rather than hardcoded, so the two cannot drift.

### Arm the snapshot UNCONDITIONALLY and let the daemon decide

An earlier revision of this spec said "arm only on a fresh datadir", quoting
`qt/src/main.cpp:105`: *"On an existing datadir we must NOT pass the snapshot (the node
is past it)."* **That rule is wrong for the daemon and it bricks volumes.** The daemon
has since grown a restart-restore path (`chainstate_service.cpp`, `[AssumeUTXO
restore]`): when persisted AssumeUTXO metadata exists but the consensus UTXO state is
not yet at the snapshot base, it *rehydrates from the configured `assumeutxo_snapshot`*
— and if no path is configured, it logs

    Persisted AssumeUTXO metadata exists, but consensus UTXO state is not at the
    snapshot base and no assumeutxo_snapshot path is configured

and exits 2, on that start and on every start after it. There is no recovery short of
deleting the volume. A container restarted during first-run sync — host reboot,
`docker compose down/up`, Ctrl-C, OOM, or `--restart unless-stopped` turning it into a
crash loop — lands exactly there, because the daemon creates `/data/blocks` and
`/data/blockchain` seconds into startup while the snapshot import only happens once
headers reach height 73035, minutes later. Any "does the datadir look fresh?" heuristic
based on those directories is therefore false for the whole window in which it matters.

So `docker-entrypoint.sh` passes `--assumeutxo_snapshot=$DINERO_SNAPSHOT
--assumeutxo_forward_connect=1` on **every** start, and the daemon's own preconditions
decide:

1. fresh datadir → deferred bootstrap arms (`[snapshot] pending`), imports when headers
   reach the base
2. existing datadir already past the base → `[snapshot] existing datadir (height N > 0)
   — NOT auto-loading snapshot`; the flag is inert, no re-import
3. existing datadir mid first-run sync → the restore path rehydrates from the flag
   instead of dying
4. a different-base or mismatched snapshot is still rejected loudly by the daemon's own
   belts (base-height/hash peek, genesis-only UTXO precondition, manifest trust gate).
   Note the behaviour change this implies for anyone mounting an existing bare-metal
   datadir that was bootstrapped from a *different* snapshot (e.g. the 65300 anchor from
   v8.0.17): the daemon now refuses to start rather than silently ignoring the configured
   snapshot. That is the intended, safe outcome — but it is a refusal, not a warning.
5. `exec` dinerod so it is PID 1 and receives signals (clean `docker stop`)

`--assumeutxo_forward_connect=1` must accompany it: per PR #393, arming a snapshot
without it holds the active tip at the snapshot base for the whole genesis→base
validation, so a fresh node shows 0 confirmations for hours — the exact bug that
shipped to Qt and DineroDPI users.

### Be a good network citizen by default

- `-listen=1`, and **expose 20999/tcp** so the node accepts inbound peers
- DNS seeding left **on** so peer discovery works out of the box
- RPC bound to **`127.0.0.1` inside the container** (pre-merge correction 5;
  `DINERO_RPCBIND` is the documented opt-in for operators who front it themselves), not
  published by the documented one-liner, and **not `EXPOSE`d at all**. Unlike Bitcoin
  Core there is no `rpcallowip` gate — `rpc_service.cpp` takes `rpcbind` literally — so
  `rpcbind` *is* the access control. Binding `0.0.0.0` made RPC reachable from every
  other container on the same Docker network regardless of published ports, which
  `EXPOSE`/`-p` does nothing to prevent; and `docker run -P` publishes every `EXPOSE`d
  port on `0.0.0.0`, so `EXPOSE 20998` would additionally put it on the host.
  `docker exec` runs in the container's own network namespace and reaches RPC anyway.
- `VOLUME /data` so chain data survives `docker rm`
- `dinero-cli` included, so `docker exec <c> dinero-cli getblockcount` works

## Files

| File | Purpose |
|---|---|
| `Dockerfile` (repo root) | The image. Root so `docker build .` works and people find it. |
| `docker-entrypoint.sh` | Base flags and unconditional snapshot arming. |
| `Dockerfile.dockerignore` | Per-Dockerfile context override; the root `.dockerignore` blanket-ignores `*.sh`. |
| `.github/workflows/docker-publish.yml` | Build + run-check + push on a `v8.*` release. |
| `README.md` (edit) | The one-liner, in the node-running section. |

The root `.dockerignore` is not modified. `ops/Dockerfile` was deleted as dead,
unbuildable code — see above.

## Publishing

Both registries, from one workflow:

- **GHCR** `ghcr.io/dinerolabs/dinero-v8` — the README one-liner (pre-merge correction
  1), pinned to an explicit version tag; authenticates with the existing `GITHUB_TOKEN`,
  so it is the one registry that is always published to.
- **Docker Hub** `dinerolabs/dinerod` — secondary mirror, pushed only when the optional
  credentials are configured. The namespace is not registered today, so it must not
  appear as a copy-pasteable command anywhere.

Triggered on published release. Without CI the published image goes stale the moment
the next version ships and someone has to remember to rebuild; that is exactly how an
official image loses trust.

**Also triggered on pull request, build-and-run only** (pre-merge correction 2).
Previously nothing tested the image until a release, in front of users. The PR path
builds, loads, and runs the same checks, but every registry-login and push step is
gated on `github.event_name != 'pull_request'`, so it holds no credentials and cannot
push. It is path-filtered to the files that can actually change the image, because the
image is assembled from published release artifacts rather than from this tree's source.

**Guard the trigger on `v8.*`.** The repo also publishes `dinerodpi-v*` releases, which
carry no `dinero-linux-x86_64-*` asset; `${GITHUB_REF_NAME#v}` leaves such a tag intact
and the build would curl a 404 and go red on every DPI release. `on: release` cannot be
tag-filtered, so the job carries
`if: github.event_name == 'workflow_dispatch' || startsWith(github.event.release.tag_name, 'v8.')`
— the same intent as `linux-release.yml`'s `tags: ['v8.*']`.

**Gate `:latest` separately.** It is pushed only for a real, non-draft, non-prerelease
`v8.*` release. A `workflow_dispatch` rebuild of an older version pushes the version tag
only, so `latest` can never be moved backwards by a manual run.

**Run the binary before pushing.** The runner is native amd64, so `load: true`, then
`docker run --rm --entrypoint /usr/local/bin/dinerod <img> --version`, then push. A
build check alone would have shipped this branch's own missing-shared-library bug; the
image only has to start once to catch that class.

Docker Hub requires `DOCKERHUB_USERNAME` and `DOCKERHUB_TOKEN` repository secrets.
The workflow must skip the Docker Hub push (not fail the job) when they are absent, so
a fork or a secretless run still publishes to GHCR.

## The one-liner

```bash
docker run -d --name dinero --stop-timeout 60 \
  --log-opt max-size=50m --log-opt max-file=3 \
  -v dinero-data:/data -p 20999:20999 ghcr.io/dinerolabs/dinero-v8:8.1.1
```

`--stop-timeout 60`: a clean shutdown takes roughly 12 seconds and Docker's default
grace period is 10, so the default would `SIGKILL` the daemon mid-shutdown against a
live chain DB on every stop.

`--log-opt`: the node logs at roughly 0.8-1 GB/hour (measured twice: 41 MB in ~3 minutes,
and 39,184,716 bytes / 630,084 lines in 2 min 26 s) and Docker's default `json-file`
driver never rotates. A
README that promises no multi-gigabyte UTXO database must not quietly write
multi-gigabyte logs instead.

This one-liner appears in three places — README, the `Dockerfile` header comment, and
here. Keep them byte-identical.

Named volume rather than a bind mount: the image runs as non-root, and a bind-mounted
host directory owned by root fails with permission denied — the most common first-run
failure for containerised nodes. A named volume inherits the image's ownership.

## Bumping the bundled snapshot is a breaking change

The entrypoint arms `--assumeutxo_snapshot` unconditionally — that is what prevents the
interrupted-first-sync brick, where a restart before import dropped the flag and left the
daemon unable to rehydrate. The cost of that choice shows up on a snapshot bump.

An existing volume already carries persisted AssumeUTXO metadata for the old base height.
Raising `SNAPSHOT_RELEASE`/`SNAPSHOT_NAME` hands it a snapshot with a *different* base, and
`chainstate_service.cpp` refuses: *"another snapshot lifecycle is active (base height N);
configured assumeutxo_snapshot has a different base"* → **exit 2 on every start**. Waiting
does not help: `AssumeUtxoLifecycle::Disable()` has no callers in the tree, so a volume
never leaves the active state by itself.

Operators can recover, or pre-empt it, by starting without the bundled snapshot:

```bash
docker run -e DINERO_SNAPSHOT=/nonexistent ...
```

The entrypoint then takes its no-snapshot branch, and a node already past the base simply
continues as a normal full node.

**Therefore: treat a snapshot bump as a breaking change for existing installs.** Ship it
with release notes stating the above, or behind an image tag existing volumes are not
automatically pulled onto. It is not a routine version bump.

## Testing

Verification is the point here — an official image that does not actually sync is worse
than none.

1. `docker build .` succeeds, and fails loudly on a deliberately corrupted checksum.
2. Fresh run reaches the tip: `getblockcount` climbs past the snapshot base (73035)
   within minutes, and `getblockchaininfo` reports `assumeutxo_active`.
3. **Peer connectivity** — `getconnectioncount` > 0, proving DNS seeding works from a
   clean container. This is what the existing `ops/Dockerfile` would fail.
4. **Restart safety, already-synced** — stop and restart a container whose datadir is
   already past the snapshot base, with the flag present (it always is). The node must
   resume, log `[snapshot] existing datadir (height N > 0) — NOT auto-loading snapshot`,
   and neither re-import nor error.
5. **Restart safety during first-run sync** — the regression that matters. Fresh volume,
   restart the container repeatedly across the first few minutes. The node must survive
   every restart and still reach the tip. It must never log `no assumeutxo_snapshot path
   is configured` or exit 2; that state is unrecoverable without deleting the volume.

   Aim at the right window. A restart in the first seconds is **benign** — no AssumeUTXO
   metadata has been persisted yet, and the node simply re-arms the deferred bootstrap.
   The window that reproduces is the minutes *after* `[LoadSnapshot]` completes but
   before the consensus UTXO / forest state is durably at the base; its signature on the
   next boot is `[AssumeUTXO restore] Consensus UTXO set is not at snapshot base
   (utxo-tip=...@0, snapshot=...@73035)`. Measured: a restart at t=31s on a fresh volume
   landed there.

   Prove the failure mode is real rather than assuming it: start `dinerod` on that same
   datadir *without* `--assumeutxo_snapshot` — what the old "existing datadir" branch
   did — and watch it exit 2 in about a second.
6. **Manifest trust gate engages** — a fresh run logs `[LoadSnapshot] Manifest trust
   gate passed: /opt/dinero/mainnet-snapshot.dat.manifest.json` and does NOT log
   `No snapshot manifest configured`.
7. Data survives `docker rm` + re-run against the same named volume.
8. `docker exec dinero dinero-cli getblockcount` works.
9. `docker stop` exits promptly and cleanly (signal handling / PID 1).

## Out of scope

arm64; mining in the image; the Qt wallet; publishing a `docker-compose.yml`; changing
anything on dinerolabs.org.

# Dinero Core RC ceremony

**Phase E.6 final.** End-to-end recipe for cutting a signed
release candidate. Combines:

- Phase E.3 packaging contract (`dinero-deb-verify --static`
  + `--installed`)
- Phase E.4 release signing (`dinero-sign-release`)
- Phase E.5 CVE response policy reference

This doc is the runbook the maintainer follows on every release
and every patch release. The signing key flow assumes the key is
live (commit `25ee9cec6`); pre-key state is dry-run only.

## Prerequisites

- A clean Linux build host with `debhelper`, `devscripts`, `dpkg-dev`,
  `cmake`, `g++`, `pkg-config`, `perl`, `zlib1g-dev`. Currently:
  `/tmp/dinero-e3/src` on VA (Ubuntu 24.04).
- A clean Linux verification host with `binutils`, `python3`, `gpg`.
  Currently: `/tmp/dinero-e3/chroot` on VA (debootstrap noble).
- The maintainer's local Mac with the signing key in
  `~/.gnupg-dinero-release/` and `gh` CLI authenticated to
  `DineroLabs/dinero-releases`.
- Repo HEAD that's already on `origin/p2p-fix` (or the chosen
  release branch).

## The ceremony

Six phases. Each phase has a hard-fail gate. Stop at the first
failure — never ship an artifact that didn't clear every gate.

### 1. Build

On the Linux build host, fresh checkout at the chosen commit:

```bash
ssh va
cd /tmp/dinero-e3/src
git fetch origin <release-branch>
git reset --hard origin/<release-branch>
rm -rf obj-x86_64-linux-gnu debian/dinero-core debian/.debhelper
dpkg-buildpackage -us -uc -b -j$(nproc)
```

**Gate:** `dpkg-buildpackage` exits 0. The .deb appears at
`/tmp/dinero-e3/dinero-core_<VERSION>-1_amd64.deb`. Build time
is ~25 min on an 8-core VPS.

### 2. Static contract gate

On the Linux build host (or any host with `binutils`):

```bash
share/scripts/dinero-deb-verify --static \
    /tmp/dinero-e3/dinero-core_<VERSION>-1_amd64.deb
```

**Gate:** `PASS: dinero-deb-verify — all hard contract checks
green.` Zero `(with N skipped)`. Anything else stops the
ceremony.

### 3. Installed contract gate

On the verification chroot (or any clean Linux env):

```bash
ssh va
cp /tmp/dinero-e3/dinero-core_<VERSION>-1_amd64.deb \
   /tmp/dinero-e3/chroot/tmp/
cp /Users/haydarevich/src/dinero/share/scripts/dinero-deb-verify \
   /tmp/dinero-e3/chroot/tmp/
mount --bind /proc /tmp/dinero-e3/chroot/proc 2>/dev/null
mount --bind /dev  /tmp/dinero-e3/chroot/dev  2>/dev/null
chroot /tmp/dinero-e3/chroot /bin/bash -c "
    dpkg -i /tmp/dinero-core_<VERSION>-1_amd64.deb
    /tmp/dinero-deb-verify --installed
"
umount /tmp/dinero-e3/chroot/dev /tmp/dinero-e3/chroot/proc
```

**Gate:** `PASS: dinero-deb-verify — all hard contract checks
green.` Zero `(with N skipped)`. Both `dinero-cli health` (FAILING
+ exit 2 against an unstarted daemon is correct) and
`dinero-cli version --json` schema must validate.

### 4. Stage the release bundle

On the **maintainer's local Mac** (the only host with the signing
key):

```bash
mkdir -p ~/dinero-rc-staging/<TAG>
cd ~/dinero-rc-staging/<TAG>
scp va:/tmp/dinero-e3/dinero-core_<VERSION>-1_amd64.deb .
# Ship the public signing key alongside artifacts so external
# verification doesn't depend on the source repo's branch state.
cp /Users/haydarevich/src/dinero/contrib/keys/dinero-core-release.asc .
ls -lh
```

The bundle for now: the .deb + the public key. (Source tarball +
tarball-install script join in Phase G.)

### 5. Sign

Still on the maintainer's Mac:

```bash
GNUPGHOME=$HOME/.gnupg-dinero-release \
    /Users/haydarevich/src/dinero/share/scripts/dinero-sign-release \
    ~/dinero-rc-staging/<TAG>
```

The script:
- Reads the live fingerprint from `contrib/keys/RELEASE-KEY-FINGERPRINT.txt`
- Generates `SHA256SUMS` (sorted reproducible) over the bundle
- Calls `gpg --local-user <FP> --detach-sign --armor` → `SHA256SUMS.asc`
- Verifies its own output before declaring success

You'll be prompted for your signing-key passphrase.

**Gate:** `PASS: signed checksums produced` and gpg's `Good signature`
verification line.

### 6. Publish (operator-owned)

The maintainer reviews the staged bundle, then publishes via `gh`:

```bash
cd ~/dinero-rc-staging/<TAG>
gh release create <TAG> \
    --repo DineroLabs/dinero-releases \
    --title "<TAG>: <one-line summary>" \
    --notes-file release-notes.md \
    --prerelease \
    dinero-core_<VERSION>-1_amd64.deb \
    dinero-core-release.asc \
    SHA256SUMS \
    SHA256SUMS.asc
```

(Drop `--prerelease` for stable releases; keep it for `-rc<N>`
candidates so they don't show as the latest stable.)

After publishing, **verify externally** before announcing — the
release path is only proven once you can verify it as someone
without your local environment:

```bash
mkdir /tmp/external-verify && cd /tmp/external-verify
BASE=https://github.com/DineroLabs/dinero-releases/releases/download/<TAG>
for f in dinero-core-release.asc dinero-core_<VERSION>-1_amd64.deb SHA256SUMS SHA256SUMS.asc; do
    curl -sLO $BASE/$f
done
GNUPGHOME=$(mktemp -d) && export GNUPGHOME
gpg --import dinero-core-release.asc
gpg --verify SHA256SUMS.asc SHA256SUMS
sha256sum -c SHA256SUMS
```

All three commands MUST succeed.

**Release-notes template** (paste into `release-notes.md` first):

```markdown
## Release artifacts

- `dinero-core_<VERSION>-1_amd64.deb` — Ubuntu 24.04 LTS and newer packaged-service .deb
- `SHA256SUMS` — sha256 over each artifact in this release
- `SHA256SUMS.asc` — detached PGP signature over `SHA256SUMS`

## Verification

```
gpg --import https://raw.githubusercontent.com/DineroLabs/Dinero-Coin/main/contrib/keys/dinero-core-release.asc
gpg --fingerprint "Dinero Core Release Signing"
# Must match: 4ED3 65CE 6604 B722 D281  EC77 3A61 4979 B8A4 8C02

gpg --verify SHA256SUMS.asc SHA256SUMS
sha256sum -c SHA256SUMS
sudo dpkg -i dinero-core_<VERSION>-1_amd64.deb
```

## Contract gates

This release passed:
- `dinero-deb-verify --static`
- `dinero-deb-verify --installed`

See `docs/operations/deb-verify-installed.md` for what those
gates check.

## Changelog

[paste git log since last tag]
```

## What this ceremony does NOT cover

- **Tag management.** Whether the `<TAG>` is `v2.2.5-rc1` or
  `v2.2.5` is a release-decision question. Pre-1.0 patch releases
  use `-rc<N>` suffixes; once 1.0 ships, plain `vX.Y.Z`.
- **Source-tarball signing.** Phase G will add a release-source
  tarball; for now we ship .deb only.
- **Apt-repo distribution.** Operators install via `dpkg -i`
  manually for 1.0. An apt repo is post-1.0.
- **Reproducible builds.** v1.0 ships signed-by-author + signed
  checksums. Bit-for-bit reproducible-by-third-parties is v2 of
  the release process.

## Compromise scenarios

If this ceremony surfaces a problem at any gate, **stop**. Don't
ship "with one known issue" — operators don't track which release
is good and which is degraded. Cut a fresh RC.

If a release is published and a CVE later affects a bundled lib,
follow the SLA in `docs/security/cve-response.md`.

If the signing key is compromised at any point, follow
`docs/security/release-signing.md` § "Compromise response".

## Decision log

- **2026-05-02:** Initial ceremony recipe. Built around the live
  Phase E.3 verifier + Phase E.4 signing flow that landed on
  `p2p-fix`. First end-to-end run will be the v2.2.5 RC1.

# Dinero Core Release Signing — Runbook (Phase E.4)

**Status:** Phase E.4 runbook landed 2026-05-02. The actual key has
**not yet been generated** — that's a one-time operator-owned step
(see "[OPERATOR] Generate the key" below). Until then,
`share/scripts/dinero-sign-release` runs in dry-run mode and the
placeholder fingerprint at `contrib/keys/dinero-core-release.asc`
documents the expected layout.

E.6 final consumes this key to sign `SHA256SUMS.asc` for every
release artifact. Without the key, E.6 cannot ship.

## Scope

This document is the maintainer's runbook for:
- Generating the dedicated `Dinero Core Release Signing` key.
- Publishing the public key + fingerprint.
- Signing release artifacts (`SHA256SUMS.asc` first; `.deb` itself
  is a later phase).
- Operator verification recipes.

**The private key never leaves operator-owned hardware.** Anything
this runbook describes that touches the secret material is marked
`[OPERATOR]` and is the maintainer's responsibility. Everything
else (placeholder files, signing scripts, verification recipes,
documentation) lives in this repo and ships with releases.

## Key policy (locked)

| Decision | Value |
|---|---|
| Identity (uid) | `Dinero Core Release Signing <haydarevich69@icloud.com>` |
| Primary key algorithm | Ed25519 (cv25519 for any encryption subkey if added) |
| Primary key role | Certify only (`C`) — no signing, no encryption |
| Subkey | Ed25519 signing-only (`S`) — used for release artifacts |
| Subkey expiry | 2 years; rotate before expiry per "Rotation" below |
| Primary key custody | OFFLINE on a dedicated removable medium; mounted only when issuing/revoking subkeys |
| Subkey custody | On the maintainer's signing host (could later become a smartcard) |
| Publication paths | `contrib/keys/dinero-core-release.asc` (in this repo); fingerprint in `dinero-releases` README; fingerprint in every GitHub release body |
| Initial signed artifact | `SHA256SUMS.asc` per release |
| Future signed artifact | `dinero-core_*.deb` directly via `dpkg-sig` (post-1.0; SHA256SUMS.asc is the immediate gate) |
| Secondary contact | Phase E.4 acceptance check; see "Maintainer absence" |

The choices above are settled — they're the contract a verifier
recipe (below) tests against. Any change is a contract change.

## [OPERATOR] Generate the key

Run **once**, on an offline / air-gapped host if possible. **Do not
run on a fleet node, on shared infra, or inside a container that
syncs `~/.gnupg` to remote storage.** Output: a `~/.gnupg/`
directory containing the new key, plus an exported public-key file.

### Step 1 — bootstrap

```bash
# On an offline host with gpg ≥ 2.2.
gpg --version

export GNUPGHOME=$(mktemp -d)
chmod 700 "$GNUPGHOME"
```

Using a fresh `GNUPGHOME` keeps generation isolated from the
operator's existing keyring; merge into the durable keyring after
verification.

### Step 2 — primary certifying key

```bash
gpg --quick-generate-key \
    "Dinero Core Release Signing <haydarevich69@icloud.com>" \
    ed25519 cert never
```

`cert never` makes the primary expiry-less (offline keys don't need
expiry — revocation certificates handle compromise). The signing
subkey is what carries an expiry.

Capture the resulting fingerprint:

```bash
gpg --list-keys --with-colons --fingerprint \
    "Dinero Core Release Signing" \
    | awk -F: '/^fpr:/ {print $10; exit}'
```

This 40-char hex string is the **primary fingerprint**. Treasure it.

### Step 3 — signing subkey

```bash
gpg --quick-add-key "<PRIMARY-FINGERPRINT>" ed25519 sign 2y
```

Replace `<PRIMARY-FINGERPRINT>` with the value from step 2.
The subkey expires in 2 years; rotate before then.

### Step 4 — generate revocation certificate

Critical — without this you cannot revoke the key if the secret
material is compromised:

```bash
gpg --output /path/to/offline/storage/dinero-core-revoke.asc \
    --gen-revoke "<PRIMARY-FINGERPRINT>"
```

Store the revocation certificate **separately** from the key
material (different physical medium ideally). If the primary key
is destroyed, this certificate is the only way to mark it revoked
on key servers.

### Step 5 — export public key

```bash
gpg --armor --export "<PRIMARY-FINGERPRINT>" \
    > /path/to/this/repo/contrib/keys/dinero-core-release.asc
```

Replace the placeholder file. Commit it (it's public material).

### Step 6 — split primary from signing subkey

Move the primary secret offline; keep only the signing subkey on
the signing host:

```bash
# On the offline host: export the signing-subkey secret only.
gpg --output dinero-core-signing-subkey.asc \
    --armor --export-secret-subkeys \
    "<PRIMARY-FINGERPRINT>!"  # the ! restricts to this key

# Transport `dinero-core-signing-subkey.asc` to the signing host
# via removable media. NEVER over the network in plaintext.

# On the signing host:
gpg --import dinero-core-signing-subkey.asc
gpg --delete-secret-keys "<PRIMARY-FINGERPRINT>"  # removes primary secret
gpg --list-secret-keys "<PRIMARY-FINGERPRINT>"     # confirm: 'sec#' for primary
```

The `sec#` (with hash) on the primary line confirms the primary
secret is no longer on the signing host — only its public material
and the signing subkey's secret remain. This is the configuration
that ships day-to-day signing without exposing the primary.

### Step 7 — record the fingerprint here

After generation, replace the placeholder fingerprint in
`contrib/keys/RELEASE-KEY-FINGERPRINT.txt` and in
`docs/security/release-signing.md` (this file, "Published
fingerprint" section below). The fingerprint becomes part of the
public contract — operators verify against it.

## Published fingerprint

**Generated 2026-05-02.**

```
4ED3 65CE 6604 B722 D281  EC77 3A61 4979 B8A4 8C02
```

Single-line form (consumed by `share/scripts/dinero-sign-release`):

```
4ED365CE6604B722D281EC773A614979B8A48C02
```

The pretty form (with spaces) is what operators visually compare
against — paste it into every `dinero-releases` GitHub release body
so an operator on a fresh VPS can cross-check independently of
this repo. The single-line form is what tools consume.

Identity (uid): `Dinero Core Release Signing <haydarevich69@icloud.com>`.
Primary algorithm: Ed25519 cert-only, no expiry. Signing subkey:
Ed25519, 2-year expiry — first rotation due 2028-05-02.

Revocation certificate stored offline by the operator (NOT in this
repo, NOT on the signing host's main disk). If the primary is ever
compromised, importing that certificate + publishing it is the
break-glass step.

## Signing flow (E.6 final)

`share/scripts/dinero-sign-release` automates the per-release
signing step. Inputs: a directory containing the artifacts to sign
(typically the `.deb` plus any tarball). Outputs: `SHA256SUMS` +
`SHA256SUMS.asc`.

```bash
# After dpkg-buildpackage produces the .deb:
share/scripts/dinero-sign-release /path/to/artifacts/
```

The script:
1. Reads the expected fingerprint from
   `contrib/keys/RELEASE-KEY-FINGERPRINT.txt`. If the file still
   contains the placeholder, runs in **dry-run mode** — emits the
   `SHA256SUMS` text to stdout but does NOT call `gpg --sign`.
2. Generates `SHA256SUMS` over every regular file in the input
   directory (sorted by path for reproducibility).
3. Calls `gpg --local-user <FINGERPRINT> --detach-sign --armor`
   to produce `SHA256SUMS.asc`.
4. Verifies the signature with `gpg --verify` before declaring
   success.

The dry-run mode means CI/CD can exercise the script without
having the signing key on its host — useful for the maintainer's
laptop validation pass before the real signing run.

## Operator verification recipe

What an operator runs after downloading a release:

```bash
# 1. Fetch the public key and verify its fingerprint.
wget https://raw.githubusercontent.com/DineroLabs/Dinero-Coin/main/contrib/keys/dinero-core-release.asc
gpg --import dinero-core-release.asc
gpg --fingerprint "Dinero Core Release Signing"
# Compare the printed fingerprint against the one in this doc
# (and at the dinero-releases GitHub release body). MUST match.

# 2. Download the release artifacts + signed checksums.
wget https://github.com/DineroLabs/dinero-releases/releases/download/<TAG>/dinero-core_<VERSION>_amd64.deb
wget https://github.com/DineroLabs/dinero-releases/releases/download/<TAG>/SHA256SUMS
wget https://github.com/DineroLabs/dinero-releases/releases/download/<TAG>/SHA256SUMS.asc

# 3. Verify signature.
gpg --verify SHA256SUMS.asc SHA256SUMS
# MUST report "Good signature from Dinero Core Release Signing".

# 4. Verify checksum.
sha256sum -c SHA256SUMS
# MUST list dinero-core_<VERSION>_amd64.deb: OK.

# 5. Install + verify the package contract (Phase E.3 gate).
sudo dpkg -i dinero-core_<VERSION>_amd64.deb
share/scripts/dinero-deb-verify --installed
# MUST report PASS.
```

Steps 1, 3, 4, 5 are independently load-bearing — skipping any of
them weakens the chain.

## Rotation

The signing subkey expires at 2 years. **Rotate at least 30 days
before expiry** to give operators time to pull the new key.

Rotation steps (operator-owned):

1. Mount the primary key from offline storage.
2. `gpg --quick-add-key <PRIMARY-FP> ed25519 sign 2y` — generates
   a new signing subkey.
3. Re-export the public key:
   `gpg --armor --export <PRIMARY-FP> > contrib/keys/dinero-core-release.asc`.
4. Re-export the signing subkey secret:
   `gpg --export-secret-subkeys <PRIMARY-FP>!` → transport to
   signing host → import → delete primary secret.
5. Commit the new `.asc` and re-publish to the GitHub release page.
6. Optionally: revoke the old subkey via the primary
   (`gpg --edit-key <PRIMARY-FP> → key <SUBKEY> → revkey`).

The **primary fingerprint never changes** across subkey rotations,
so operator verification recipes don't break.

## Compromise response

If the signing-subkey secret is compromised:

1. **Immediately** revoke the subkey from the primary
   (offline host required):
   `gpg --edit-key <PRIMARY-FP> → key <SUBKEY-ID> → revkey`.
2. Generate a new signing subkey (rotation flow above).
3. Publish the revocation:
   - Push the updated `dinero-core-release.asc` (now contains
     the revocation signature).
   - Post a security advisory on `dinero-releases` listing every
     release signed by the compromised subkey + an instruction
     to operators to re-verify against the new subkey.

If the **primary** is compromised: import the revocation certificate
from offline storage, publish to keyservers, and start fresh —
generate a new primary, post a coordinated announcement, accept that
trust resets at this point.

## Maintainer absence

Per `docs/security/cve-response.md` § "Maintainer absence", a
critical-CVE response cannot wait on the maintainer's calendar.
Mitigations specific to release signing:

- **Designate a secondary signer** with publish access to
  `dinero-releases` and a copy of the signing subkey. Custody
  decision is operator-owned; one option is for the secondary to
  hold their own signing subkey under the same primary
  (`gpg --quick-add-key` produces multiple co-equal signing
  subkeys; both are valid signers).
- **Document the secondary's fingerprint** alongside the primary
  fingerprint in this file. Operators verify against either.
- **Pre-stage the build/sign/publish runbook** so the secondary
  can execute without deep context (E.6 covers this).

This is a Phase E.4 acceptance check: secondary signer arranged
before E.6 ships.

## Decision log

- **2026-05-02:** Initial runbook + placeholder publication
  files (`contrib/keys/dinero-core-release.asc`,
  `contrib/keys/RELEASE-KEY-FINGERPRINT.txt`) +
  `share/scripts/dinero-sign-release` in dry-run mode. Key not
  yet generated; that's the next operator-owned step. E.6 final
  blocked on this.
- **2026-05-02 (key generated):** Operator generated the key.
  Fingerprint: `4ED365CE6604B722D281EC773A614979B8A48C02`. Initial
  uid email used the non-deliverable form
  `releases@haydarevich69.icloud.com`. Same fingerprint, but
  metadata corrected via `gpg --quick-add-uid` /
  `--quick-set-primary-uid` / `--quick-revoke-uid` to use the
  operator's deliverable address `haydarevich69@icloud.com` as
  the primary uid. Public key re-exported with the corrected
  uid. Trust anchor (the fingerprint) is unchanged across the
  uid swap, so no re-publication impact for verifiers that have
  already pinned the fingerprint.

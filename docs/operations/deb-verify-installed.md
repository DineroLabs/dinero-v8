# Running `dinero-deb-verify --installed`

Phase E.3's PackageGate has two modes: `--static` runs offline against
a `.deb` or staging dir; `--installed` runs against a system where
`dpkg -i dinero-core_*.deb` has completed. This doc covers
`--installed` — how to run it anywhere (chroot, container, fresh VPS,
GHA runner) without locking us into a specific CI vendor.

## What `--installed` adds over `--static`

Hard-fails that `--static` cannot see:

- **Postinst effect verification** — `dinero` system user actually
  exists with non-interactive shell; `/var/lib/dinero/` actually has
  mode `0750` owned `dinero:dinero`; `dinero.service` actually
  visible to systemd; not auto-enabled.
- **`dinero-cli health` exit-code matrix** — daemon-unreachable must
  return exit 2 + `FAILING <reason>` per spec §Standard 10. Today's
  CLI returns exit 1 + `Error: Failed to connect`; the gate
  hard-fails this with a fix-pointer at `src/cli/`. (When the CLI
  conforms, this becomes a green check.)
- **(After Phase D.4 picks the command name)** Bundled-libs JSON
  schema — currently SKIPs in both modes.

## Recipe — clean Ubuntu chroot (debootstrap)

Smallest reproducible env. Works on any Linux host with `debootstrap`
+ `binutils` installed.

```bash
# One-time setup
sudo apt-get install -y debootstrap binutils
ROOT=/tmp/dinero-deb-verify-chroot
sudo debootstrap --variant=minbase \
  --include=systemd,adduser,libc6,libstdc++6,libgcc-s1,binutils,python3 \
  noble "$ROOT" http://archive.ubuntu.com/ubuntu/

# Per-build verification
sudo cp dinero-core_2.2.5-1_amd64.deb "$ROOT/tmp/"
sudo cp share/scripts/dinero-deb-verify "$ROOT/tmp/"
sudo mount --bind /proc "$ROOT/proc"
sudo mount --bind /dev "$ROOT/dev"
sudo chroot "$ROOT" /bin/bash -c \
  "dpkg -i /tmp/dinero-core_2.2.5-1_amd64.deb && /tmp/dinero-deb-verify --installed"
sudo umount "$ROOT/dev" "$ROOT/proc"
```

Notes:
- `binutils` in the `--include` list ensures `readelf` is present
  (Check 2 SKIPs cleanly without it but that's strictly less honest).
- `python3` in the `--include` list enables Check 8 bundled-libs
  JSON schema validation (the verifier uses Python's stdlib `json`
  module — no extra packages needed).
- `noble` = Ubuntu 24.04, the Core 1.0 Linux baseline. Use `jammy`
  only when intentionally cutting a separate 22.04-compatible build.
- Postinst's systemctl bits are gated on `[ -d /run/systemd/system ]`
  and silently no-op in a chroot. That's fine: the verifier's
  Check 6 reads from `/var/lib/dpkg/info/dinero-core.postrm` rather
  than relying on systemd state.

## Recipe — Docker / Podman

```bash
docker run --rm -v "$PWD":/work ubuntu:24.04 bash -c '
  apt-get update -qq && apt-get install -y --no-install-recommends \
      adduser binutils libc6 libstdc++6 libgcc-s1 python3
  dpkg -i /work/dinero-core_2.2.5-1_amd64.deb
  /work/share/scripts/dinero-deb-verify --installed
'
```

## Recipe — fresh VPS / VM

Run as `root` after a clean Ubuntu install:

```bash
apt-get install -y binutils python3
dpkg -i ./dinero-core_2.2.5-1_amd64.deb
./share/scripts/dinero-deb-verify --installed
# After validation — purge:
apt-get purge -y dinero-core
```

`apt purge` runs the postrm `purge` arm, which removes
`/var/lib/dinero/`, `/etc/dinero/`, the dinero user, and the
journald drop-in. Sandbox state is fully cleaned.

## Recipe — GitHub Actions (thin wrapper)

The script is the contract; CI is just a runner. Drop-in workflow:

```yaml
# .github/workflows/deb-verify-installed.yml (sketch)
jobs:
  verify-installed:
    strategy:
      matrix: { ubuntu: [24.04] }
    runs-on: ubuntu-${{ matrix.ubuntu }}
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get install -y binutils python3
      - run: sudo dpkg -i artifacts/dinero-core_*.deb
      - run: ./share/scripts/dinero-deb-verify --installed
```

Same script, same gates. Add this when CI spend is justified — not
required for E.3 closure.

## Pass criteria

`dinero-deb-verify --installed` exits 0 ⇒ live runtime contract is
honored. Both `--static` AND `--installed` must pass before E.3/E.6
is declared complete.

Status as of 2026-05-02: **both modes PASS with no SKIPs** when
binutils + python3 are present in the test env. All 8 categories
green: ldd, RUNPATH, Depends allowlist, install layout, file modes,
postinst+postrm sanity, service unit, runtime contract (health
exit-code matrix + bundled-libs JSON schema).

## Optional SKIPs

The script SKIPs (does not fail) gracefully when test-env tooling
is missing:

- Check 2 (RUNPATH) SKIPs without `binutils`.
- Check 8 bundled-libs JSON SKIPs without `python3`.

Both SKIPs disappear in any environment built per the recipes above.
A green run is `PASS: dinero-deb-verify — all hard contract checks
green.` (no `(with N skipped)` qualifier).

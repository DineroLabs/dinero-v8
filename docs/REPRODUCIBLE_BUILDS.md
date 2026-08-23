# Reproducible Dinero Builds

Status: Linux `dinerod` is guarded by an independent two-builder CI comparison.
macOS and Windows release artifacts are not yet claimed to be reproducible.

Reproducibility is a supply-chain property: a fixed source commit, toolchain,
dependency set, and build configuration must produce a byte-identical binary.
It is separate from consensus compatibility, canary deployment, code signing,
and notarization.

## Verified scope

The live `Reproducible Linux daemon` workflow builds the same commit twice on
separately provisioned Ubuntu 24.04 runners. The checkouts deliberately use
different absolute source paths (`source-alpha` and `source-beta`), neither
builder consumes an object cache, and a third job compares both `dinerod`
files byte-for-byte and by SHA-256.

The workflow records the commit, `SOURCE_DATE_EPOCH`, compiler, CMake, Rust,
and resulting SHA-256 in downloadable evidence. It runs when the reproducible
build policy changes, weekly, and on manual dispatch.

This proves reproducibility only inside the declared Linux builder contract.
It does not claim that different compilers, distributions, architectures, or
dependency versions produce the same bytes. Such environments can still
produce consensus-compatible binaries, but that is a different claim.

## Canonical configuration

Use the timestamp of the commit being built, not the current time:

```bash
export SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)"
export TZ=UTC
export LC_ALL=C

cmake -S . -B build-repro -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDINERO_REPRODUCIBLE_BUILD=ON \
  -DDINERO_USE_VENDORED_DEPS=ON \
  -DENABLE_GRPC=OFF
cmake --build build-repro --target dinerod -j2
sha256sum build-repro/dinerod
```

`DINERO_REPRODUCIBLE_BUILD=ON` refuses to configure without
`SOURCE_DATE_EPOCH`. Its CMake policy:

- maps source and build roots out of file names, macros, and debug metadata;
- propagates the same canonical source/build mappings into the isolated
  vendored RocksDB build;
- warns if first-party code uses time-dependent compiler macros;
- requests deterministic archive metadata;
- removes non-semantic ELF build IDs or Mach-O UUIDs; and
- enables MSVC reproducibility/path mapping when that lane is eventually
  promoted to a verified contract.

Release scripts that export `SOURCE_DATE_EPOCH` enable this policy by default.
Ordinary developer builds remain unchanged unless the option is explicitly
enabled.

## Independent verification

1. Verify the signed release tag and check out that exact commit.
2. Use the same OS image, compiler, Rust version, dependency pins, and CMake
   options recorded by the release's reproducibility evidence.
3. Build with the canonical environment above.
4. Compare the unmodified `dinerod` SHA-256 with the published verified hash.
5. If it differs, do not describe the release as reproduced. Preserve both
   binaries and manifests and inspect them with `diffoscope`.

The repository comparison helper can be used directly:

```bash
python3 scripts/ci/compare_reproducible_builds.py \
  /path/to/independent-a/dinerod \
  /path/to/independent-b/dinerod
```

## Release claims

Release notes must distinguish these statements:

- `dinerod` matched the two-builder Linux reproducibility gate;
- published checksums were signed by the release key;
- macOS artifacts were Developer ID signed and Apple notarized; and
- a canary remained consensus-compatible with the deployed network.

One statement does not imply the others. In particular, Apple signing and
notarization add identity and timestamped service metadata, so the final DMG,
ZIP, and signed app bundle are not currently promised to be byte-identical.
The unsigned embedded daemon can be compared separately.

## Failure classification

When two builds differ, classify the difference before changing code:

- absolute source/build path leakage;
- timestamps or `__DATE__`/`__TIME__` use;
- archive member metadata;
- linker build ID, UUID, or link ordering;
- compiler, SDK, libc, Rust, or dependency drift; or
- genuine code-generation nondeterminism.

Record irreducible differences honestly. A signed artifact with a published
hash still has verifiable provenance, but it is not independently reproducible
until another declared builder produces the same bytes.

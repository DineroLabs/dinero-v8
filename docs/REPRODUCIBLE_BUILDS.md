# Reproducible Builds - DineroCoin v1.0

**Status:** Phase Z.1 - Build Reproducibility Foundation
**Date:** 2025-12-31
**Objective:** Bit-for-bit identical binaries from the same git tag

---

## Executive Summary

**"Trust, but verify."**

Reproducible builds allow independent operators to verify that published binaries match the source code, eliminating supply-chain attacks and build tampering.

Two operators, on different machines, building from the same git tag, **must** produce identical SHA256 hashes.

---

## Guarantees (Phase Z.1)

Phase Z.1 provides:
- ✅ **Deterministic binaries** - Same input → same output
- ✅ **Auditable artifacts** - Published hashes for verification
- ✅ **Supply-chain transparency** - No hidden modifications
- ✅ **Operator trust** - Verify before running

Phase Z.1 does NOT provide:
- ❌ No code refactors
- ❌ No performance tuning
- ❌ No dependency upgrades (unless strictly required)
- ❌ No consensus changes
- ❌ No runtime behavior changes

---

## Supported Build Matrix (v1.0)

### Conservative Platform Support

We support reproducible builds on platforms commonly used by node operators:

| Platform            | Architecture | Toolchain     | Status |
|---------------------|--------------|---------------|--------|
| Ubuntu 22.04 LTS    | x86_64       | GCC 11        | ✅ Primary |
| Ubuntu 22.04 LTS    | x86_64       | Clang 15      | ✅ Tested |
| macOS 13 (Ventura)  | arm64        | AppleClang 14 | ✅ Tested |
| macOS 13 (Ventura)  | x86_64       | AppleClang 14 | ✅ Tested |

### Deferred Platforms

- **Windows (MSVC)**: Deferred to v1.1 (MSVC reproducibility requires additional tooling)
- **FreeBSD**: Community-supported (not officially tested)
- **Alpine Linux**: Community-supported (musl libc differences)

**Rationale:** Start conservative. Focus on platforms where reproducibility is well-understood.

---

## Build Environment Requirements

### Compiler Versions (Strict)

**GCC:**
- Version: 11.4.0
- Source: `apt install gcc-11 g++-11` (Ubuntu 22.04)
- Verification: `gcc-11 --version`

**Clang:**
- Version: 15.0.7
- Source: `apt install clang-15` (Ubuntu 22.04)
- Verification: `clang-15 --version`

**AppleClang (macOS):**
- Version: 14.0.x (Xcode 14.x)
- Source: Xcode Command Line Tools
- Verification: `clang --version`

**CMake:**
- Version: 3.26.x or higher
- Source: `apt install cmake` or Homebrew
- Verification: `cmake --version`

**Version Mismatches:**
- Minor version differences may produce different binaries
- Always use the exact versions listed above for release builds
- CI enforces version checks (soft warning, not hard failure)

### Dependency Pinning

See [DEPENDENCIES.md](DEPENDENCIES.md) for complete dependency list with:
- Exact versions
- Source URLs
- SHA256 hashes
- Git commit hashes (for git dependencies)

**Critical Rule:** No `latest` versions. All dependencies pinned.

---

## Deterministic Build Flags

### Environment Variables (Mandatory)

```bash
# Eliminate timestamp drift
export SOURCE_DATE_EPOCH=1700000000  # 2023-11-15 00:00:00 UTC

# Eliminate locale drift
export TZ=UTC
export LC_ALL=C

# Eliminate path leaks
export HOME=/reproducible
export USER=builder
```

### Compiler Flags

**GCC/Clang:**
```cmake
-fno-record-gcc-switches  # Don't embed compiler command line
-Wdate-time               # Warn on __DATE__, __TIME__ usage
-fdebug-prefix-map=$PWD=. # Normalize debug paths
```

**CMake Configuration:**
```cmake
set(CMAKE_SKIP_RPATH TRUE)
set(CMAKE_BUILD_RPATH "")
set(CMAKE_INSTALL_RPATH "")
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
```

**Rationale:**
- Eliminates build timestamps
- Eliminates absolute paths in binaries
- Eliminates locale-dependent behavior
- Enables bit-for-bit reproducibility

---

## Canonical Build Script

**Location:** `contrib/build-deterministic.sh`

**Usage:**
```bash
git checkout v1.0.0-rc1
./contrib/build-deterministic.sh
```

**Responsibilities:**
1. Verify compiler versions
2. Verify dependency hashes
3. Set deterministic environment
4. Build in clean directory
5. Produce binaries: `dinerod`, `dinero-cli`
6. Output SHA256 hashes

**Critical:** This is the **only** supported build path for release verification.

---

## Verification Procedure

### Step 1: Checkout Release Tag

```bash
git clone https://github.com/Trucker2827/Dinero-Coin.git
cd Dinero-Coin
git checkout v1.0.0-rc1
git verify-tag v1.0.0-rc1  # Verify GPG signature (if available)
```

### Step 2: Build Deterministically

```bash
./contrib/build-deterministic.sh
```

**Expected Output:**
```
[Build Log...]
✅ Build complete
SHA256(dinerod)     = a1b2c3d4e5f6...
SHA256(dinero-cli)  = f6e5d4c3b2a1...
```

### Step 3: Compare Hashes

```bash
cat docs/RELEASE_HASHES_v1.0.0-rc1.txt
```

**Expected Result:**
```
a1b2c3d4e5f6... dinerod
f6e5d4c3b2a1... dinero-cli
```

**Verification:**
```bash
sha256sum -c docs/RELEASE_HASHES_v1.0.0-rc1.txt
```

✅ **Success:** `dinerod: OK`
✅ **Success:** `dinero-cli: OK`

❌ **Failure:** Hashes do not match → **DO NOT RUN BINARY**

---

## What Breaks Reproducibility (Failure Modes)

### Known Failure Modes

1. **Compiler Version Mismatch**
   - GCC 11.3 vs 11.4 may produce different codegen
   - Solution: Use exact versions from build matrix

2. **Different libc**
   - glibc 2.35 vs 2.36 may embed different symbols
   - Solution: Build on Ubuntu 22.04 LTS

3. **Different CPU Target Flags**
   - `-march=native` embeds host CPU features
   - Solution: Use generic target (`-march=x86-64`)

4. **Filesystem Ordering**
   - Different filesystems may iterate files differently
   - Solution: Sort inputs (CMake does this by default)

5. **Non-UTC Locale**
   - Date formatting may differ
   - Solution: `export TZ=UTC LC_ALL=C`

6. **Debug vs Release Builds**
   - `-DCMAKE_BUILD_TYPE=Debug` embeds debug symbols
   - Solution: Always use `Release` for verification

7. **LTO (Link-Time Optimization) Differences**
   - Different LTO implementations may reorder code
   - Solution: Disable LTO for release builds (for now)

### Debugging Reproducibility Failures

If hashes don't match:

1. **Compare build environments:**
   ```bash
   gcc --version
   clang --version
   cmake --version
   ldd --version
   ```

2. **Check environment variables:**
   ```bash
   echo $SOURCE_DATE_EPOCH
   echo $TZ
   echo $LC_ALL
   ```

3. **Binary diff (advanced):**
   ```bash
   diffoscope dinerod-build1 dinerod-build2
   ```

4. **Report to developers:**
   - Open GitHub issue
   - Attach build log
   - Attach environment details

---

## CI Validation (Optional)

**If CI is available:**
- Run deterministic build twice
- Compare SHA256 hashes
- Fail CI if mismatch

**If not available:**
- Manual verification by release manager
- Documented in release notes
- Community verification encouraged

**Status:** Manual verification for v1.0 (CI automation deferred to v1.1)

---

## Reproducible Build Freeze

**Contract:**

Any change that breaks reproducibility requires:
1. Explicit justification (security fix, critical bug)
2. Updated reference hashes
3. New release tag (e.g., v1.0.1)
4. Public disclosure in release notes

**Rationale:**

Breaking reproducibility without disclosure is a **supply-chain vulnerability**.

---

## Release Process Integration

### Pre-Release Checklist

Before tagging a release:
- [ ] All dependencies pinned in `DEPENDENCIES.md`
- [ ] Deterministic build script tested
- [ ] Reference hashes generated
- [ ] At least 2 independent operators verify hashes
- [ ] GPG-signed tag created
- [ ] Release notes include hash verification instructions

### Post-Release Verification

After publishing binaries:
- Community members verify hashes
- Results published (GitHub Discussions, Reddit, Discord)
- Mismatches trigger immediate investigation

---

## Audit Trail

Phase Z.1 establishes **reproducible build foundation**:

1. **Phase D** - Consensus frozen ✅
2. **Phase E.1** - Crash safety ✅
3. **Phase E.2** - Resource exhaustion protection ✅
4. **Phase E.3** - CPU timeout enforcement ✅
5. **Phase E.3.1** - Operational observability ✅
6. **Phase H** - Headers-first sync ✅
7. **Phase Z.1** - Reproducible builds ← **YOU ARE HERE** 🔨

**Next:** Phase Z.2 (Configuration Guarantees)

---

## References

- [Reproducible Builds Project](https://reproducible-builds.org/)
- [Bitcoin Core Deterministic Builds](https://github.com/bitcoin/bitcoin/blob/master/doc/gitian-building.md)
- [Debian Reproducible Builds](https://wiki.debian.org/ReproducibleBuilds)
- [SOURCE_DATE_EPOCH Specification](https://reproducible-builds.org/specs/source-date-epoch/)

---

**Phase Z.1: Reproducible Builds - Foundation Complete**

**Trust guarantee:**
- ✅ Deterministic binaries
- ✅ Auditable artifacts
- ✅ Supply-chain transparency
- ✅ Operator verification

**Node operators can verify before trusting.**

# DineroCoin Hermetic Toolchain System

## Purpose

**Hermetic toolchains define build reality.**

They tell CMake:
- Which compiler is authoritative
- Which SDK/sysroot is authoritative
- Which headers and libraries exist
- **Which ones don't**

Without toolchain files, your build depends on:
- ✗ What's installed on the machine
- ✗ What's in PATH
- ✗ What's in the registry (Windows)
- ✗ What package managers did
- ✗ Luck

With toolchain files, your build depends on:
- ✓ Explicit compiler paths
- ✓ Explicit SDK versions
- ✓ Vendored dependencies only
- ✓ Reproducible rules

**This is not "adding complexity". This is removing ambiguity.**

---

## Philosophy

### The Problem

Developer machines are **contaminated** by default:

| Platform | Ambient Pollution |
|----------|-------------------|
| **macOS** | Homebrew (`/opt/homebrew`), MacPorts, Fink, multiple Xcode SDKs |
| **Linux** | `/usr/local`, distro drift (Ubuntu 20.04 vs 22.04 vs 24.04) |
| **Windows** | Multiple MSVC versions, vcpkg auto-integration, registry discovery, PATH chaos |

CMake's defaults assume:
- System = trustworthy ❌
- Environment = stable ❌
- User-installed software = benign ❌

**That assumption is false on developer machines.**

### The Solution

Hermetic toolchains **define reality**:

```
If it builds with this toolchain, it builds anywhere.
If it doesn't build with this toolchain, it shouldn't exist.
```

This is the same approach used by:
- **Bitcoin Core** (consensus code must be deterministic)
- **LLVM** (CI infrastructure across distros)
- **Chromium** (multi-platform reproducible builds)

---

## Toolchain Files

```
toolchains/
├── common-hermetic.cmake        # Universal rules (all platforms)
├── macos-hermetic.cmake         # macOS: Homebrew quarantine + SDK lock
├── linux-hermetic.cmake         # Linux: glibc pinning + /usr/local quarantine
├── windows-msvc-hermetic.cmake  # Windows: MSVC + SDK lock + vcpkg kill
└── README.md                    # This file
```

### `common-hermetic.cmake`

**Universal build contract** (included by platform toolchains):

- Block CMake package discovery (registries, PATH)
- Prioritize vendored dependencies
- Enforce reproducibility settings
- Security hardening (stack protection, fortify source)

### `macos-hermetic.cmake`

**macOS-specific hermetics**:

- ✓ Compiler: `/usr/bin/clang` (not Homebrew clang)
- ✓ SDK: `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk` (locked)
- ✓ Deployment target: macOS 12.0
- ✓ Homebrew: **QUARANTINED** (removed from all search paths)
- ✓ MacPorts/Fink: **QUARANTINED**
- ✓ RocksDB archive fix: Applied

**Usage:**
```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/macos-hermetic.cmake \
  -DDINERO_USE_VENDORED_DEPS=ON
```

### `linux-hermetic.cmake`

**Linux-specific hermetics**:

- ✓ Compiler: GCC 11+ (pinned, distro-independent)
- ✓ `/usr/local`: **QUARANTINED**
- ✓ System paths: `/usr` only (FHS standard)
- ✓ glibc: Detected and logged
- ✓ Security hardening: RELRO, PIE, no-exec-stack

**Usage:**
```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/linux-hermetic.cmake \
  -DDINERO_USE_VENDORED_DEPS=ON
```

### `windows-msvc-hermetic.cmake`

**Windows-specific hermetics** (most critical):

- ✓ Compiler: MSVC (cl.exe) only - no MinGW, no clang-cl
- ✓ Windows SDK: 10.0.22621.0 (locked)
- ✓ CRT linkage: **Static** (`/MT`) - critical for vendored deps
- ✓ vcpkg: **KILLED** (auto-integration disabled)
- ✓ Registry: **BLOCKED** (no package discovery)
- ✓ PATH: **IGNORED** (no environment pollution)
- ✓ Generator: Visual Studio only (not Ninja with MSVC)

**Usage (PowerShell):**
```powershell
cmake -S . -B build `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-msvc-hermetic.cmake `
  -DDINERO_USE_VENDORED_DEPS=ON
```

---

## When to Use Toolchain Files

### **ALWAYS use for:**
- ✓ CI builds (GitHub Actions, Jenkins, etc.)
- ✓ Release builds (official binaries)
- ✓ Deterministic consensus code builds
- ✓ Multi-machine reproducibility

### **OPTIONAL for:**
- ? Local development (developer freedom)
- ? Quick iteration (if environment is clean)

### **NEVER use for:**
- ✗ Cross-compilation (these are host-native toolchains)

---

## How This Integrates with Existing DineroCoin Build

### Your Existing Hermetic Pattern

You already implemented **three-layer hermetic GoogleTest**:

1. **Layer 1:** SYSTEM → INTERFACE upgrade (`cmake/VendorGTest.cmake:40-73`)
2. **Layer 2:** Homebrew hard quarantine (`cmake/VendorGTest.cmake:9-18`)
3. **Layer 3:** Global BEFORE precedence (`cmake/VendorGTest.cmake:76-88`)

This solved the **target-level include priority** problem.

### Toolchain Files Add Defense-in-Depth

Toolchain files run **before project()**, so they:
- Set compiler **before CMake detects anything**
- Quarantine paths **before find_package() runs**
- Lock SDK **before headers are discovered**

```
┌─────────────────────────────────────────────┐
│ Toolchain File (global, pre-project)        │
│ ↓ Compiler lock                             │
│ ↓ SDK lock                                  │
│ ↓ Path quarantine                           │
└─────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────┐
│ CMakeLists.txt (project configuration)      │
│ ↓ RocksDB vendoring                         │
│ ↓ GoogleTest vendoring                      │
│ ↓ Boost discovery                           │
└─────────────────────────────────────────────┘
         ↓
┌─────────────────────────────────────────────┐
│ VendorGTest.cmake (target-level fixes)      │
│ ↓ SYSTEM → INTERFACE upgrade                │
│ ↓ Homebrew quarantine (Layer 2)             │
│ ↓ Global BEFORE precedence (Layer 3)        │
└─────────────────────────────────────────────┘
```

**Both are necessary:**
- **Toolchain file:** Prevents bad paths from entering CMake's search space
- **VendorGTest.cmake:** Fixes include priority for targets that need it

---

## Platform Comparison

| Threat | macOS | Linux | Windows |
|--------|-------|-------|---------|
| **Package manager pollution** | Homebrew (ambient) | `/usr/local` (manual) | vcpkg (auto-integrates) |
| **Compiler ambiguity** | `/usr/bin/clang` vs `/opt/homebrew/bin/clang` | gcc-9 vs gcc-11 vs gcc-13 | MSVC 2019 vs 2022, clang-cl, MinGW |
| **SDK/sysroot drift** | Multiple Xcode SDKs | glibc 2.31 vs 2.35 | Windows SDK 19041 vs 22621 |
| **Registry discovery** | N/A | N/A | ✓ (silent, invisible) |
| **Hermetic necessity** | Strong | Medium | **CRITICAL** |

**Windows is where hermetic toolchains matter MOST.**

---

## Verification

### Test Hermetic Build (macOS)

```bash
# Clean build
rm -rf build_hermetic

# Configure with toolchain
cmake -S . -B build_hermetic \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/macos-hermetic.cmake \
  -DDINERO_USE_VENDORED_DEPS=ON \
  -DENABLE_TESTS=ON

# Check output for:
# ✓ "🔒 Hermetic Toolchain: ACTIVE"
# ✓ "SDK locked to /Library/Developer/..."
# ✓ "Homebrew QUARANTINED"

# Build test
cmake --build build_hermetic --target test_consensus_ring2_validity

# Verify includes (should NOT contain /opt/homebrew)
cmake --build build_hermetic --target test_consensus_ring2_validity --verbose 2>&1 | grep -i "homebrew"
# Expected: No output (Homebrew not in include paths)
```

### Test Hermetic Build (Windows)

```powershell
# Clean build
Remove-Item -Recurse -Force build_hermetic -ErrorAction SilentlyContinue

# Configure with toolchain
cmake -S . -B build_hermetic `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-msvc-hermetic.cmake `
  -DDINERO_USE_VENDORED_DEPS=ON `
  -DENABLE_TESTS=ON

# Check output for:
# ✓ "🔒 Hermetic Toolchain: ACTIVE"
# ✓ "Windows SDK: 10.0.22621.0"
# ✓ "vcpkg: KILLED"

# Build
cmake --build build_hermetic --config Release
```

### Test Hermetic Build (Linux)

```bash
# Clean build
rm -rf build_hermetic

# Configure with toolchain
cmake -S . -B build_hermetic \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/linux-hermetic.cmake \
  -DDINERO_USE_VENDORED_DEPS=ON \
  -DENABLE_TESTS=ON

# Check output for:
# ✓ "🔒 Hermetic Toolchain: ACTIVE"
# ✓ "Compiler: /usr/bin/g++-11"
# ✓ "/usr/local: QUARANTINED"

# Build test
cmake --build build_hermetic --target test_consensus_ring2_validity -j$(nproc)
```

---

## Build Without Toolchain Files (Developer Freedom)

**Toolchain files are OPTIONAL for local development:**

```bash
# Standard build (no toolchain file)
cmake -S . -B build -DDINERO_USE_VENDORED_DEPS=ON
cmake --build build
```

This works because:
- `DINERO_USE_VENDORED_DEPS=ON` uses vendored deps
- `cmake/VendorGTest.cmake` already has Layers 1-3 hermetic pattern
- `cmake/VendorRocksDBSimple.cmake` uses vendored RocksDB

**Developer choice preserved. Hermetic builds available when needed.**

---

## CI Integration

### GitHub Actions Example

```yaml
name: Hermetic Build

on: [push, pull_request]

jobs:
  build-macos:
    runs-on: macos-13
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Configure (Hermetic)
        run: |
          cmake -S . -B build \
            -DCMAKE_TOOLCHAIN_FILE=toolchains/macos-hermetic.cmake \
            -DDINERO_USE_VENDORED_DEPS=ON \
            -DENABLE_TESTS=ON

      - name: Build
        run: cmake --build build -j$(sysctl -n hw.ncpu)

      - name: Test
        run: cd build && ctest --output-on-failure

  build-windows:
    runs-on: windows-2022
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Configure (Hermetic)
        run: |
          cmake -S . -B build `
            -G "Visual Studio 17 2022" `
            -A x64 `
            -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-msvc-hermetic.cmake `
            -DDINERO_USE_VENDORED_DEPS=ON `
            -DENABLE_TESTS=ON

      - name: Build
        run: cmake --build build --config Release

      - name: Test
        run: cd build && ctest -C Release --output-on-failure

  build-linux:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Install GCC-11
        run: sudo apt update && sudo apt install -y gcc-11 g++-11

      - name: Configure (Hermetic)
        run: |
          cmake -S . -B build \
            -DCMAKE_TOOLCHAIN_FILE=toolchains/linux-hermetic.cmake \
            -DDINERO_USE_VENDORED_DEPS=ON \
            -DENABLE_TESTS=ON

      - name: Build
        run: cmake --build build -j$(nproc)

      - name: Test
        run: cd build && ctest --output-on-failure
```

---

## Future Enhancements

### Possible Next Steps

1. **Remove Layer 3 hacks** (`cmake/VendorGTest.cmake:85-88`)
   - Toolchain files may eliminate need for `include_directories(BEFORE ...)`
   - Requires testing to confirm

2. **Reproducible timestamps** (for Bitcoin Core-style deterministic builds)
   - Set `SOURCE_DATE_EPOCH` environment variable
   - Strips non-deterministic metadata from binaries

3. **Cross-compilation toolchains** (separate from hermetic host builds)
   - `toolchains/macos-arm64-cross.cmake` (x86_64 → arm64)
   - `toolchains/linux-musl-static.cmake` (glibc → musl for static bins)

4. **CI enforcement** (make toolchains mandatory in CI)
   - Add check in CMakeLists.txt: `if(CI AND NOT CMAKE_TOOLCHAIN_FILE) message(FATAL_ERROR ...)`

---

## Key Insights

### 1. Toolchain Files Are NOT About Cross-Compiling

They are about **truth containment**:

> "This is what exists. Nothing else does. If it builds here, it builds anywhere."

### 2. macOS Exposed the Problem First

macOS is where hermetic builds become **unavoidable** because:
- Homebrew is ambient
- SDK paths are implicit
- Framework resolution is complex

But the solution is **universal**.

### 3. Windows Is Where It Matters Most

Windows has:
- More ambient pollution than macOS
- More failure modes (vcpkg, registry, multiple SDKs)
- Higher stakes (production releases depend on CRT linkage)

**If macOS is leaky, Windows is radioactive.**

### 4. This Is How Institutions Build

Bitcoin Core, LLVM, Chromium all converged on this pattern:
- **Explicit is better than implicit**
- **Locked is better than flexible**
- **Reproducible is better than convenient**

You're building something serious. This is the right architecture.

---

## Questions?

If you're unsure whether to use a toolchain file, ask:

1. **Does this build need to be reproducible?** → Use toolchain file
2. **Will others need to build the same binary?** → Use toolchain file
3. **Is this consensus/critical code?** → Use toolchain file
4. **Am I just iterating locally?** → Toolchain file optional

**When in doubt, use the toolchain file. It catches issues early.**

---

## References

- [CMake Toolchains Documentation](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)
- [Bitcoin Core Build System](https://github.com/bitcoin/bitcoin/tree/master/cmake)
- [LLVM Hermetic Toolchain Approach](https://llvm.org/docs/BuildingADistribution.html)
- [Reproducible Builds Project](https://reproducible-builds.org/)

---

**This toolchain system transforms DineroCoin from "builds on my machine" to "builds deterministically everywhere."**

**Hermetic builds are not complexity. They are clarity.**

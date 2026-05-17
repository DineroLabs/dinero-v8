# Linux-Clean Build Gate

**Status**: 🟡 **Defined** | **Enforcement**: Pending Path 1 completion

---

## Purpose

This document defines what constitutes an **authoritative build** for DineroCoin.

These are not aspirational goals - they are **invariants** that must hold before:
- Multi-node testing
- Mainnet deployment
- Release candidates
- Audit submissions

---

## Platform Authority

### Primary Build Platform: Linux

**Distribution**: Ubuntu 22.04 LTS (Jammy Jellyfish) or later
**Architecture**: x86_64

**Rationale**:
- Stricter compiler and header checking than macOS
- Reproducible builds for auditors
- Deterministic behavior across environments
- Industry standard for consensus software (Bitcoin Core, Ethereum, Monero)

### Compiler Requirements

**Primary**: GCC 11 or later
**Secondary**: Clang 14 or later

Both must pass without warnings or errors.

**Standard**: `-std=c++20`

---

## Mandatory Compiler Flags

### Error Prevention

```bash
-Wall           # Enable all common warnings
-Wextra         # Enable extra warnings
-Wpedantic      # Strict ISO C++ compliance
-Werror         # Treat warnings as errors
```

### Undefined Behavior Detection

```bash
-fsanitize=undefined    # Catch UB at runtime (debug builds)
-fsanitize=address      # Catch memory errors (debug builds)
```

### Optimization (Release)

```bash
-O3                     # Maximum optimization
-DNDEBUG               # Disable assertions
-march=x86-64-v2       # Portable x86_64 (no AVX-512)
```

---

## Include Policy

### Rule: Every file includes exactly what it uses

**Forbidden**:
- Relying on transitive includes (header A includes B which includes C, and you use C without including it)
- Platform-specific headers without `#ifdef` guards
- Using `<jsoncpp/json/json.h>` system path (must use vendored `<json/json.h>`)

**Required**:
Each `.cpp` and `.h` file must explicitly include all standard headers it references:

| If you use... | You must include... |
|---------------|---------------------|
| `std::string` | `<string>` |
| `std::vector`, `std::array` | `<vector>`, `<array>` |
| `std::unique_ptr`, `std::shared_ptr` | `<memory>` |
| `std::optional` | `<optional>` |
| `std::function` | `<functional>` |
| `uint32_t`, `uint64_t`, `int32_t` | `<cstdint>` |
| `std::sort`, `std::find`, `std::reverse` | `<algorithm>` |
| `std::runtime_error`, `std::logic_error` | `<stdexcept>` |
| `memcpy`, `memcmp`, `strlen` | `<cstring>` |
| `std::exp`, `std::log`, `std::pow` | `<cmath>` |
| `std::filesystem::path` | `<filesystem>` |

**Verification**: Use [include-what-you-use](https://include-what-you-use.org/) periodically.

---

## Build Success Criteria

### 1. Clean Build from Scratch

```bash
rm -rf build/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j1
```

**Must succeed with**:
- ✅ Zero warnings
- ✅ Zero errors
- ✅ All targets built

### 2. No Platform-Specific Hacks

**Forbidden patterns**:
```cpp
#ifdef __APPLE__
// Do something different on Mac
#endif
```

**Exception**: System headers that differ legitimately (e.g., `<endian.h>` vs `<machine/endian.h>`)

### 3. Reproducible Builds

Two identical checkouts on different Ubuntu 22.04 machines must produce:
- Identical binary hashes (excluding timestamps/build paths)
- Identical behavior when run

**Tool**: Use [reproducible-builds.org](https://reproducible-builds.org/) techniques

---

## CI Integration

### Linux CI is Authoritative

**Required checks**:
1. ✅ Build succeeds on Ubuntu 22.04 with `-Werror`
2. ✅ All unit tests pass
3. ✅ No sanitizer violations in debug mode
4. ✅ Integration tests pass (when available)

**macOS/Windows CI**:
- Must also pass
- But failures override Linux success

If Linux fails, **the build is broken** - regardless of macOS status.

---

## Enforcement

### Pre-Commit

Developers should run locally:
```bash
cmake --build build -- -j1 2>&1 | tee build.log
grep -i "warning:" build.log && exit 1
grep -i "error:" build.log && exit 1
```

### Pre-Merge

Pull requests must not be merged if:
- Linux CI shows warnings
- Build requires platform-specific hacks
- Tests fail on Linux

### Pre-Release

Before any release tag:
1. ✅ Fresh Linux build succeeds
2. ✅ Multi-node regtest passes (once Path 1 complete)
3. ✅ No outstanding sanitizer violations
4. ✅ Documentation is current

---

## Non-Negotiable Warnings

These warnings must be **zero** before mainnet:

| Warning | Reason |
|---------|--------|
| `-Wunused-variable` | Dead code, possible logic error |
| `-Wuninitialized` | Undefined behavior |
| `-Wsign-compare` | Subtle bugs in loop bounds |
| `-Wformat` | printf vulnerabilities |
| `-Wparentheses` | Operator precedence mistakes |
| `-Wreturn-type` | Missing return statements |
| `-Wunused-result` | Ignored critical return values |

**Tool**: `-Werror` makes all warnings fatal.

---

## When to Update This Gate

This document should be revised if:
1. We adopt a newer C++ standard (e.g., C++23)
2. Linux distribution baseline changes (e.g., Ubuntu 24.04 LTS)
3. New sanitizers become available
4. Consensus-critical validation tools emerge

**Process**: Update → commit → announce → enforce.

---

## Rationale

### Why Linux is Authoritative

Not ideology - **pragmatism**:
- Bitcoin Core uses Linux as truth source
- Ethereum builds are validated on Linux
- Monero CI gates on Linux
- Auditors expect reproducible Linux builds

### Why No Shortcuts

Six months from now, you will not remember:
- Which include was "probably fine"
- Why that warning "didn't matter"
- Why the macOS build "was close enough"

But you **will** encounter:
- Crashes from UB you ignored
- Divergent behavior between nodes
- Audit failures from non-deterministic builds

The pain of strict enforcement is **one-time**.
The pain of loose standards is **permanent**.

---

## Current Status

- 🟡 **Gate Defined**: This document establishes the rules
- 🔴 **Not Yet Enforced**: ~18 include violations outstanding (see PORTABILITY_CHECKPOINT.md)
- 🎯 **Target**: Path 1 (systematic portability hardening) will bring codebase to compliance

Once Path 1 completes:
- This gate becomes **enforced**
- All future commits must pass
- CI will reject violations automatically

---

_Last Updated: 2026-01-05_
_Next: Begin Path 1 (portability hardening) using this gate as reference_

# Building DineroCoin

## Prerequisites

### Required Tools
- **CMake** 3.20 or later
- **C++20 compatible compiler** (Clang 14+, GCC 11+, or MSVC 2019+)
- **Git** with submodule support

### Platform-Specific
- **macOS**: Xcode Command Line Tools
- **Linux**: build-essential package
- **Windows**: Visual Studio 2019+ with C++ tools

---

## Quick Start

### 1. Clone with Submodules

**CRITICAL**: Always initialize submodules after cloning:

```bash
git clone https://github.com/your-org/dinerocoin.git
cd dinerocoin
git submodule update --init --recursive
```

**Why is this required?**
DineroCoin uses git submodules for critical dependencies (jsoncpp, msgpack-c, rocksdb, etc.). Without this step, the build will fail with missing dependency errors.

### 2. Verify Dependencies

Run the submodule check script to ensure everything is initialized:

```bash
./.ci/check-submodules.sh
```

Expected output:
```
🔍 Checking critical git submodules...
✅ OK: third_party/rocksdb
✅ OK: third_party/snappy
✅ OK: third_party/jsoncpp
✅ OK: third_party/msgpack-c

✅ All critical submodules present
```

If any submodules are missing, re-run:
```bash
git submodule update --init --recursive
```

### 3. Build

```bash
mkdir build && cd build
cmake .. -DUSE_SYSTEM_OPENSSL=ON
make -j$(nproc)  # Linux
make -j$(sysctl -n hw.ncpu)  # macOS
```

### 4. Run Tests

```bash
ctest --output-on-failure
```

---

## Common Issues

### "CMake Error: add_subdirectory given source 'third_party/jsoncpp' which is not an existing directory"

**Cause**: Git submodules not initialized
**Fix**: Run `git submodule update --init --recursive`

### "ld: library 'atomic' not found"

**Cause**: RocksDB trying to link libatomic on macOS (not needed)
**Fix**: Already patched in submodule - ensure you're using the repo's version

### "symbol(s) not found for architecture arm64"

**Cause**: Missing library in CMake linkage
**Fix**: Check CMakeLists.txt for correct target_link_libraries() configuration

---

## CI Integration

Add this to your CI workflow **before** the build step:

```yaml
- name: Initialize submodules
  run: git submodule update --init --recursive

- name: Verify submodules
  run: ./.ci/check-submodules.sh
```

This prevents build failures from missing dependencies in automated environments.

---

## Development Workflow

When adding new submodules:

1. **Add the submodule**:
   ```bash
   git submodule add <url> third_party/<name>
   ```

2. **Pin to a specific version** (recommended):
   ```bash
   cd third_party/<name>
   git checkout <tag-or-commit>
   cd ../..
   git add third_party/<name>
   ```

3. **Update .ci/check-submodules.sh** if it's a critical dependency:
   ```bash
   # Add to CRITICAL_SUBMODULES array
   "third_party/<name>"
   ```

4. **Commit both .gitmodules and the submodule pointer**:
   ```bash
   git commit -m "feat: add <name> submodule"
   ```

---

## Additional Resources

- **CMake Configuration**: See `CMakeLists.txt` for available options
- **Submodule Management**: `.gitmodules` lists all registered submodules
- **Dependency Audit**: Run `.ci/check-submodules.sh` to verify state

For issues not covered here, check the issue tracker or ask in Discord.

# 📦 Vendored Dependencies in DineroCoin

## Overview

DineroCoin uses **vendored** (self-contained) dependencies to ensure:
- ✅ **No system dependencies** - Build anywhere without Homebrew/apt
- ✅ **Reproducible builds** - Same library versions every time
- ✅ **Portable binaries** - Static linking means no missing .dylib/.so files
- ✅ **Cross-platform** - Works on macOS, Linux, Windows

## Current Vendored Libraries

### secp256k1 (Elliptic Curve Cryptography)
- **Version**: Latest from submodule
- **Location**: `vendor/lib/libsecp256k1.a`
- **Size**: ~1.5MB static library
- **Modules Enabled**:
  - Recovery (signature recovery)
  - ECDH (key agreement)
  - Schnorr signatures
  - Extra keys

## 🚀 Quick Setup

### First Time Setup
```bash
# One command does everything
./setup-vendored-secp256k1.sh
```

This will:
1. Build secp256k1 from the submodule
2. Install to `vendor/lib/` and `vendor/include/`
3. Update CMakeLists.txt automatically
4. Create a backup of your original CMakeLists.txt

### Manual Setup (Step by Step)
```bash
# Step 1: Build secp256k1
./build-vendored-secp256k1.sh

# Step 2: Update CMakeLists.txt
./update-cmake-for-vendored-secp256k1.sh
```

## 📂 Directory Structure

After setup:
```
DineroCoin/
├── vendor/                           # Vendored dependencies
│   ├── lib/
│   │   └── libsecp256k1.a           # Static library (~1.5MB)
│   └── include/
│       ├── secp256k1.h
│       ├── secp256k1_ecdh.h
│       ├── secp256k1_extrakeys.h
│       ├── secp256k1_preallocated.h
│       ├── secp256k1_recovery.h
│       └── secp256k1_schnorrsig.h
├── secp256k1/                        # Submodule source
│   └── build-vendored/               # Build directory
└── CMakeLists.txt                    # Updated to use vendor/
```

## 🔍 What Changed in CMakeLists.txt?

### Before (Homebrew Dependency)
```cmake
target_link_libraries(dinerod PRIVATE
    /opt/homebrew/lib/libsecp256k1.dylib  # ❌ Homebrew dependency
)
```

### After (Vendored)
```cmake
include_directories(${CMAKE_SOURCE_DIR}/vendor/include)  # ✅ Vendored

target_link_libraries(dinerod PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/lib/libsecp256k1.a  # ✅ Static, self-contained
)
```

## ✅ Benefits

### Before Vendoring
- ❌ Required: `brew install secp256k1`
- ❌ Different versions on different machines
- ❌ Dynamic linking (.dylib/.so) - must be present at runtime
- ❌ Harder to deploy

### After Vendoring
- ✅ No external dependencies needed
- ✅ Same version everywhere
- ✅ Static linking - embedded in binary
- ✅ Works anywhere - just copy the binary

## 🧪 Testing

### Build and Test
```bash
# Clean build to test vendored dependencies
rm -rf build-test
mkdir build-test && cd build-test

# Configure
cmake ..

# Build
make -j$(sysctl -n hw.ncpu)

# Run tests
ctest -V

# Check that binary doesn't depend on Homebrew
otool -L bin/dinerod | grep -i homebrew
# Should return nothing!
```

### Verify Static Linking
```bash
# On macOS
otool -L bin/dinerod

# On Linux
ldd bin/dinerod

# secp256k1 should NOT appear in the list
# (it's statically linked into the binary)
```

## 🔄 Updating secp256k1

To update to a newer version of secp256k1:

```bash
# Update submodule
cd secp256k1
git pull origin master
cd ..

# Rebuild vendored library
./build-vendored-secp256k1.sh

# CMakeLists.txt already points to vendor/, so no changes needed
```

## 🐛 Troubleshooting

### "libsecp256k1.a not found"
```bash
# Rebuild vendored library
./build-vendored-secp256k1.sh
```

### "undefined symbols" during linking
```bash
# Make sure you're linking the static library
grep "libsecp256k1.a" CMakeLists.txt

# Should see:
# ${CMAKE_SOURCE_DIR}/vendor/lib/libsecp256k1.a
```

### Still seeing Homebrew references
```bash
# Check if update script ran correctly
./update-cmake-for-vendored-secp256k1.sh

# If still issues, manually check:
grep -n "homebrew" CMakeLists.txt
# Should return nothing
```

## 📦 Distribution

When distributing DineroCoin:

### Source Distribution
Include the `vendor/` directory:
```bash
tar czf dinerocoin-source.tar.gz \
  --exclude='.git' \
  --exclude='build*' \
  .
```

### Binary Distribution
Binaries built with vendored libraries are **fully self-contained**:
```bash
# Just copy the binary - no dependencies needed!
cp build/bin/dinerod /usr/local/bin/

# Verify it works
dinerod --version
```

## 🔐 Security

### Why Static Linking is Safer
- ✅ **Known version** - Exact secp256k1 version embedded
- ✅ **No supply chain attacks** - Not affected by system library updates
- ✅ **Reproducible** - Same binary everywhere
- ✅ **No LD_LIBRARY_PATH tricks** - Can't be hijacked

### Verifying Library Integrity
```bash
# Check SHA256 of vendored library
shasum -a 256 vendor/lib/libsecp256k1.a

# Check which commit it was built from
cd secp256k1
git log -1
```

## 🌍 Cross-Platform Builds

### macOS (arm64 + x86_64)
```bash
cmake .. -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
make -j
```

### Linux
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
```

### Windows (Cross-compile from Linux)
```bash
cmake .. -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
make -j
```

## 📚 Additional Vendored Libraries (Future)

Consider vendoring these in the future:
- **OpenSSL** - For SSL/TLS and additional crypto
- **RocksDB** - Already vendored via submodule
- **libevent** - For networking
- **miniupnpc** - For UPnP support

## 🎯 Best Practices

1. **Keep submodules updated**
   ```bash
   git submodule update --recursive --remote
   ```

2. **Rebuild after updates**
   ```bash
   ./build-vendored-secp256k1.sh
   ```

3. **Test after updates**
   ```bash
   make clean && make test
   ```

4. **Version control vendor/**
   - Commit `vendor/` to Git
   - Or document exact build steps in CI

## 🚀 CI/CD Integration

Example GitHub Actions:
```yaml
- name: Build Vendored Dependencies
  run: |
    ./build-vendored-secp256k1.sh

- name: Build DineroCoin
  run: |
    mkdir build && cd build
    cmake ..
    make -j$(nproc)

- name: Verify No Homebrew Dependencies
  run: |
    ! otool -L bin/dinerod | grep homebrew
```

---

**Questions?** Check the scripts:
- `./setup-vendored-secp256k1.sh` - One-step setup
- `./build-vendored-secp256k1.sh` - Build only
- `./update-cmake-for-vendored-secp256k1.sh` - Update CMakeLists only

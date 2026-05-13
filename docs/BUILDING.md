# Building DineroCoin

Build from one codebase into three binaries.

## Quick Start

### Linux
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDIN_WITH_ROCKSDB=OFF
cmake --build build --target dinerod
```

### macOS
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDIN_WITH_ROCKSDB=OFF
cmake --build build --target dinerod
```

### Windows (MSVC)
```cmd
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDIN_WITH_ROCKSDB=OFF
cmake --build build --target dinerod
```

## Build Options

- `DIN_WITH_ROCKSDB=ON/OFF` - Enable RocksDB storage backend
- `DIN_BUILD_GUI=ON/OFF` - Build Qt GUI (requires Qt6)
- `DIN_ENABLE_P2P=ON/OFF` - Enable P2P networking layer
- `DIN_JSON_JSONCPP=ON/OFF` - Use JsonCpp instead of nlohmann/json
- `ENABLE_LIGHTNING=ON/OFF` - Enable Lightning Network support (default: OFF)

### Lightning Network Option

Lightning Network support is **disabled by default** to reduce binary size and build time.

**Default build (Lightning disabled):**
```bash
cmake -B build -DENABLE_LIGHTNING=OFF  # Default
cmake --build build
```

**Enable Lightning Network:**
```bash
cmake -B build -DENABLE_LIGHTNING=ON
cmake --build build
```

**Benefits of disabling Lightning:**
- Reduced binary size (~1-2 MB savings)
- Faster build time (~10-15% reduction)
- Simpler dependency tree

**Note:** Lightning support is currently undergoing refactoring (see `docs/PHASE3_BUILD_DECOUPLING.md`). When enabled, you may encounter compilation errors related to UTXO type mismatches. These will be resolved in Phase 3.

## Dependencies

### Core (Required)
- CMake 3.20+
- C++17 compiler
- OpenSSL
- SQLite3

### Optional
- Qt6 (for GUI)
- RocksDB (for storage backend)

## Architecture

- **Core code is STL-only** (no Qt)
- **GUI lives in src/gui/** and is built only with `-DDIN_BUILD_GUI=ON`
- **Platform-specific code** is isolated in `src/platform/{posix,windows,apple}/`
- **No file globs in CMake** - explicit source lists

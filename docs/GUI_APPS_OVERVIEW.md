# DineroCoin GUI Applications Overview

## Current GUI Applications

### 1. **dinero-desktop** (Developer Tool)
**Location**: `src/gui-desktop/` → `build/bin/dinero-desktop.app`
**Purpose**: Developer database inspection and management tool
**Features**:
- SQLite database inspection
- Wallet data analysis
- Database migrations
- RAII transaction management
- Background job monitoring
- Developer debugging tools

**Target Users**: Developers, database administrators, technical users

### 2. **dinero-qt** (User-Facing App) ⭐
**Location**: `dinero-qt/` → `build-qt/dinero-qt`
**Purpose**: Main user application for regular users
**Features**:
- **Wallet**: Create/load wallets, send/receive, address management
- **Mining**: Start/stop mining, set payout address, real-time status
- **Explorer**: Browse blockchain, view blocks/transactions
- **Security**: Hardened RPC client, vNext-only, no legacy calls
- **Modern UI**: Qt6/QML interface with capability-driven rendering

**Target Users**: Regular users, miners, wallet users

### 3. **dinero-mining-gui** (Mining Focused)
**Location**: `src/gui/mining_gui_mvp.cpp` → `build/bin/dinero-mining-gui`
**Purpose**: Simple mining interface
**Features**:
- Mining dashboard with real-time status
- Address management
- Start/stop mining controls
- Basic RPC integration

**Target Users**: Users who only want to mine

### 4. **dinero-modern-gui** (Legacy/Experimental)
**Location**: `src/gui/modern_all_in_one.cpp` → `build/bin/dinero-modern-gui.app`
**Purpose**: Experimental all-in-one interface
**Features**:
- Combined daemon + GUI
- WebSocket support
- Ephemeral ports
- Development/testing

**Target Users**: Developers, testers

## Recommended User Experience

### For Regular Users
**Primary App**: `dinero-qt` (hardened Qt GUI)
- Complete wallet functionality
- Mining capabilities
- Blockchain explorer
- Secure, vNext-only RPC
- Modern, intuitive interface

### For Developers
**Primary App**: `dinero-desktop` (developer tool)
- Database inspection
- Wallet data analysis
- Migration tools
- Debugging capabilities

### For Mining-Only Users
**Alternative**: `dinero-mining-gui` (simple mining interface)
- Focused mining dashboard
- Minimal complexity
- Real-time mining status

## Build and Run Commands

### User-Facing App (dinero-qt)
```bash
# Build
cmake -S dinero-qt -B build-qt -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos"
cmake --build build-qt -j8

# Run
DIN_DATADIR=/tmp/din-ui/regtest ./build-qt/dinero-qt
```

### Developer Tool (dinero-desktop)
```bash
# Build (already in main build)
cmake --build build --target dinero-desktop -j8

# Run
./build/bin/dinero-desktop.app/Contents/MacOS/dinero-desktop
```

### Mining GUI
```bash
# Build (already in main build)
cmake --build build --target dinero-mining-gui -j8

# Run
./build/bin/dinero-mining-gui
```

## Security Comparison

| Feature | dinero-qt | dinero-desktop | dinero-mining-gui |
|---------|-----------|----------------|-------------------|
| vNext-only RPC | ✅ | ❌ | ❌ |
| Allow-listed methods | ✅ | ❌ | ❌ |
| Schema validation | ✅ | ❌ | ❌ |
| Legacy detection | ✅ | ❌ | ❌ |
| Cookie auth | ✅ | ✅ | ✅ |
| Build-time scanning | ✅ | ❌ | ❌ |

## User Journey

### New User
1. **Download** `dinero-qt` (user-facing app)
2. **Start daemon** `./build/bin/dinerod --regtest --datadir=/tmp/din-ui --httpport=20999 -gen=0`
3. **Launch GUI** `DIN_DATADIR=/tmp/din-ui/regtest ./build-qt/dinero-qt`
4. **Create wallet** → Generate addresses → Start mining

### Developer
1. **Download** `dinero-desktop` (developer tool)
2. **Inspect databases** → Analyze wallet data → Debug issues
3. **Use CLI** `./build/bin/dinerod` for advanced operations

### Miner
1. **Choose app**: `dinero-qt` (full) or `dinero-mining-gui` (simple)
2. **Set mining address** → Start mining → Monitor status

## Next Steps

### Immediate
- **Promote dinero-qt** as the main user application
- **Document the difference** between developer and user tools
- **Create user guides** for dinero-qt
- **Package releases** with dinero-qt as primary GUI

### Future
- **Enhance dinero-qt** with full wallet functionality
- **Add explorer features** to dinero-qt
- **Improve mining interface** in dinero-qt
- **Consider deprecating** older GUI variants

## Conclusion

**dinero-qt** is the recommended user-facing application with:
- Complete functionality (wallet + mining + explorer)
- Security guardrails (vNext-only, no legacy)
- Modern interface (Qt6/QML)
- Capability-driven UI (adapts to daemon features)

**dinero-desktop** remains the developer tool for database inspection and debugging.

Users should use **dinero-qt** for all regular operations.

# Dinero Grandma-Ready GUI Design

## 🎯 Vision: "Press Start" Simplicity

The Dinero Grandma-Ready GUI makes cryptocurrency mining as simple as using a microwave. Anyone can open the app and start mining in seconds, without technical knowledge.

## ✨ Key Features

### 🚀 One-Click Mining
- **Giant START/STOP button** - The most prominent element on screen
- **Automatic daemon management** - No need to manually start the node
- **Cookie authentication** - Zero password management
- **Address generation** - One-click wallet creation

### 🔒 Grandma-Safe Design
- **Battery protection** - Automatically pauses on laptop battery
- **CPU throttling** - Smart core allocation (leaves 1 core for system)
- **Clear status indicators** - Green/yellow/red connection status
- **No confusing technical terms** - Everything in plain English

### 📊 Essential Information Only
- **Connection status** - Node connected/syncing/offline
- **Mining address** - Your wallet address with copy/QR buttons
- **Real-time stats** - Hash rate, blocks found, estimated rewards
- **System info** - Disk usage, network (testnet/mainnet)

## 🏗️ Technical Architecture

### Cookie Authentication Flow
```cpp
// 1. Check for cookie file
QString cookiePath = "~/.dinero/testnet/.cookie";

// 2. Read cookie and set auth header
QByteArray cookie = readFile(cookiePath).trimmed();
m_authHeader = "Basic " + cookie.toBase64();

// 3. Watch for cookie changes (daemon restart)
m_cookieWatcher->addPath(cookiePath);
```

### RPC Integration
The GUI uses the existing daemon RPC methods:
- `getblockchaininfo` - Connection status and sync progress
- `getnewaddress` - Generate mining wallet address
- `miner.start` - Start mining with address and thread count
- `miner.stop` - Stop mining
- `miner.status` - Get hash rate and mining statistics

### Automatic Daemon Management
```cpp
void connectToDaemon() {
    updateCookieAuth();
    if (cookieNotFound) {
        startDaemon();  // Launch daemon automatically
        waitForCookie();
    }
    testConnection();
}
```

## 🎨 UI Design Principles

### Visual Hierarchy
1. **Connection Status** (top) - Most important for safety
2. **Mining Address** - Your money destination
3. **Giant Start/Stop Button** - Primary action
4. **Mining Stats** - Feedback and progress
5. **System Info** - Context and settings

### Color Coding
- 🟢 **Green** - Connected, mining, success
- 🟡 **Yellow** - Connecting, syncing, warnings
- 🔴 **Red** - Offline, errors, stopped
- 🔵 **Blue** - Information, statistics

### Typography
- **24px bold** - Start/Stop button
- **14px bold** - Status indicators
- **12px regular** - Statistics and info
- **Monospace** - Addresses and technical data

## 📱 Responsive Layout

### Main Window (600x500 minimum)
```
┌─────────────────────────────────────┐
│ 🌐 Connection Status: 🟢 Connected  │
├─────────────────────────────────────┤
│ 💰 Your Mining Wallet              │
│ [Address] [Generate] [Copy] [QR]    │
├─────────────────────────────────────┤
│        🚀 START MINING              │
│     (Giant 80px button)             │
│ CPU Cores: [====|---] 4 of 8        │
│ ⚡ Pause mining when on battery     │
├─────────────────────────────────────┤
│ 📊 Mining Statistics               │
│ Hash Rate: 1.2 KH/s  Blocks: 0     │
│ Time: 00:05:23      Rewards: 0 DIN │
├─────────────────────────────────────┤
│ 💻 Disk: 2.1 GB    Network: Testnet│
│                          ⚙️ Settings│
└─────────────────────────────────────┘
```

## 🔧 Implementation Status

### ✅ Completed
- **Core GUI structure** - Main window with all sections
- **Cookie authentication** - Automatic daemon connection
- **RPC integration** - All necessary method calls
- **CMake integration** - Build target `dinero-grandma-qt6`
- **macOS app bundle** - Proper .app packaging

### 🚧 In Progress
- **Daemon auto-start** - Launch daemon if not running
- **First-run wizard** - Welcome flow for new users
- **Settings dialog** - Network selection, data directory

### 📋 Planned
- **QR code display** - Show mining address as QR
- **Backup reminder** - Nag until wallet is backed up
- **Battery detection** - Platform-specific power status
- **Thermal protection** - CPU temperature monitoring
- **Windows/Linux support** - Cross-platform deployment

## 🚀 Build Instructions

### Prerequisites
- Qt6 (Widgets, Network, Core)
- Official Qt installation (not Homebrew)
- CMake with `WITH_QT=ON`

### Build Commands
```bash
# Configure with Qt6
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/Applications/Qt/6.7.2/macos" \
  -DWITH_QT=ON

# Build grandma GUI
cmake --build build --target dinero-grandma-qt6 --parallel

# Run the app
./build/bin/dinero-grandma-qt6.app/Contents/MacOS/dinero-grandma-qt6
```

### Verification
```bash
# Check for clean Qt bundling (no Homebrew deps)
otool -L build/bin/dinero-grandma-qt6.app/Contents/MacOS/dinero-grandma-qt6 | grep Qt
# Should show: @rpath/Qt*.framework (bundled inside app)
```

## 🎯 Success Metrics

### User Experience Goals
- **< 30 seconds** from app launch to mining start
- **Zero technical knowledge** required
- **One-click** address generation and mining
- **Clear visual feedback** at all times
- **Safe defaults** for all settings

### Technical Goals
- **< 2 seconds** RPC response time
- **Automatic recovery** from daemon restarts
- **Cross-platform compatibility** (macOS/Windows/Linux)
- **Hermetic deployment** (no external dependencies)

## 🌟 Future Enhancements

### Phase 2: Smart Features
- **Automatic address backup** to iCloud/Google Drive
- **Mining scheduler** - Mine only during off-peak hours
- **Profit calculator** - Real-time USD/EUR conversion
- **Pool mining support** - Connect to mining pools

### Phase 3: Social Features
- **Mining leaderboard** - Compare with friends
- **Achievement system** - Gamify the mining experience
- **Community chat** - Built-in miner community
- **Referral rewards** - Invite friends to mine

## 💡 Design Philosophy

> **"If grandma can't use it, it's not ready."**

Every design decision prioritizes simplicity over features. The GUI should feel familiar to anyone who has used a smartphone or basic computer application. Technical complexity is hidden behind intuitive interfaces and smart defaults.

The goal is to make Dinero the **most accessible cryptocurrency** in the world, where anyone can participate in the network without technical barriers.

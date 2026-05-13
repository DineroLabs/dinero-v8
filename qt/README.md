# Dinero-Qt

Desktop wallet GUI for DineroCoin.

## Why Separate?

Dinero-Qt is intentionally a **separate project** from dinerod (the daemon):

| Concern | GUI | Daemon |
|---------|-----|--------|
| **Bugs** | UI glitches, rendering issues | Consensus failures |
| **Crashes** | App closes, user restarts | Network fork risk |
| **Updates** | Frequent (UX improvements) | Rare (consensus changes) |
| **Attack Surface** | Huge (Qt, fonts, images, translations) | Minimal (crypto, P2P) |

> "GUI bugs ≠ consensus bugs"

This follows Bitcoin Core's architecture: `bitcoind` and `bitcoin-qt` are separate binaries.

## Features

- **Wallet Management** - Create, restore, and manage wallets
- **Send/Receive** - Send and receive DIN with QR code support
- **Transaction History** - View all transactions
- **Debug Console** - Direct RPC access for power users
- **Hardware Wallet** - Ledger/Trezor support (experimental)
- **Real-time Updates** - WebSocket connection for live data

## Architecture

```
┌─────────────┐     RPC      ┌─────────────┐
│   dinerod   │◄────────────►│  dinero-qt  │
│  (daemon)   │  HTTP/JSON   │   (GUI)     │
│  consensus  │              │   display   │
└─────────────┘              └─────────────┘
```

The GUI **never** touches consensus code. It only:
- Parses user input
- Renders data
- Communicates via RPC

## Build

### Prerequisites

- CMake 3.21+
- Qt 6.x (Widgets, Network)
- C++17 compiler

### macOS

```bash
# Configure (adjust Qt path as needed)
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos"

# Build
cmake --build build -j8

# Run
open build/bin/dinero-qt.app
# or
./build/bin/dinero-qt.app/Contents/MacOS/dinero-qt
```

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/bin/dinero-qt
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `DIN_EXPERIMENTAL_FEATURES` | OFF | Enable experimental features |
| `DIN_ENABLE_WEBSOCKETS` | ON | Real-time updates via WebSocket |
| `DIN_ENABLE_QRENCODE` | OFF | Use libqrencode (vs built-in) |

## Usage

### 1. Start dinerod first

```bash
./dinerod -rpcport=20996 -rpcuser=user -rpcpassword=pass
```

### 2. Launch dinero-qt

```bash
./dinero-qt
```

Or on macOS:
```bash
open dinero-qt.app
```

### 3. Connect

The GUI will automatically connect to `localhost:20996`. Use the settings to change the RPC endpoint.

## Directory Structure

```
dinero-qt/
├── src/
│   ├── main.cpp              # Entry point
│   ├── mainwindow.cpp/h      # Main application window
│   ├── rpcclient.cpp/h       # RPC communication
│   ├── connection_manager.cpp/h
│   ├── walletwizard.cpp/h    # Wallet creation wizard
│   ├── debugconsole.cpp/h    # Debug/RPC console
│   ├── QrUtil.cpp/h          # QR code utilities
│   ├── qrcodegen.cpp/h       # Built-in QR generator
│   └── hardwarewalletwidget.cpp/h
├── qml/                      # QML resources (if enabled)
├── icons/                    # App icons
├── *.png, *.icns            # Logo assets
├── Info.plist.in            # macOS bundle info
└── CMakeLists.txt
```

## Development

### Enable Experimental Features

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DDIN_EXPERIMENTAL_FEATURES=ON
```

This enables:
- Bridge widget
- Payments widget
- Escrow widget
- Marketplace widget

### Debug Console

The built-in debug console allows direct RPC calls:

```
> getblockchaininfo
> getbalance
> getnewaddress
```

## License

Same as DineroCoin - see LICENSE file.

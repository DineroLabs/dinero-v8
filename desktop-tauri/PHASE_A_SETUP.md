# Dinero Desktop - Phase A Setup

**Status**: Ready for post-mainnet development  
**Timeline**: Build after mainnet stabilizes  
**Effort**: 1-2 days focused work

## Prerequisites

```bash
# Install Rust (stable)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Install Node.js 18+ and pnpm
brew install node pnpm  # macOS
# or use nvm/volta

# Install Tauri CLI
cargo install tauri-cli --version "^2.0"

# Platform-specific deps
# macOS: Xcode Command Line Tools
xcode-select --install

# Windows: Visual Studio Build Tools + WebView2
# Linux (Ubuntu): sudo apt install libgtk-3-dev libayatana-appindicator3-dev libwebkit2gtk-4.1-dev
```

## Quick Start (Post-Mainnet)

```bash
cd desktop-tauri/

# Install JS dependencies
pnpm install

# Run in development mode (hot reload)
pnpm tauri dev

# Build production installers
pnpm tauri build
```

## What Phase A Delivers

✅ **Node Discovery**: Reads `nodeinfo.json` from standard locations  
✅ **Cookie Authentication**: Secure RPC connection  
✅ **Status Panel**: Live blockchain info, network status  
✅ **Cross-Platform**: Native installers (.dmg, .msi, .AppImage)  
✅ **Security**: CSP-hardened, localhost-only connections  

## Architecture

- **Backend**: Rust (Tauri) - handles RPC, file system, native APIs
- **Frontend**: React + TypeScript - modern UI components  
- **RPC Contract**: Uses `/rpc/schemas/din.rpc.v1/` specifications
- **Authentication**: Reads `.cookie` file (same as Qt6 GUI)

## Network Support

- **Mainnet**: Read-only by default (safe for production)
- **Testnet**: Full functionality enabled
- **Regtest**: Development mode, all features enabled

## Future Phases

- **Phase B**: Wallet operations (send/receive, PSBT signing)
- **Phase C**: Mining panel, transaction history, QR codes
- **Phase D**: Distribution polish, auto-updates, crash reporting

## Testing

```bash
# Start regtest node
dinerod -regtest -datadir=./test-data

# Launch GUI (should auto-connect)
pnpm tauri dev

# Verify: Status panel shows blocks, network info
```

## Build Outputs

After `pnpm tauri build`:

```
src-tauri/target/release/bundle/
├── macos/
│   ├── Dinero Desktop.app
│   └── Dinero Desktop.dmg
├── msi/
│   └── Dinero Desktop_0.1.0_x64_en-US.msi  
└── appimage/
    └── dinero-desktop_0.1.0_amd64.AppImage
```

## Integration with Core

This GUI is **completely decoupled** from the daemon:

- Reads same `nodeinfo.json` as Qt6 GUI
- Uses same RPC endpoints and cookie auth
- No changes needed to `dinerod` codebase
- Can coexist with Qt6 GUI

## Why Phase A First?

1. **Proof of Concept**: Validates the architecture
2. **User Feedback**: Early testing on all platforms  
3. **Distribution**: Tests installer/signing pipeline
4. **Foundation**: Sets up for wallet features in Phase B

Ready to build when mainnet is stable! 🚀

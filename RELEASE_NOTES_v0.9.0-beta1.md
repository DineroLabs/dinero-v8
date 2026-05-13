# Dinero Desktop v0.9.0-beta1 Release Notes

🎉 **Welcome to the first beta release of Dinero Desktop with real-time WebSocket events!**

## 🚀 New Features

### **Real-Time Event System**
- **WebSocket Integration**: Live updates for blocks, wallet transactions, mempool activity, and mining status
- **Instant Notifications**: No more polling delays - see events as they happen
- **Professional UX**: Connection status indicators with "🟢 Live" and "🟡 Reconnecting..." states
- **Graceful Fallback**: Automatically falls back to polling if WebSocket unavailable

### **Enhanced Mining Experience**
- **Live Mining Updates**: Real-time hashrate and thread monitoring
- **Mining Status API**: New `mining.status` RPC method with comprehensive metrics
- **Visual Feedback**: Instant start/stop confirmation and activity logging

### **Bulletproof Authentication**
- **HTTP Basic Auth**: Cookie-based authentication with automatic rotation support
- **nodeinfo.json Discovery**: Automatic RPC port and cookie path detection
- **Resilient Connection**: Auto-reconnect with exponential backoff and session recovery

### **Professional Desktop Wallet**
- **Modern Qt6 Interface**: Clean, responsive design with native macOS integration
- **PSBT Workflow**: Complete Partially Signed Bitcoin Transaction support
- **Real-Time Explorer**: Live mempool monitoring and transaction streaming
- **Send Tab**: Secure transaction creation with mainnet safety guards

## 🔒 Security & Compliance

- **Schema Contract**: All RPC responses include `rpc_schema: "din.rpc.v1"` and `schema_rev: 1`
- **Mainnet Protection**: Beta version defaults to regtest/testnet with explicit mainnet warnings
- **Cookie Security**: Secure cookie-based authentication with automatic file watching
- **Session Management**: Unique WebSocket session IDs with proper cleanup

## 🌐 Network Safety (Beta Defaults)

- **Regtest Default**: Safe testing environment with no real DIN at risk
- **Mainnet Gating**: Explicit checkbox required for mainnet transactions
- **Beta Warnings**: Clear UI indicators that this is a beta version
- **Development Focus**: Optimized for testing and development workflows

## 🛠️ Technical Improvements

- **WebSocket Server**: Beast-based WebSocket server on port 21001
- **Event Subscription**: `events.subscribe` API with topic-based filtering
- **Connection Resilience**: Automatic reconnection with jitter and backoff
- **Performance**: Efficient real-time updates without polling overhead

## ⚠️ Known Beta Limitations

- **Server Hardening**: WebSocket keepalive, rate limiting, and idle cleanup are minimal
- **Event Coverage**: Some daemon events may not yet stream (falls back to polling)
- **Cross-Platform**: Primary testing on macOS; Windows/Linux packages coming soon
- **Mining Integration**: Full mining metrics integration still in development

## 📋 Requirements

- **macOS**: 10.15+ (Catalina or later)
- **Qt6**: 6.9.1+ (bundled in release package)
- **Network**: Local daemon or remote RPC access
- **Storage**: ~50MB for application bundle

## 🔧 Installation

1. Download `dinero-desktop-v0.9.0-beta1.dmg`
2. Mount the DMG and drag Dinero Desktop to Applications
3. Launch and connect to your daemon via automatic `nodeinfo.json` discovery
4. For first-time setup, ensure your daemon is running with WebSocket support

## 🧪 Beta Testing Focus

**Please test and report:**
- ✅ Real-time event streaming (mining, blocks, transactions)
- ✅ Connection resilience (daemon restart, network drops)
- ✅ Authentication robustness (cookie rotation, session recovery)
- ✅ Cross-network compatibility (regtest, testnet, mainnet)
- ✅ PSBT transaction workflows
- ✅ GUI responsiveness under load

## 🐛 Known Issues

- CMake build warnings for missing meta-targets (fixed in this release)
- Some mining metrics may show placeholder values
- WebSocket server optimization ongoing

## 🚀 What's Next (Post-Beta)

- **Server Hardening**: Ping/pong keepalive, rate limits, idle timeouts
- **Event Streaming**: Complete daemon event integration
- **Cross-Platform**: Windows MSI and Linux AppImage packages
- **Performance**: WebSocket connection pooling and metrics
- **UX Polish**: First-run setup wizard and connection diagnostics

## 💬 Feedback & Support

This is a **beta release** - your feedback is crucial!

- **Issues**: Report bugs and feature requests
- **Testing**: Focus on real-time functionality and connection stability
- **Performance**: Monitor CPU/memory usage during extended sessions

---

**🎯 This beta represents a major leap forward in cryptocurrency wallet UX with professional real-time event streaming. Thank you for testing and helping make Dinero Desktop production-ready!**

*Dinero Development Team*  
*September 2025*

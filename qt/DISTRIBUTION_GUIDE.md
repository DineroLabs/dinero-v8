# Dinero-Qt Distribution Guide (For Developers)

**Purpose**: How to package and distribute the GUI for end users  
**Audience**: Developers building release packages

---

## 🎯 Distribution Goals

The GUI must work **out-of-the-box** for users who:
1. Download a single package
2. Extract/install it
3. Run the daemon → Run the GUI → It works!

**No complex setup, no SSH tunnels, no hardcoded servers!**

---

## 📦 What to Bundle

### Minimum Package Contents

```
DineroCoin/
├── dinerod              # Blockchain daemon
├── dinero-miner         # CPU miner (optional but recommended)
├── dinero-qt            # GUI application
├── dinero-cli           # Command-line interface (optional)
├── README.txt           # Quick start instructions
└── LICENSE
```

### macOS `.dmg` Package

```
DineroCoin.dmg
└── DineroCoin.app/
    └── Contents/
        ├── MacOS/
        │   ├── dinero-qt        # GUI executable
        │   ├── dinerod          # Daemon
        │   └── dinero-miner     # Miner
        ├── Resources/
        │   ├── icon.icns
        │   ├── README.txt
        │   └── docs/
        └── Info.plist
```

**Installation**: User drags `DineroCoin.app` to `/Applications`

### Windows `.exe` Installer

```
DineroCoin-Setup.exe
└── Installs to: C:\Program Files\DineroCoin\
    ├── dinero-qt.exe
    ├── dinerod.exe
    ├── dinero-miner.exe
    ├── Qt libraries (Qt6Core.dll, etc.)
    ├── README.txt
    └── Uninstall.exe
```

**Installation**: Standard Windows installer with Start Menu shortcuts

### Linux Packages

**AppImage** (Recommended - works everywhere):
```
DineroCoin-Qt-x86_64.AppImage
# Self-contained, no installation needed
# Just: chmod +x DineroCoin-Qt-x86_64.AppImage && ./DineroCoin-Qt-x86_64.AppImage
```

**Debian/Ubuntu (.deb)**:
```
/usr/bin/
├── dinerod
├── dinero-miner
└── dinero-qt

/usr/share/applications/
└── dinerocoin-qt.desktop

/usr/share/pixmaps/
└── dinerocoin.png
```

**RPM** (Fedora/RHEL): Similar structure

---

## ⚙️ Default Configuration

### RPC Server Settings (Built-in)

The GUI is hardcoded to try:
1. `http://127.0.0.1:20998/` (testnet - primary)
2. `http://127.0.0.1:20997/` (mainnet - fallback)

**Users can override via environment variable**:
```bash
export DINERO_RPC_URL="http://custom:20998/"
```

### Data Directory Defaults

**macOS**: `~/Library/Application Support/Dinero`  
**Linux**: `~/.dinero`  
**Windows**: `%APPDATA%\Dinero`

**Users can override**:
```bash
export DINERO_DATA_DIR="/custom/path"
```

---

## 🚫 What NOT to Include in Distribution

### ❌ Don't Hardcode Developer-Specific Settings

**Bad**:
```cpp
// DON'T DO THIS!
servers_ = {
  QUrl("http://173.249.195.59:20998/"),  // Your specific server!
  QUrl("http://96.9.226.98:20998/"),     // Another dev server!
};
```

**Good**:
```cpp
// Do this instead
servers_ = {
  QUrl("http://127.0.0.1:20998/"),  // Local testnet (works for everyone)
  QUrl("http://127.0.0.1:20997/")   // Local mainnet (works for everyone)
};
```

### ❌ Don't Include Development Scripts

Files like these are for **your** development only:
- `setup-tunnels.sh` (specific to your servers)
- `fetch-testnet-cookies.sh` (specific to your servers)
- `test-*.sh` scripts
- SSH keys or credentials
- Hardcoded IP addresses

**Move these to a separate `/dev-tools/` folder** that's NOT distributed.

---

## 📝 Build Process

### macOS Build

```bash
# Build all components
cmake --build build-clean --target all

# Create .app bundle
macdeployqt build-gui/dinero-qt.app

# Bundle daemon and miner
cp build-clean/dinerod build-gui/dinero-qt.app/Contents/MacOS/
cp build-clean/dinero-miner build-gui/dinero-qt.app/Contents/MacOS/

# Create .dmg
hdiutil create -volname "DineroCoin" -srcfolder build-gui/dinero-qt.app \
  -ov -format UDZO DineroCoin-v1.0.0-macOS.dmg

# Sign and notarize (for distribution)
codesign --deep --force --verify --verbose \
  --sign "Developer ID Application: Your Name" \
  build-gui/dinero-qt.app
```

### Windows Build

```bash
# Build with Visual Studio or MinGW
cmake --build build-release --config Release

# Use windeployqt to bundle Qt libraries
windeployqt.exe build-release/dinero-qt.exe

# Create installer with NSIS or InnoSetup
# (See packaging/windows/installer.nsi)
makensis installer.nsi
```

### Linux Build

```bash
# Build statically linked binaries (recommended)
cmake -DBUILD_STATIC=ON ..
cmake --build build-release

# Create AppImage
linuxdeployqt dinero-qt -appimage

# Or build distribution packages
# Debian:
dpkg-deb --build dinerocoin-qt
# RPM:
rpmbuild -bb dinerocoin-qt.spec
```

---

## 📋 Pre-Release Checklist

### Code Review

- [ ] No hardcoded IP addresses or server URLs
- [ ] No development-specific paths
- [ ] No SSH tunnel dependencies
- [ ] Environment variables properly handled
- [ ] All paths are portable (no absolute paths)
- [ ] Works on clean system (test in VM)

### Configuration Validation

- [ ] Default RPC URLs are localhost only
- [ ] Data directories use standard OS locations
- [ ] Cookie authentication works automatically
- [ ] No credentials hardcoded
- [ ] All external dependencies bundled

### Testing

- [ ] Fresh install on clean macOS (test in VM)
- [ ] Fresh install on clean Windows (test in VM)
- [ ] Fresh install on clean Linux (test in VM)
- [ ] Daemon connects automatically
- [ ] GUI connects to daemon
- [ ] Wallet creation works
- [ ] Send/receive works
- [ ] Mining works

### Documentation

- [ ] USER_GUIDE.md is complete
- [ ] README.txt in package has quick start
- [ ] Version number is correct everywhere
- [ ] Changelog is updated
- [ ] Known issues documented

---

## 🔧 Development vs Production

### Development Environment (Your Setup)

You can use **advanced features** for development:

```bash
# dev-tools/setup-tunnels.sh
# (Not distributed - just for your testing)
ssh -f -N -L 20991:127.0.0.1:20998 root@your-testnet-server

# dev-tools/dev-config.sh
export DINERO_RPC_URL="http://127.0.0.1:20991/"
export DINERO_DATA_DIR="/Users/you/dev/dinero-data"
./build-gui/dinero-qt
```

**Keep all dev-specific scripts in `dev-tools/` folder!**

### Production Package (For Users)

Simple, clean, no dev dependencies:

```bash
# User extracts package
unzip DineroCoin-v1.0.0.zip

# Start daemon
./dinerod --testnet

# Open GUI
./dinero-qt

# That's it! No configuration needed!
```

---

## 📊 Release Workflow

### 1. Prepare Release

```bash
# Update version numbers
VERSION="1.0.0"

# Update in:
# - CMakeLists.txt
# - src/version.h
# - README.md
# - CHANGELOG.md

# Tag release
git tag -a v${VERSION} -m "Release v${VERSION}"
git push origin v${VERSION}
```

### 2. Build for All Platforms

```bash
# macOS
./build-scripts/build-macos.sh

# Windows (on Windows machine or cross-compile)
./build-scripts/build-windows.sh

# Linux
./build-scripts/build-linux.sh
```

### 3. Create Packages

```bash
# macOS: DineroCoin-v1.0.0-macOS.dmg
# Windows: DineroCoin-v1.0.0-Windows-Setup.exe
# Linux: DineroCoin-v1.0.0-Linux-x86_64.AppImage
#        DineroCoin-v1.0.0-Linux-amd64.deb
#        DineroCoin-v1.0.0-Linux-x86_64.rpm
```

### 4. Test Packages

**Critical**: Test on **clean systems** (VMs):
- [ ] macOS VM (fresh install)
- [ ] Windows VM (fresh install)
- [ ] Ubuntu VM (fresh install)

**Test flow**:
1. Install package
2. Run daemon
3. Run GUI
4. Create wallet
5. Test all features
6. No errors in console

### 5. Upload Release

```bash
# Create checksums
shasum -a 256 DineroCoin-v1.0.0-* > SHA256SUMS.txt

# Sign checksums
gpg --clearsign SHA256SUMS.txt

# Upload to GitHub releases
gh release create v${VERSION} \
  --title "Dinero v${VERSION}" \
  --notes-file CHANGELOG.md \
  DineroCoin-v1.0.0-*

# Update website download links
```

---

## 🔒 Security Considerations

### Code Signing

**macOS**:
```bash
# Sign app bundle
codesign --deep --force --sign "Developer ID Application" dinero-qt.app

# Notarize (required for macOS 10.15+)
xcrun notarytool submit DineroCoin.dmg \
  --apple-id your@email.com \
  --password @keychain:AC_PASSWORD \
  --team-id TEAM_ID
```

**Windows**:
```bash
# Sign with Authenticode
signtool sign /f certificate.pfx /p password \
  /tr http://timestamp.digicert.com \
  DineroCoin-Setup.exe
```

### Binary Verification

Provide checksums for users to verify downloads:

```bash
# In SHA256SUMS.txt
a1b2c3d4... DineroCoin-v1.0.0-macOS.dmg
e5f6g7h8... DineroCoin-v1.0.0-Windows-Setup.exe
i9j0k1l2... DineroCoin-v1.0.0-Linux-x86_64.AppImage
```

Users can verify:
```bash
shasum -a 256 -c SHA256SUMS.txt
```

---

## 📚 Documentation for Users

### Include in Package

1. **README.txt** - Quick start (1 page)
2. **USER_GUIDE.md** - Full guide (comprehensive)
3. **LICENSE** - Software license
4. **CHANGELOG.md** - Version history

### Quick Start README Example

```text
==================================================
        DINEROCOIN WALLET - QUICK START
==================================================

STEP 1: START THE DAEMON
-------------------------
macOS/Linux:
  ./dinerod --testnet

Windows:
  dinerod.exe --testnet

STEP 2: OPEN THE GUI
--------------------
macOS:
  Open DineroCoin.app from Applications

Linux:
  ./dinero-qt

Windows:
  Double-click dinero-qt.exe (or use Start Menu)

STEP 3: CREATE WALLET
----------------------
Follow the on-screen setup wizard.
IMPORTANT: Write down your 12-word seed phrase!

HELP & SUPPORT
--------------
Full guide: docs/USER_GUIDE.md
Discord: https://discord.gg/dinero
Email: support@dinero-coin.com

==================================================
```

---

## ✅ Distribution Checklist

Before releasing:

- [ ] All binaries are portable (no absolute paths)
- [ ] Defaults work for typical user (localhost)
- [ ] No dev-specific configuration included
- [ ] All dependencies bundled
- [ ] Tested on fresh VMs for each platform
- [ ] Documentation is complete and accurate
- [ ] Version numbers updated everywhere
- [ ] Checksums generated and verified
- [ ] Binaries are signed (macOS/Windows)
- [ ] GitHub release created with all assets
- [ ] Website download links updated
- [ ] Announcement prepared (Discord/Twitter/Reddit)

---

## 🎯 Success Metrics

A good distribution means users can:

1. ✅ Download one file/package
2. ✅ Extract or install
3. ✅ Run daemon with simple command
4. ✅ Open GUI → it connects automatically
5. ✅ Create wallet → start using
6. ✅ No configuration needed
7. ✅ No technical knowledge required

**If users need SSH tunnels, environment variables, or manual config → distribution failed!**

---

## 📞 Support Plan

### Common User Issues

**"Connection Refused"**
→ Solution: Start daemon first (add to README in bold)

**"Unauthorized"**
→ Solution: Wait for daemon to fully start (5 seconds)

**"Can't find miner"**
→ Solution: Ensure dinero-miner is bundled in package

**"Sync is slow"**
→ Solution: Normal, explain in docs

### Support Channels

- Discord (fastest response)
- GitHub Issues (bug reports)
- Email (general inquiries)
- Documentation (self-service)

---

**Next**: Review this guide, update build scripts, test in VMs, then distribute!

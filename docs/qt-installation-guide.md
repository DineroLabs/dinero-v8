# Qt Installation Guide for Dinero Hermetic Builds

This guide provides step-by-step instructions for installing official Qt on all supported platforms to enable hermetic builds of Dinero's GUI applications.

## 📦 Downloaded Installers

All Qt installers have been downloaded from the [official Qt releases page](https://download.qt.io/official_releases/online_installers/):

| Platform | File | Size | Architecture |
|----------|------|------|--------------|
| **macOS** | `qt-online-installer-mac-x64-online.dmg` | 22M | x64 |
| **Windows** | `qt-online-installer-windows-x64-online.exe` | 52M | x64 |
| **Windows** | `qt-online-installer-windows-arm64-online.exe` | 31M | ARM64 |
| **Linux** | `qt-online-installer-linux-x64-online.run` | 70M | x64 |
| **Linux** | `qt-online-installer-linux-arm64-online.run` | 72M | ARM64 |

## 🍎 macOS Installation

1. **Mount the installer**:
   ```bash
   hdiutil attach qt-online-installer-mac-x64-online.dmg
   ```

2. **Run the installer**:
   ```bash
   open "/Volumes/qt-online-installer-macOS-x64-4.10.0/qt-online-installer-macOS-x64-4.10.0.app"
   ```

3. **Installation settings**:
   - **Install to**: `/Applications/Qt/6.7.2/macos`
   - **Components**: Qt Base, Qt Widgets, Qt Network, Qt Core
   - **Tools**: CMake, Ninja (optional but recommended)

4. **Verify installation**:
   ```bash
   ls -la /Applications/Qt/6.7.2/macos/bin/
   # Should show qmake, macdeployqt, etc.
   ```

## 🪟 Windows Installation

### Windows x64

1. **Run the installer**:
   ```cmd
   qt-online-installer-windows-x64-online.exe
   ```

2. **Installation settings**:
   - **Install to**: `C:\Qt\6.7.2\msvc2022_64`
   - **Components**: Qt Base, Qt Widgets, Qt Network, Qt Core
   - **Compiler**: MSVC 2022 64-bit
   - **Tools**: CMake, Ninja

3. **Verify installation**:
   ```cmd
   dir "C:\Qt\6.7.2\msvc2022_64\bin\"
   ```

### Windows ARM64

1. **Run the installer**:
   ```cmd
   qt-online-installer-windows-arm64-online.exe
   ```

2. **Installation settings**:
   - **Install to**: `C:\Qt\6.7.2\msvc2022_arm64`
   - **Components**: Qt Base, Qt Widgets, Qt Network, Qt Core
   - **Compiler**: MSVC 2022 ARM64
   - **Tools**: CMake, Ninja

## 🐧 Linux Installation

### Linux x64

1. **Make executable and run**:
   ```bash
   chmod +x qt-online-installer-linux-x64-online.run
   ./qt-online-installer-linux-x64-online.run
   ```

2. **Installation settings**:
   - **Install to**: `/opt/Qt/6.7.2/gcc_64`
   - **Components**: Qt Base, Qt Widgets, Qt Network, Qt Core
   - **Compiler**: GCC 64-bit
   - **Tools**: CMake, Ninja

3. **Verify installation**:
   ```bash
   ls -la /opt/Qt/6.7.2/gcc_64/bin/
   ```

### Linux ARM64

1. **Make executable and run**:
   ```bash
   chmod +x qt-online-installer-linux-arm64-online.run
   ./qt-online-installer-linux-arm64-online.run
   ```

2. **Installation settings**:
   - **Install to**: `/opt/Qt/6.7.2/gcc_arm64`
   - **Components**: Qt Base, Qt Widgets, Qt Network, Qt Core
   - **Compiler**: GCC ARM64
   - **Tools**: CMake, Ninja

## 🔧 Build Configuration

After installing Qt, configure Dinero builds with the appropriate paths:

### macOS
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/Applications/Qt/6.7.2/macos" \
  -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew" \
  -DDINERO_VENDOR_ROCKSDB=ON -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON
```

### Windows x64
```cmd
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="C:\Qt\6.7.2\msvc2022_64" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DDINERO_VENDOR_ROCKSDB=ON -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON
```

### Windows ARM64
```cmd
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="C:\Qt\6.7.2\msvc2022_arm64" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DDINERO_VENDOR_ROCKSDB=ON -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON
```

### Linux x64
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/Qt/6.7.2/gcc_64" \
  -DDINERO_VENDOR_ROCKSDB=ON -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON
```

### Linux ARM64
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/Qt/6.7.2/gcc_arm64" \
  -DDINERO_VENDOR_ROCKSDB=ON -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON
```

## ✅ Verification

After installation and build, verify hermetic packaging:

### macOS
```bash
# Check that Qt frameworks are bundled inside the app
otool -L build/bin/dinero-qt6.app/Contents/MacOS/dinero-qt6 | grep Qt
# Should show @executable_path/../Frameworks/Qt*.framework paths

# Verify no external dependencies
otool -L build/bin/dinero-qt6.app/Contents/MacOS/dinero-qt6 | grep -v '@' | grep -v '/System/' | grep -v '/usr/lib/'
# Should be empty or only show system libraries
```

### Windows
```cmd
# Check dependencies
dumpbin /DEPENDENTS build\bin\dinero-qt6.exe
# Should not show any Qt DLLs (they should be bundled)
```

### Linux
```bash
# Check dependencies
ldd build/bin/dinero-qt6 | grep -i qt
# Should not show any system Qt libraries
```

## 🚨 Important Notes

1. **Hard Qt Guard**: The build system will fail if it detects Homebrew or system Qt installations
2. **Audit System**: Post-build audits automatically verify hermetic packaging
3. **Version Consistency**: Use Qt 6.7.2 across all platforms for consistency
4. **Component Selection**: Only install required components to minimize installation size

## 🎯 Benefits

- **Zero Runtime Dependencies**: Applications work on fresh OS installations
- **Consistent Behavior**: Identical Qt version across all platforms
- **Professional Distribution**: Self-contained applications ready for deployment
- **Automated Verification**: Build system enforces hermetic packaging

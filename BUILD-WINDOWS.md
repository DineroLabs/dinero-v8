# 🪟 DineroCoin Windows Build Guide

## Quick Start - Build Windows .exe

```bash
# First, fix the CMake cache issue (one time only)
rm -rf build

# Make script executable
chmod +x build-win.sh

# Build Windows binary
./build-win.sh
```

**Output:** `dinerod-win.exe` - Ready to run on Windows!

---

## 📋 What You Get

- **dinerod-win.exe** - Standalone Windows executable (64-bit)
- No DLL dependencies required
- Statically linked for maximum portability
- Works on Windows 7, 8, 10, 11

---

## 🚀 Build Options

### Windows Only
```bash
./build-win.sh
```

### Linux Only
```bash
./build-linux.sh
```

### Both Platforms
```bash
./build-all-platforms.sh all
```

Or use shortcuts:
```bash
./build-all-platforms.sh win     # Windows only
./build-all-platforms.sh linux   # Linux only
```

---

## 🔧 How It Works

The **Dockerfile-win** uses:
- **Ubuntu 22.04** base image
- **MinGW** (Minimalist GNU for Windows) cross-compiler
- Static linking to eliminate DLL dependencies
- CMake toolchain for cross-compilation

**Build Process:**
1. Docker creates a Linux container
2. MinGW cross-compiles your C++ code for Windows
3. Binary is extracted as `dinerod-win.exe`
4. Transfer to Windows and run!

---
## 🐛 Troubleshooting

### CMake Cache Error
**Error:**
```
CMake Error: The current CMakeCache.txt directory is different...
```

**Solution:**
```bash
rm -rf build
```

The `.dockerignore` file prevents this in future builds.

### Docker Issues
```bash
# Clean Docker build cache
docker builder prune -af

# Remove old containers
docker rm $(docker ps -aq) 2>/dev/null
```

### "Permission Denied" on Mac
```bash
chmod +x build-win.sh
chmod +x build-all-platforms.sh
```

---

## ✅ Testing Your Windows Binary

### On Mac (using Wine - if installed)
```bash
wine dinerod-win.exe --version
```

### On Windows
Just double-click `dinerod-win.exe` or run from Command Prompt:
```cmd
dinerod-win.exe --version
```

---

## 📁 Files Created

After running the build script:
- `Dockerfile-win` - Windows cross-compilation configuration
- `build-win.sh` - Windows build script
- `build-all-platforms.sh` - Multi-platform builder
- `.dockerignore` - Prevents CMake cache issues
- `dinerod-win.exe` - Your Windows executable! 🎉

---

## 🎯 Distribution

Your `dinerod-win.exe` is ready to distribute to Windows users. It includes:
- ✅ Static linking (no DLL hell)
- ✅ 64-bit compatibility
- ✅ Works on Windows 7+
- ✅ Single file deployment

Happy building! 🚀

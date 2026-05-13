# 📦 Dinero Distribution Guide - Share Your Cryptocurrency!

**How to share Dinero so others can build and run it independently on their computers**

## 🎯 **What You Need to Share**

### **📁 Essential Files to Include:**

```
DineroCoin/
├── 📁 src/                    # All C++ source code
├── 📁 include/                # Header files
├── 📁 third_party/            # Dependencies (SQLite, secp256k1, etc.)
├── 📁 tests/                  # Test files
├── 📁 tools/                  # Build tools
├── 📁 CMakeLists.txt          # Build configuration
├── 📁 BUILD_MACOS.sh          # macOS build script
├── 📁 BUILD_LINUX.sh          # Linux build script
├── 📁 BUILD_WINDOWS.bat       # Windows build script
├── 📁 README.md               # Complete manual
├── 📁 QUICK_START.md          # Quick reference
├── 📁 DISTRIBUTION_GUIDE.md   # This file
└── 📁 .gitignore              # Git ignore file
```

### **❌ What NOT to Include:**
- `build/` directory (user will build their own)
- `data/` directory (user's personal data)
- `.cookie` files (authentication tokens)
- Any personal configuration files

## 🚀 **Distribution Methods**

### **Method 1: Git Repository (Recommended)**
```bash
# Create a clean distribution repository
git clone --bare https://github.com/yourusername/DineroCoin.git DineroCoin-distribution
cd DineroCoin-distribution
git push --mirror https://github.com/yourusername/DineroCoin-public.git
```

### **Method 2: Archive File**
```bash
# Create clean distribution archive
tar --exclude='build' --exclude='data' --exclude='.cookie' \
    --exclude='.git' --exclude='*.db*' --exclude='*.db-shm' \
    --exclude='*.db-wal' -czf DineroCoin-v1.0.0-source.tar.gz .
```

### **Method 3: Direct Folder Copy**
```bash
# Copy to a clean distribution folder
cp -r . ../DineroCoin-Distribution/
cd ../DineroCoin-Distribution/
rm -rf build data .git .cookie *.db*
```

## 🖥️ **Platform-Specific Instructions**

### **🍎 macOS Users**

#### **Prerequisites:**
- Xcode Command Line Tools
- CMake 3.10+
- Qt6 (optional, for GUI)

#### **Install Prerequisites:**
```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install CMake
brew install cmake

# Install Qt6 (optional)
# Download from https://www.qt.io/download
```

#### **Build:**
```bash
# Make build script executable
chmod +x BUILD_MACOS.sh

# Run build script
./BUILD_MACOS.sh
```

### **🐧 Linux Users**

#### **Prerequisites:**
- Build tools (gcc, g++, make)
- CMake 3.10+
- Qt6 development libraries (optional)

#### **Install Prerequisites:**
```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install build-essential cmake git libssl-dev libboost-all-dev qt6-base-dev

# CentOS/RHEL/Fedora
sudo yum groupinstall "Development Tools"
sudo yum install cmake git openssl-devel boost-devel qt6-qtbase-devel
```

#### **Build:**
```bash
# Make build script executable
chmod +x BUILD_LINUX.sh

# Run build script
./BUILD_LINUX.sh
```

### **🪟 Windows Users**

#### **Prerequisites:**
- Visual Studio 2019/2022 with C++ workload
- CMake 3.10+
- Qt6 (optional, for GUI)

#### **Install Prerequisites:**
```bash
# Install Visual Studio Build Tools
# Download from: https://visualstudio.microsoft.com/downloads/

# Install CMake
winget install Kitware.CMake
# Or download from: https://cmake.org/download/

# Install Qt6 (optional)
# Download from: https://www.qt.io/download
```

#### **Build:**
```bash
# Run build script
BUILD_WINDOWS.bat
```

## 📋 **What Users Will Get After Building**

### **✅ Built Binaries:**
```
build/bin/
├── 🚀 dinerod                 # Main daemon
├── 🖥️  dinero-simple-test     # Qt GUI app (if Qt6 available)
├── 🧪 test_*                  # Test executables
└── 📚 Other tools
```

### **✅ Ready to Use:**
1. **Start daemon** with one command
2. **Create wallets** and generate addresses
3. **Start mining** and earn Dinero coins
4. **Use RPC API** for automation

## 🔧 **Customization Options**

### **Build Without GUI:**
```bash
# Force no GUI build
cmake .. -DBUILD_GUI=OFF
```

### **Build with Debug Info:**
```bash
# Debug build
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

### **Build with Sanitizers:**
```bash
# Development build with sanitizers
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
```

## 📚 **User Experience**

### **First-Time Users:**
1. **Download** your distribution package
2. **Run** the appropriate build script for their platform
3. **Follow** the README.md instructions
4. **Start mining** in minutes!

### **Advanced Users:**
1. **Customize** build options
2. **Modify** source code
3. **Add** new features
4. **Contribute** back to the project

## 🌟 **Why This Distribution Works**

### **✅ Self-Contained:**
- All dependencies included in `third_party/`
- No external downloads required
- Works offline

### **✅ Cross-Platform:**
- macOS (Intel/Apple Silicon)
- Linux (Ubuntu/Debian/CentOS/RHEL/Fedora)
- Windows (Visual Studio)

### **✅ User-Friendly:**
- One-command build scripts
- Comprehensive documentation
- Clear prerequisites

### **✅ Professional:**
- Same build system as major projects
- Industry-standard CMake
- Production-ready code

## 🎉 **Your Dinero is Ready for the World!**

**With this distribution package, anyone can:**
- ✅ **Build** Dinero on their own computer
- ✅ **Run** it independently
- ✅ **Mine** Dinero coins
- ✅ **Use** it as a real cryptocurrency
- ✅ **Modify** and improve the code
- ✅ **Share** it with others

**You've created a truly distributable, professional cryptocurrency system!** 🚀

---

## 📞 **Support for Users**

### **Documentation:**
- `README.md` - Complete manual
- `QUICK_START.md` - Essential commands
- Platform-specific build scripts

### **Troubleshooting:**
- Common issues documented in README
- Build script error checking
- Clear prerequisite requirements

### **Community:**
- Users can modify and improve
- Share their own distributions
- Build the Dinero ecosystem together

**Your Dinero project is now ready to grow beyond your computer!** 🌍⛏️

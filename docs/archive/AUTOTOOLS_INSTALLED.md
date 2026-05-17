# ✅ Autotools Installation - Complete

**Date:** October 7, 2025  
**Status:** 🟢 **INSTALLED**

---

## 📦 Installed Packages

| Tool | Version | Location | Purpose |
|------|---------|----------|---------|
| **autoconf** | 2.72 | `/opt/homebrew/bin/autoconf` | Generate configure scripts |
| **automake** | 1.18.1 | `/opt/homebrew/bin/automake` | Generate Makefile.in |
| **libtool** | 2.5.4 | `/opt/homebrew/bin/glibtool` | Build shared libraries |
| **autoreconf** | 2.72 | `/opt/homebrew/bin/autoreconf` | Update configure scripts |

---

## 🔧 Common Usage

### **1. Bootstrap a Project**
```bash
# If project has autogen.sh:
./autogen.sh

# Or manually:
autoreconf -fi
```

### **2. Configure and Build**
```bash
./configure
make
make install
```

### **3. For secp256k1 (typical)**
```bash
cd vendor/secp256k1
./autogen.sh
./configure --enable-module-recovery
make
sudo make install
```

---

## 📝 Note: macOS Specifics

On macOS, GNU libtool is installed as **`glibtool`** to avoid conflict with Apple's `/usr/bin/libtool`.

- **GNU libtool:** `/opt/homebrew/bin/glibtool` ✅ (for building libraries)
- **Apple libtool:** `/usr/bin/libtool` (for linking)

Most `autoreconf` scripts will automatically detect and use `glibtool` on macOS.

---

## ✅ Verification

```bash
# Check all tools are available:
which autoconf automake autoreconf glibtool

# Check versions:
autoconf --version
automake --version
glibtool --version
autoreconf --version
```

---

## 🎯 What's Next?

Now you can build projects that use GNU Autotools, such as:
- secp256k1
- libsodium
- OpenSSL (older versions)
- Many other C/C++ libraries

**Ready to use!** 🚀

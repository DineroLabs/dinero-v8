# Cross-Platform Repository Restructure Plan

**Date**: October 1, 2025  
**Status**: 🚧 **IMPLEMENTING CROSS-PLATFORM STRUCTURE**

---

## 🎯 **Goal**

Transform the current DineroCoin repository into a clean, cross-platform codebase that builds into three different binaries (Linux/macOS/Windows) from the same source, with clear separation between core logic and platform-specific code.

---

## 📁 **New Repository Structure**

```
/CMakeLists.txt
/cmake/               # cmake modules & toolchain files
/docs/                # BUILDING.md, CONTRIBUTING.md
/include/
  dinero/             # public headers (STL-only)
    core/             # consensus, wallet, rpc interfaces
    platform/         # cross-platform interfaces (headers only)
    compat/           # adapters (json, endian, filesystem quirks)

/src/
  core/               # STL-only core (builds into libdinero_core)
  platform/           # per-OS implementations
    posix/
    windows/
    apple/
  daemon/             # dinerod sources (NO Qt)
  gui/                # Qt UI only (separate target)

/third_party/         # vendored libs (if any)
/tests/               # unit/integration tests
/scripts/             # build and audit scripts
```

---

## 🔧 **Implementation Steps**

### **Phase 1: Core Structure** ✅
- [x] Create new directory structure
- [x] Move existing files to appropriate locations
- [x] Separate core logic from platform-specific code

### **Phase 2: Platform Abstraction** 🚧
- [ ] Create platform interface headers
- [ ] Implement POSIX, Windows, and Apple-specific code
- [ ] Add network, filesystem, and system abstraction layers

### **Phase 3: Build System** 🚧
- [ ] Update CMakeLists.txt for cross-platform builds
- [ ] Add platform-specific source selection
- [ ] Create feature flags (DIN_WITH_ROCKSDB, DIN_BUILD_GUI, etc.)

### **Phase 4: Code Quality** 🚧
- [ ] Add DIN_TODO macro system
- [ ] Create Qt-free core audit script
- [ ] Implement JSON adapter layer
- [ ] Add header hygiene rules

### **Phase 5: CI/CD** 🚧
- [ ] Create GitHub Actions CI matrix
- [ ] Add cross-platform build testing
- [ ] Implement automated audits

---

## 🎯 **Key Principles**

1. **One repo, one CMake project, no per-OS forks**
2. **Three outputs**: dinerod (Linux), dinerod.exe (Windows), dinerod universal (macOS)
3. **Core code is Qt-free and STL-only**
4. **GUI uses Qt but lives in a separate target**
5. **OS-specific code is isolated behind small interfaces**
6. **No file globs in CMake - explicit source lists**
7. **Clear placeholder/stub policy with compile-time guards**

---

## 📋 **Success Criteria**

- [ ] Builds cleanly on Linux, macOS, and Windows
- [ ] Core code is completely Qt-free
- [ ] Platform-specific code is properly abstracted
- [ ] All placeholders use DIN_TODO macro system
- [ ] CI matrix tests all three platforms
- [ ] Clear documentation for contributors

---

**Next Steps**: Begin implementing the new structure while maintaining current functionality.

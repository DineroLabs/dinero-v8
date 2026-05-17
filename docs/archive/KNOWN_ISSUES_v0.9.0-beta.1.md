# 🚨 Known Issues - Dinero Desktop v0.9.0-beta.1

**Last Updated**: September 18, 2025  
**Build**: `2fe7f62bc503`

---

## ⚠️ **Critical Issues**

### **1. Mainnet Mining Disabled**
- **Issue**: Mainnet mining is intentionally disabled in this beta
- **Impact**: Cannot mine real DineroCoin on production network
- **Workaround**: Use regtest or testnet for mining tests
- **Status**: Intentional safety measure for beta
- **ETA**: Will be enabled in Release Candidate

### **2. Network Switching Requires Restart**
- **Issue**: Switching between regtest/testnet/mainnet requires manual daemon restart
- **Impact**: Cannot seamlessly switch networks from GUI
- **Workaround**: Stop daemon, change network, restart daemon manually
- **Status**: Known limitation, fix planned
- **ETA**: Beta.2

---

## 🐛 **GUI Issues**

### **3. Stub Method Messages**
- **Issue**: Some GUI buttons show "not yet implemented" messages
- **Affected**: Import Key, Network Switch, some menu items
- **Impact**: Cosmetic only - core functionality works
- **Workaround**: Use available features (wallet, mining, explorer)
- **Status**: Low priority polish items
- **ETA**: Beta.2

### **4. GUI Launch on Some Systems**
- **Issue**: GUI may not launch on some macOS configurations
- **Symptoms**: App appears to start but no window shows
- **Workaround**: Launch from command line to see error messages:
  ```bash
  /path/to/dinero-desktop.app/Contents/MacOS/dinero-desktop
  ```
- **Status**: Under investigation
- **ETA**: Hotfix if widespread

### **5. Qt AutoMoc Warnings**
- **Issue**: Build shows AutoMoc warnings about missing Q_OBJECT macros
- **Impact**: Cosmetic build warnings only
- **Workaround**: Warnings can be ignored
- **Status**: Low priority cleanup
- **ETA**: Beta.2

---

## ⛏️ **Mining Issues**

### **6. Mining Address Required**
- **Issue**: Must generate wallet address before starting mining
- **Impact**: Cannot start mining immediately after fresh install
- **Workaround**: Go to Wallet tab → "Generate New Address" first
- **Status**: By design, but could be automated
- **ETA**: Beta.2 (auto-generate if none exists)

### **7. Thread Count Discrepancy**
- **Issue**: Mining may show different thread counts in start vs status
- **Example**: Start with 12 threads, status shows 1 thread
- **Impact**: Cosmetic only - mining still works correctly
- **Workaround**: Check actual CPU usage to verify mining activity
- **Status**: Display bug in status reporting
- **ETA**: Beta.2

### **8. Scientific Notation in Difficulty**
- **Issue**: `getdifficulty` RPC returns scientific notation (e.g., `2.3e-10`)
- **Impact**: Smoke tests fail, but functionality works
- **Workaround**: Parse scientific notation or use `getblockchaininfo`
- **Status**: Cosmetic RPC formatting issue
- **ETA**: Beta.2

---

## 🔗 **RPC/API Issues**

### **9. RPC Parameter Format Confusion**
- **Issue**: Some RPC methods expect array parameters, others expect objects
- **Example**: `mining.start` needs `[{...}]` not `{...}`
- **Impact**: Manual RPC calls may fail with wrong format
- **Workaround**: Check examples in release notes for correct format
- **Status**: API consistency improvement needed
- **ETA**: Beta.2

### **10. Port Detection Issues**
- **Issue**: Daemon may bind to different ports than expected
- **Symptoms**: RPC calls fail with connection errors
- **Workaround**: Check `nodeinfo.json` for actual ports:
  ```bash
  cat test-mining/nodeinfo.json | grep '"port"'
  ```
- **Status**: Unified port configuration needed
- **ETA**: Beta.2

---

## 💾 **Database/Storage Issues**

### **11. Database Lock on Unclean Shutdown**
- **Issue**: SQLite databases may remain locked after daemon crash
- **Symptoms**: "Database locked" errors on restart
- **Workaround**: 
  ```bash
  rm test-mining/regtest/*.db-wal
  rm test-mining/regtest/*.db-shm
  ```
- **Status**: Need better shutdown handling
- **ETA**: Beta.2

### **12. Large Log Files**
- **Issue**: Daemon logs can grow large over time
- **Impact**: Disk space usage
- **Workaround**: Manually rotate logs or use `-printtoconsole=0`
- **Status**: Log rotation needed
- **ETA**: Beta.2

---

## 🖥️ **macOS Specific Issues**

### **13. Gatekeeper Security Warning**
- **Issue**: macOS shows security warning for unsigned app
- **Symptoms**: "Cannot open because developer cannot be verified"
- **Workaround**: Right-click → "Open" → "Open" (confirm twice)
- **Status**: Need code signing certificate
- **ETA**: Release Candidate (signed builds)

### **14. Deprecated Security APIs**
- **Issue**: Build warnings about deprecated Keychain APIs
- **Impact**: Warnings only, functionality works
- **Workaround**: Warnings can be ignored
- **Status**: Need to migrate to modern Security framework
- **ETA**: v1.0

### **15. Battery/Thermal Monitoring**
- **Issue**: Mining safety may not detect all thermal conditions
- **Impact**: Possible system stress on intensive mining
- **Workaround**: Monitor system manually, use throttling
- **Status**: Enhanced monitoring planned
- **ETA**: Beta.2

---

## 🔧 **Build/Development Issues**

### **16. CMake REPRO Flag Ignored**
- **Issue**: `-DREPRO=ON` flag shows as unused
- **Impact**: Reproducible builds not fully implemented
- **Workaround**: Standard builds are deterministic enough for beta
- **Status**: Reproducible build system needed
- **ETA**: Release Candidate

### **17. Duplicate Library Warnings**
- **Issue**: Linker warnings about duplicate secp256k1 libraries
- **Impact**: Warnings only, builds work correctly
- **Workaround**: Warnings can be ignored
- **Status**: CMake cleanup needed
- **ETA**: Beta.2

### **18. Missing Test Targets**
- **Issue**: Some test executables fail to build
- **Example**: `test_developer_fund` has undefined symbols
- **Impact**: Affects developers only
- **Workaround**: Skip problematic tests, focus on main targets
- **Status**: Test suite cleanup needed
- **ETA**: Beta.2

---

## 📱 **Platform Support**

### **19. Windows/Linux Builds Missing**
- **Issue**: Only macOS build available in this beta
- **Impact**: Cannot test on other platforms
- **Workaround**: Use macOS for beta testing
- **Status**: Cross-platform builds planned
- **ETA**: Beta.2

### **20. Qt Version Dependencies**
- **Issue**: Requires specific Qt 6.9.1 version
- **Impact**: May not work with other Qt versions
- **Workaround**: Use exact Qt version specified
- **Status**: Version flexibility needed
- **ETA**: Beta.2

---

## 🔄 **Workaround Summary**

### **Quick Fixes for Common Issues**

1. **GUI Won't Launch**: Try command line launch to see errors
2. **Mining Won't Start**: Generate wallet address first
3. **RPC Connection Failed**: Check actual port in `nodeinfo.json`
4. **Database Locked**: Remove `.db-wal` and `.db-shm` files
5. **Gatekeeper Warning**: Right-click → Open → Open
6. **Scientific Notation**: Parse as number or ignore formatting

### **Best Practices for Beta Testing**

1. **Use Regtest**: Safest for mining experiments
2. **Monitor Resources**: Watch CPU/memory usage during mining
3. **Save Logs**: Keep daemon output for bug reports
4. **Backup Data**: Copy important wallet data before testing
5. **Report Issues**: Use GitHub issues with detailed information

---

## 📞 **Getting Help**

### **If You Encounter Issues**

1. **Check This List**: Your issue might be already known
2. **Search GitHub**: Look for existing issue reports
3. **Gather Information**: 
   - macOS version and hardware
   - Exact error messages
   - Steps to reproduce
   - Log files (if available)
4. **Create Issue**: Use GitHub with "Beta 0.9.0-beta.1" label

### **Emergency Workarounds**

- **Complete Reset**: Delete data directory and restart fresh
- **Safe Mode**: Use regtest only, avoid mainnet/testnet
- **Manual Mining**: Use direct RPC calls instead of GUI
- **Fallback**: Use previous stable version if critical

---

## 🎯 **Issue Priority**

### **P0 (Critical - Hotfix Candidates)**
- GUI launch failures (if widespread)
- Mining safety bypasses
- Data corruption issues
- Security vulnerabilities

### **P1 (High - Beta.2)**
- Network switching
- RPC API consistency  
- Enhanced mining safety
- Cross-platform builds

### **P2 (Medium - Release Candidate)**
- Code signing
- Reproducible builds
- Performance optimizations
- Advanced features

### **P3 (Low - v1.0)**
- UI polish
- API deprecation fixes
- Documentation improvements
- Nice-to-have features

---

**Remember**: This is beta software. While the core mining functionality is production-ready, expect some rough edges in the user experience. Your testing and feedback help make the final release better! 🚀

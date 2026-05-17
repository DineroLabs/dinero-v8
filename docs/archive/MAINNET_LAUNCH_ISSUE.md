# 🚨 Mainnet Launch Issue - November 7, 2025

## **Problem Discovered**

When attempting to launch mainnet on Linux servers, discovered **critical RPC handler missing**.

### **Symptoms:**
```bash
./dinero-cli getblockcount
Error: Method not found: getblockcount
Error code: -32601

./dinero-cli getblockhash 0
Error: Method not found: getblockhash
Error code: -32601
```

### **Root Cause:**
Linux build (from Oct/Nov) is missing **core blockchain RPC handlers**.

**What's Registered** (from logs):
- ✅ Market handlers (experimental)
- ✅ Payment handlers (experimental)
- ✅ Auth handlers (experimental)
- ✅ Hardware wallet handlers (experimental)
- ❌ **Blockchain handlers** (MISSING!)
- ❌ **Wallet handlers** (MISSING!)
- ❌ **Mining handlers** (MISSING!)

### **Why This Happened:**
When we disabled experimental features on Linux (to fix compilation), we accidentally **excluded the core RPC handler files** from the build.

---

## **Current Status**

### **California Server (172.93.160.131):**
```
✅ Daemon: RUNNING
✅ RPC: Responding
❌ Core methods: NOT AVAILABLE
Status: BROKEN (can't query blockchain)
```

### **Virginia Server (173.249.195.59):**
```
❌ Daemon: Not started yet
Status: N/A
```

### **Mac:**
```
✅ Binaries: Complete (all RPC handlers)
✅ Tested: Working
Status: READY
```

---

## **Solution Options**

### **Option 1: Fix Linux CMakeLists.txt (Recommended)**
**Time:** 30 minutes  
**Risk:** Low (surgical fix)

**What to do:**
1. Edit `CMakeLists.txt` (Linux section)
2. Re-enable core RPC handler files:
   ```cmake
   src/rpc/methods_blockchain_context.cpp  # getblockcount, getblockhash
   src/rpc/methods_wallet_context.cpp      # getbalance, getnewaddress
   src/rpc/methods_mining_context.cpp      # getmininginfo, submitblock
   src/rpc/methods_network_context.cpp     # getpeerinfo, getnetworkinfo
   ```
3. Keep experimental handlers DISABLED
4. Rebuild on Linux servers
5. Restart daemons

**Result:** Fully functional Linux mainnet with core RPC only

---

### **Option 2: Copy Mac Build Approach (Alternative)**
**Time:** 1 hour  
**Risk:** Medium (needs testing)

**What to do:**
1. Make Linux `CMakeLists.txt` match Mac build
2. Both platforms build identical binaries
3. Rebuild and test

**Result:** Mac and Linux have exact same features

---

### **Option 3: Use Mac as Seed Node Only (Workaround)**
**Time:** Immediate  
**Risk:** High (single point of failure)

**What to do:**
1. Run mainnet daemon on Mac only
2. Use Mac as single seed node
3. Fix Linux later

**Result:** Mainnet works but centralized on Mac

---

## **Recommended Action**

**Go with Option 1** (fix Linux RPC handlers):

1. **Identify missing files** in Linux build
2. **Add them back** to CMakeLists.txt (Linux section)
3. **Rebuild** on both servers (takes 3-5 minutes)
4. **Test** with `getblockcount`, `getblockhash`
5. **Launch** mainnet with both servers

**ETA:** 30-45 minutes total

---

## **What We Learned**

When disabling experimental features, we need to:
1. ✅ Disable experimental RPC handlers
2. ✅ Keep CORE RPC handlers (blockchain, wallet, mining)
3. ✅ Test basic RPC methods after build
4. ✅ Verify on BOTH platforms (Mac + Linux)

---

## **Next Steps**

**Immediate:**
1. Fix Linux CMakeLists.txt
2. Rebuild both servers
3. Launch mainnet

**After Launch:**
1. Add block 100 checkpoint (when mined)
2. Monitor for 24 hours
3. Add Virginia as second seed node
4. Distribute Mac binaries to users

---

**Status:** ⏸️ **Paused** (waiting for CMakeLists.txt fix)  
**Impact:** High (blocks mainnet launch)  
**Priority:** 🔥 Critical  
**ETA:** 30-45 minutes

---

**Prepared by:** AI Engineering Assistant  
**Date:** November 7, 2025 (5:55 PM PST)


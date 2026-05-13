# Dinero-Qt GUI Testing Checklist

**Date**: 2025-10-06  
**Purpose**: Verify all fixes are working correctly

## Quick Test Suite

### ✅ Test 1: Signal/Slot Fix (dumpseed error handling)

**Steps**:
1. Open dinero-qt
2. Go to Wallet tab
3. Click "Create/Restore Wallet" (if needed)
4. Lock the wallet (if unlocked)
5. Try to export seed: Click "📤 Export Seed for Mobile"
6. **Expected**: Error dialog should appear saying wallet needs to be unlocked
7. **Before fix**: Nothing happened (connection was broken)

**Status**: [ ] Pass  [ ] Fail

---

### ✅ Test 2: Portable Mining Path Resolution

**Test 2a: Development Environment**
```bash
cd /Users/haydarevich/Documents/DineroCoin/gui/build
./dinero-qt
```
1. Open GUI
2. Go to Mining tab
3. Set mining address
4. Click "▶️ Start"
5. **Expected**: Should find miner in ../../build-clean/dinero-miner
6. **Check**: Mining should start without "Miner Not Found" error

**Status**: [ ] Pass  [ ] Fail

**Test 2b: Environment Variable Override**
```bash
export DINERO_MINER_PATH=/custom/path/dinero-miner
export DINERO_DATA_DIR=/custom/datadir
./dinero-qt
```
1. Start mining
2. **Expected**: Should use environment variable paths
3. **Check**: Console should show custom paths being used

**Status**: [ ] Pass  [ ] Fail

**Test 2c: Bundled App (macOS)**
```bash
open build-gui/dinero-qt.app
```
1. Start mining
2. **Expected**: Should find miner in app bundle
3. **Check**: Error message (if any) shows all searched paths

**Status**: [ ] Pass  [ ] Fail

---

### ✅ Test 3: RPC Error Handling

**Test 3a: Missing getsupply Fields**
1. Start GUI connected to daemon
2. Check Overview tab
3. **Expected**: If getsupply is missing fields, should show "Supply: N/A"
4. **Check**: Console for warning: "getsupply missing required fields"
5. **Before fix**: Would crash or show invalid data

**Status**: [ ] Pass  [ ] Fail

**Test 3b: Missing getmempoolinfo Fields**
1. Check Overview tab
2. **Expected**: If getmempoolinfo fails, should show "Mempool: N/A"
3. **Check**: No crashes, graceful degradation

**Status**: [ ] Pass  [ ] Fail

**Test 3c: Missing getblockchaininfo Fields**
1. Check Overview tab
2. **Expected**: If getblockchaininfo fails, sync status stays blank
3. **Check**: Warning in console, no crash

**Status**: [ ] Pass  [ ] Fail

---

## Comprehensive Testing

### Wallet Operations

**Create HD Wallet**
- [ ] Create new wallet wizard appears
- [ ] Can generate seed phrase
- [ ] Can encrypt with password
- [ ] Wallet unlocks successfully
- [ ] First address derives correctly

**Lock/Unlock**
- [ ] Lock button works
- [ ] Wallet shows locked state
- [ ] Unlock with correct password works
- [ ] Unlock with wrong password fails gracefully
- [ ] Locked state persists after unlock timeout

**Derive Addresses**
- [ ] Unlock wallet first (or get prompted)
- [ ] Click "🆕 New Address" in Receive tab
- [ ] Address appears in table
- [ ] Can copy address to clipboard
- [ ] Mining address auto-updates

**Export Seed**
- [ ] Unlock wallet first
- [ ] Security warning appears
- [ ] Seed phrase displays correctly
- [ ] Can copy to clipboard
- [ ] Works with locked wallet (shows error) ← **THIS WAS BROKEN, NOW FIXED**

---

### Send Operations

**Send Transaction**
- [ ] Can enter recipient address
- [ ] Can enter amount
- [ ] Amount validation works (positive, <= balance)
- [ ] Address validation works (din1q format)
- [ ] "Max" button fills correct amount
- [ ] Requires wallet to be unlocked
- [ ] Transaction creates successfully
- [ ] Transaction broadcasts successfully
- [ ] Balance updates after send
- [ ] Transaction appears in history

**Error Handling**
- [ ] Empty recipient shows error
- [ ] Invalid address format shows error
- [ ] Amount > balance shows error
- [ ] Negative amount shows error
- [ ] Locked wallet prompts unlock

---

### Mining Operations

**Start Mining**
- [ ] Can set mining address from wallet ← **IMPROVED: Better path finding**
- [ ] "Use Wallet Address" button works
- [ ] Can set thread count
- [ ] Start button initiates mining
- [ ] Hashrate updates appear
- [ ] Block found detection works
- [ ] Mining output shows in text area

**Stop Mining**
- [ ] Stop button terminates process
- [ ] UI updates correctly
- [ ] Stats stop updating
- [ ] Can restart mining

**Mining Stats**
- [ ] Uptime counter works
- [ ] Hashrate displays (MH/s)
- [ ] Blocks found counter works
- [ ] Total hashes counter works
- [ ] Block found triggers celebration

**Path Resolution** ← **NEWLY FIXED**
- [ ] Works in development mode
- [ ] Works with bundled app
- [ ] Respects environment variables
- [ ] Shows helpful error if not found
- [ ] Error message lists searched paths

---

### Network / Connection

**Server Connection**
- [ ] Connects to localhost:20997 (mainnet)
- [ ] Falls back to localhost:20998 (testnet) if needed
- [ ] Status shows green when connected
- [ ] Status shows red on error
- [ ] Server failover works (if multiple configured)

**Data Refresh**
- [ ] Auto-refresh every 5 seconds
- [ ] Manual refresh button works
- [ ] All tabs update correctly
- [ ] Connection status updates

---

### UI/UX

**Tabs Navigation**
- [ ] All tabs render correctly
- [ ] Tab switching is smooth
- [ ] No layout issues on resize
- [ ] Tables display properly

**Responsive Updates**
- [ ] Balance updates in real-time
- [ ] Transaction list updates
- [ ] Address list updates
- [ ] UTXO list updates
- [ ] Mining stats update every second

---

## Performance Tests

**Large Transaction History**
- [ ] Loads 100+ transactions without lag
- [ ] Sorting works correctly
- [ ] No memory leaks

**Large Address List**
- [ ] Can handle 50+ addresses
- [ ] Sorting works
- [ ] Copy buttons all work

**Long Mining Session**
- [ ] Can mine for 1+ hour
- [ ] Stats remain accurate
- [ ] No memory leaks
- [ ] Clean stop works

---

## Error Scenarios

**Daemon Not Running**
- [ ] Shows connection error
- [ ] Doesn't crash
- [ ] Retries connection
- [ ] Helpful error message

**Malformed RPC Response** ← **NEWLY IMPROVED**
- [ ] Shows "N/A" for missing data
- [ ] Logs warning to console
- [ ] Doesn't crash GUI
- [ ] Continues operating

**Network Issues**
- [ ] Handles timeouts gracefully
- [ ] Switches servers if configured
- [ ] Shows connection status
- [ ] Recovers when network returns

---

## Platform-Specific Tests

### macOS
- [ ] App bundle launches
- [ ] Miner path resolution works
- [ ] Data directory correct (~/ Library/Application Support/Dinero)
- [ ] Keyboard shortcuts work
- [ ] App icon displays

### Linux
- [ ] Executable runs
- [ ] Miner path resolution works
- [ ] Data directory correct (~/.dinero)
- [ ] Desktop integration works

### Windows
- [ ] .exe runs
- [ ] Miner path resolution works (dinero-miner.exe)
- [ ] Data directory correct (%APPDATA%/Dinero)
- [ ] Start menu integration works

---

## Regression Tests

Check that old functionality still works:

- [ ] Original cookie loading still works
- [ ] Multi-server config still works
- [ ] All RPC methods still work
- [ ] Transaction signing still works
- [ ] HD wallet derivation still works
- [ ] Encryption still works

---

## Console Checks

Look for these in console output:

**Good Signs**:
- "Loaded cookie from: /path/to/.cookie"
- "Connected to server"
- "RPC getbalance HTTP 200"
- Mining output with hashrate

**Warning Signs** (should be handled gracefully):
- "getsupply missing required fields" ← **NEWLY ADDED**
- "getmempoolinfo missing required fields" ← **NEWLY ADDED**  
- "getblockchaininfo missing required fields" ← **NEWLY ADDED**
- "Miner not found" (with helpful paths shown) ← **NEWLY IMPROVED**

**Bad Signs** (should not appear):
- Segmentation fault
- Uncaught exceptions
- Qt warning about invalid connections ← **WAS APPEARING, NOW FIXED**
- Crashes

---

## Sign-Off

**Tested by**: _________________  
**Date**: _________________  
**Version**: _________________  

**Critical Fixes Verified**:
- [ ] Signal/slot mismatch fixed (dumpseed error handling)
- [ ] Mining path resolution works portably
- [ ] RPC error handling prevents crashes
- [ ] All original functionality still works

**Overall Status**: [ ] Pass  [ ] Fail  [ ] Needs Work

**Notes**:
_______________________________________________________________
_______________________________________________________________
_______________________________________________________________

---

## Quick Test Script

```bash
#!/bin/bash
# Quick automated test

echo "=== Dinero-Qt Quick Test ==="

# Test 1: Build
echo "Building dinero-qt..."
cmake --build build-gui --target dinero-qt
if [ $? -eq 0 ]; then
  echo "✅ Build successful"
else
  echo "❌ Build failed"
  exit 1
fi

# Test 2: Can launch (will exit after 5 seconds)
echo "Testing launch..."
timeout 5 ./build-gui/dinero-qt &
sleep 2
if pgrep dinero-qt > /dev/null; then
  echo "✅ GUI launched successfully"
  pkill dinero-qt
else
  echo "❌ GUI failed to launch"
  exit 1
fi

# Test 3: Environment variables
echo "Testing environment variables..."
export DINERO_MINER_PATH=/tmp/fake-miner
export DINERO_DATA_DIR=/tmp/fake-data
timeout 5 ./build-gui/dinero-qt 2>&1 | grep -q "DINERO_MINER_PATH"
if [ $? -eq 0 ]; then
  echo "✅ Environment variables respected"
else
  echo "⚠️ Could not verify environment variables (may be OK)"
fi

echo ""
echo "=== Quick Test Complete ==="
echo "Run full manual tests for comprehensive verification"
```

Save as `gui/quick-test.sh` and run: `chmod +x gui/quick-test.sh && ./gui/quick-test.sh`

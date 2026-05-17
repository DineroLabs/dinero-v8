# REGTEST SAFEGUARDS - Comprehensive Protection System

**Last Updated:** October 16, 2025
**Purpose:** Prevent accidental mainnet/regtest data mixup

---

## 🛡️ Multi-Layer Protection System

Our regtest implementation includes **7 layers of protection** to prevent any possibility of mixing regtest and mainnet data:

---

## Layer 1: Different Network Magic Bytes

**Location:** `src/consensus/chainparams_impl.cpp:111`

```cpp
// MAINNET
.magic = 0xd9b4bef9

// REGTEST
.magic = 0xfabfb5da
```

**Protection:** P2P nodes with different magic bytes **cannot connect** to each other. Attempting to connect a regtest node to mainnet will be immediately rejected at the protocol level.

---

## Layer 2: Different Port Numbers

**Location:** `src/consensus/chainparams_impl.cpp:112-115`

| Port Type | Mainnet | Regtest | Protection |
|-----------|---------|---------|------------|
| RPC       | 20997   | 20996   | Cannot accidentally connect CLI to wrong network |
| P2P       | 20999   | 21001   | Cannot accidentally peer with wrong network |
| HTTP      | 8080    | 18880   | Separate web interfaces |
| WebSocket | 8081    | 18881   | Separate WS connections |

**Protection:** Even if you forget `--regtest` flag, different ports prevent cross-network communication.

---

## Layer 3: Different Genesis Block

**Location:** `src/consensus/chainparams_impl.cpp:129-144`

### Mainnet Genesis:
```
Time:     1760472333 (Oct 14, 2025)
Bits:     0x1d3fffff
Hash:     f01568ded692203556e0cc8a6c14a2bf3eea141edef2200bc7670276c78d52aa
Message:  "Dinero: Real Money for Free People - Genesis Block 2025"
```

### Regtest Genesis:
```
Time:     1000000000 (Sep 8, 2001 - clearly test data!)
Bits:     0x207fffff (much easier - instant mining)
Hash:     0000000000000000000000000000000000000000000000000000000000000001
Message:  "Regtest Genesis - DO NOT USE ON MAINNET"
```

**Protection:** Completely different genesis blocks make it **cryptographically impossible** to accept regtest blocks on mainnet or vice versa.

---

## Layer 4: Network Marker File (Runtime Protection)

**Location:** `src/daemon/main.cpp:643-680`

### How It Works:

1. **First startup** in a data directory:
   ```
   → Creates `.network` file containing "mainnet" or "regtest"
   → Logs: "📝 Created network marker: mainnet"
   ```

2. **Subsequent startups**:
   ```
   → Reads `.network` file
   → Compares with current network selection
   → If mismatch: FATAL ERROR and exit
   ```

3. **Protection Example:**
   ```bash
   # First run mainnet
   ./dinerod --datadir=/data/chain
   # Creates /data/chain/.network with "mainnet"

   # Try to run regtest on same datadir
   ./dinerod --regtest --datadir=/data/chain
   # ❌ FATAL ERROR: NETWORK MISMATCH DETECTED
   #    Data contains: mainnet
   #    Trying to run: regtest
   #    EXIT CODE: 1
   ```

**Protection:** **Impossible** to accidentally mix networks in the same data directory, even if you:
- Forget to pass `--regtest` flag
- Use wrong config file
- Copy-paste wrong command

---

## Layer 5: Different Address Prefixes

**Location:** `src/consensus/chainparams_impl.cpp:110,124`

| Network | HRP (Bech32) | Pubkey Prefix | Example Address |
|---------|-------------|---------------|-----------------|
| Mainnet | `din`       | 0x00          | `din1q...`     |
| Regtest | `rdin`      | 0x6f          | `rdin1q...` or `m...` |

**Protection:** Addresses are visually different. Cannot accidentally send mainnet coins to regtest address (invalid format).

---

## Layer 6: Startup Warning Banner (Human Protection)

**Location:** `src/daemon/main.cpp:617-629`

When starting in regtest mode, you see:

```
╔════════════════════════════════════════════════════════════╗
║  ⚠️  REGTEST MODE - LOCAL TESTING ONLY  ⚠️               ║
╠════════════════════════════════════════════════════════════╣
║  • Network: REGTEST (isolated from mainnet/testnet)       ║
║  • Magic: 0xfabfb5da (≠ mainnet 0xd9b4bef9)              ║
║  • Ports: RPC=20996, P2P=21001 (≠ mainnet)               ║
║  • Genesis: Regtest-only (≠ mainnet genesis)             ║
║  • ASERT: Enabled from block 1 for testing                ║
║                                                            ║
║  ⚠️  WARNING: DO NOT USE REGTEST FOR PRODUCTION!  ⚠️     ║
╚════════════════════════════════════════════════════════════╝
```

**Protection:** Clear visual indication that you're running regtest. Impossible to miss.

---

## Layer 7: Separate Data Directories (Best Practice)

### Recommended Setup:

```bash
# Mainnet (production)
./dinerod --datadir=~/.dinero

# Regtest (testing)
./dinerod --regtest --datadir=/tmp/regtest-test
```

### Why This Matters:

Even with all the safeguards above, keeping data directories **physically separate** adds an extra layer of safety:

- ✅ No chance of accidental file corruption
- ✅ Easy to delete regtest data without fear
- ✅ Clean separation of concerns
- ✅ Can run both simultaneously (different ports)

---

## 🧪 Testing the Safeguards

We provide a comprehensive test script: `TEST_REGTEST_ASERT.sh`

### What It Tests:

1. ✅ Regtest daemon starts correctly
2. ✅ `.network` marker file is created
3. ✅ Network mismatch detection works (tries to start mainnet with regtest datadir)
4. ✅ ASERT difficulty adjustment enabled from block 1
5. ✅ All parameters are regtest-specific

### Run the Test:

```bash
cd /Users/haydarevich/Documents/DineroCoin
./TEST_REGTEST_ASERT.sh
```

Expected output:
```
╔════════════════════════════════════════════════════════════╗
║  REGTEST ASERT TESTING - Dinero Blockchain                 ║
╚════════════════════════════════════════════════════════════╝

═══════════════════════════════════════════════════════════
TEST 1: Start regtest daemon with ASERT
═══════════════════════════════════════════════════════════
✅ Started regtest daemon (PID: 12345)
✅ Daemon running successfully

═══════════════════════════════════════════════════════════
TEST 2: Verify network isolation (check .network marker)
═══════════════════════════════════════════════════════════
✅ Network marker exists: regtest

═══════════════════════════════════════════════════════════
TEST 3: Try to start mainnet with same datadir (should FAIL)
═══════════════════════════════════════════════════════════
✅ Network mismatch correctly detected!
✅ Safeguard working: Cannot mix mainnet with regtest data

═══════════════════════════════════════════════════════════
ALL TESTS PASSED! ✅
═══════════════════════════════════════════════════════════
```

---

## 🔒 Failure Modes - What Happens If...

### Q: What if I forget `--regtest` flag with regtest datadir?
**A:** Daemon exits with **NETWORK MISMATCH** error. Cannot start.

### Q: What if I use regtest RPC port with mainnet daemon?
**A:** Connection refused (port not listening). No data corruption.

### Q: What if I try to import regtest blocks into mainnet?
**A:** Rejected immediately - different genesis hash. Chain incompatible.

### Q: What if I use same wallet seed on both networks?
**A:** Addresses will have different prefixes (`din` vs `rdin`). Transactions invalid on wrong network.

### Q: What if someone sends me regtest blocks over P2P?
**A:** Rejected at protocol level - different magic bytes. Connection dropped.

### Q: Can I run both mainnet and regtest simultaneously?
**A:** Yes! Different ports allow both to run at the same time:
```bash
# Terminal 1: Mainnet
./dinerod --datadir=~/.dinero

# Terminal 2: Regtest
./dinerod --regtest --datadir=/tmp/regtest
```

---

## 📋 Quick Reference

### Start Regtest:
```bash
./dinerod --regtest --datadir=/tmp/dinero-regtest
```

### Start Mainnet:
```bash
./dinerod --datadir=~/.dinero
```

### Verify Network:
```bash
# Check .network file
cat /tmp/dinero-regtest/.network
# Should show: "regtest"

cat ~/.dinero/.network
# Should show: "main"
```

---

## ✅ Summary

Our regtest implementation is **bulletproof** with 7 independent layers of protection:

1. ✅ Different magic bytes (P2P protocol level)
2. ✅ Different ports (network level)
3. ✅ Different genesis (chain level)
4. ✅ Network marker file (filesystem level)
5. ✅ Different address prefixes (transaction level)
6. ✅ Startup warnings (human level)
7. ✅ Separate data directories (operational level)

**Result:** It is **virtually impossible** to accidentally mix mainnet and regtest data, even if you try!

---

## 🎯 ASERT Testing on Regtest

Regtest is configured with **ASERT from block 1**, just like mainnet, but with:

- **Easier difficulty** (0x207fffff) for instant mining
- **Same 5-minute target spacing** to test ASERT accurately
- **Same 12-hour half-life** to test adjustment speed

This allows you to:
- ✅ Test ASERT difficulty adjustment
- ✅ Simulate network hashrate changes
- ✅ Verify ASERT responds correctly to fast/slow blocks
- ✅ Ensure mainnet ASERT will work correctly

**All without any risk to mainnet data!**

---

## 📞 Support

If you encounter any issues with regtest:

1. Check `.network` file matches expected network
2. Verify you're using correct `--regtest` or `--testnet` flag
3. Use separate data directories for each network
4. Run `TEST_REGTEST_ASERT.sh` to verify setup

---

**Document Version:** 1.0
**Tested On:** macOS arm64 (Apple Silicon)
**Compiler:** clang++ (Apple Clang 15.0.0)

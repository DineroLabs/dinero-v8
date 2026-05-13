# 🔐 On-Chain Notarization Guide

**Purpose:** Permanently record the DineroCoin mainnet launch bundle hash on-chain.

**Bundle Hash:** `7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c`

---

## 📋 Prerequisites

1. **Daemon running** (`dinerod`)
2. **Wallet created** with funds (minimum 0.00000001 DIN)
3. **CLI tool available** (`dinero-cli` or built binary)
4. **Wallet unlocked** (if encrypted)

---

## 🚀 Step-by-Step Instructions

### Step 1: Build CLI Tool (if not already built)

```bash
cd /Users/haydarevich/Documents/DineroCoin
cmake --build build --target dinero-cli -j8
```

### Step 2: Start the Daemon

```bash
# For mainnet
./build/dinerod --datadir=/path/to/data --rpcport=20998 --printtoconsole

# Or for regtest (testing)
./build/dinerod --datadir=/tmp/test-dinero --regtest --rpcport=20998 --printtoconsole
```

Wait for daemon to start and sync.

### Step 3: Verify Daemon is Running

```bash
./build/dinero-cli getblockcount
```

Expected: Returns current block height.

### Step 4: Check Wallet Status

```bash
# Check if wallet exists
./build/dinero-cli getwalletinfo

# Check balance
./build/dinero-cli getbalance
```

**Required:** Balance must be at least `0.00000001 DIN` (1 una).

### Step 5: Unlock Wallet (if encrypted)

```bash
./build/dinero-cli walletpassphrase "your_password" 300
```

This unlocks the wallet for 300 seconds (5 minutes).

### Step 6: Send Notarization Transaction

```bash
BUNDLE_HASH="7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c"
NOTARIZATION_ADDRESS="din1q3a90d70ffae6a40e2e10b4b4a09e568e5f7c1c91"

echo -n "$BUNDLE_HASH" | \
  ./build/dinero-cli sendtoaddress "$NOTARIZATION_ADDRESS" \
  0.00000001 "" "Mainnet Launch Kit Notarization"
```

**Expected Output:**
```
Transaction ID: <64-character hex string>
```

### Step 7: Verify Transaction

```bash
# Get transaction details
TXID="<transaction_id_from_step_6>"
./build/dinero-cli gettransaction "$TXID"

# Check transaction in mempool
./build/dinero-cli getmempoolinfo

# Wait for confirmation (optional)
./build/dinero-cli gettransaction "$TXID" | grep confirmations
```

---

## 🔍 Verification Checklist

After sending the transaction, verify:

- [ ] Transaction ID received
- [ ] Transaction appears in `gettransaction` output
- [ ] Transaction comment contains: "Mainnet Launch Kit Notarization"
- [ ] Transaction amount is `0.00000001 DIN`
- [ ] Transaction recipient is `din1q3a90d70ffae6a40e2e10b4b4a09e568e5f7c1c91`
- [ ] Transaction confirms (after mining/block inclusion)

---

## 📝 Complete Notarization Script

Save this as `notarize_onchain.sh`:

```bash
#!/bin/bash
set -e

BUNDLE_HASH="7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c"
NOTARIZATION_ADDRESS="din1q3a90d70ffae6a40e2e10b4b4a09e568e5f7c1c91"
AMOUNT="0.00000001"
COMMENT="Mainnet Launch Kit Notarization"

echo "🔐 DineroCoin Mainnet Bundle Notarization"
echo "=========================================="
echo ""
echo "Bundle Hash: $BUNDLE_HASH"
echo "Address: $NOTARIZATION_ADDRESS"
echo "Amount: $AMOUNT DIN"
echo ""

# Find CLI tool
CLI_TOOL="./build/dinero-cli"
if [ ! -f "$CLI_TOOL" ]; then
    echo "❌ dinero-cli not found at $CLI_TOOL"
    echo "Build it first: cmake --build build --target dinero-cli"
    exit 1
fi

# Check daemon
if ! $CLI_TOOL getblockcount &> /dev/null; then
    echo "⚠️  Daemon not responding"
    echo "Start daemon: ./build/dinerod --datadir=/path/to/data"
    exit 1
fi

echo "✅ Daemon is running"
echo ""

# Send transaction
echo "📝 Sending notarization transaction..."
TXID=$(echo -n "$BUNDLE_HASH" | $CLI_TOOL sendtoaddress "$NOTARIZATION_ADDRESS" "$AMOUNT" "" "$COMMENT" 2>&1)

if [ $? -eq 0 ] && [ -n "$TXID" ] && [ ${#TXID} -eq 64 ]; then
    echo "✅ Notarization transaction sent!"
    echo ""
    echo "Transaction ID: $TXID"
    echo ""
    echo "🔍 Verifying transaction..."
    sleep 2
    
    $CLI_TOOL gettransaction "$TXID" 2>/dev/null || echo "Transaction pending..."
    
    echo ""
    echo "✅ Bundle hash is now permanently recorded on-chain"
    echo "   Transaction: $TXID"
    echo ""
    echo "🔗 View transaction:"
    echo "   ./build/dinero-cli gettransaction $TXID"
else
    echo "❌ Failed to send transaction"
    echo "Error: $TXID"
    echo ""
    echo "Common issues:"
    echo "  - Wallet not unlocked (use: walletpassphrase)"
    echo "  - Insufficient funds (need at least 0.00000001 DIN)"
    echo "  - Invalid address"
    exit 1
fi
```

Make it executable:
```bash
chmod +x notarize_onchain.sh
./notarize_onchain.sh
```

---

## 🔗 Querying Notarization Later

Once the transaction is confirmed, you can query it:

```bash
# Search for notarization transaction
./build/dinero-cli listtransactions "" 10000 | grep -A 10 -B 5 "Mainnet Launch Kit Notarization"

# Get specific transaction by ID
./build/dinero-cli gettransaction "<txid>"

# Search by address
./build/dinero-cli listtransactions "" 10000 | grep "din1q3a90d70ffae6a40e2e10b4b4a09e568e5f7c1c91"
```

---

## ⚠️ Important Notes

1. **Network Selection:** Ensure you're on the correct network (mainnet vs regtest)
2. **Transaction Fee:** The transaction will include a small fee (~0.00001 DIN)
3. **Confirmation:** Transaction needs to be included in a block to be permanent
4. **Backup:** Keep the transaction ID safe for future reference

---

## 📊 Notarization Record

After successful notarization, document:

- **Transaction ID:** `<txid>`
- **Block Height:** `<height>` (when confirmed)
- **Timestamp:** `<timestamp>`
- **Bundle Hash:** `7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c`
- **Network:** `mainnet` or `regtest`

This creates a permanent, immutable record of the launch kit's authenticity on the DineroCoin blockchain.

---

**"Dinero: Real Money for Free People"**

*Mainnet Launch — November 2025*


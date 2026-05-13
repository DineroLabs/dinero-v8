# Manual UTXO Injection CLI

This guide demonstrates how to manually inject UTXOs into the reference wallet for testing purposes **without requiring blockchain synchronization**.

## Overview

The UTXO injection CLI allows you to:
- ✅ Add UTXOs manually (simulating received transactions)
- ✅ Test wallet balance calculations
- ✅ Test deterministic UTXO selection
- ✅ Build test transactions
- ✅ Verify coinbase maturity rules

## Quick Start

### 1. Create a Test Wallet

```bash
./dinero-wallet-cli create test-wallet 12
```

This creates a wallet with a 12-word mnemonic and displays the address.

### 2. Add UTXOs Manually

```bash
# Add a regular UTXO
./dinero-wallet-cli add-utxo test-wallet abc123def456 0 5000000000 100 false

# Add a coinbase UTXO (requires >100 confirmations)
./dinero-wallet-cli add-utxo test-wallet coinbase12345 0 5000000000 50 true

# Add more UTXOs for testing
./dinero-wallet-cli add-utxo test-wallet aaa111bbb222 1 3000000000 150 false
./dinero-wallet-cli add-utxo test-wallet zzz999yyy888 0 2000000000 200 false
```

**Parameters:**
- `<wallet>` - Wallet name
- `<txid>` - Transaction ID (any string)
- `<vout>` - Output index (0, 1, 2, ...)
- `<amount>` - Amount in una (1 DIN = 100000000 una)
- `<height>` - Block height when confirmed
- `[coinbase]` - Optional: `true` if coinbase output

### 3. Set Current Blockchain Height

UTXOs need confirmations relative to a current height. Set the wallet's view of the current blockchain height:

```bash
./dinero-wallet-cli set-height test-wallet 250
```

This makes:
- UTXO at height 100: 150 confirmations (confirmed ✅)
- UTXO at height 50: 200 confirmations (confirmed ✅, coinbase mature ✅)
- UTXO at height 200: 50 confirmations (confirmed ✅)

### 4. Check Balance

```bash
./dinero-wallet-cli balance test-wallet
```

Output:
```
╔══════════════════════════════════════════════════════════════════╗
║                    WALLET BALANCE                                ║
╚══════════════════════════════════════════════════════════════════╝

Confirmed:   150.00000000 DIN
Unconfirmed: 0.00000000 DIN
Immature:    0.00000000 DIN
───────────────────────────────────────────────────────────
Total:       150.00000000 DIN
```

### 5. List UTXOs (Deterministic Sorting)

```bash
./dinero-wallet-cli list-utxos test-wallet
```

UTXOs are **always sorted deterministically** by (txid, vout) lexicographically:

```
Count: 4

TXID: aaa111bbb222:1
Amount: 30.00000000 DIN
Height: 150
───────────────────────────────────────────────────────────
TXID: abc123def456:0
Amount: 50.00000000 DIN
Height: 100
───────────────────────────────────────────────────────────
TXID: coinbase12345:0
Amount: 50.00000000 DIN
Height: 50 (coinbase)
───────────────────────────────────────────────────────────
TXID: zzz999yyy888:0
Amount: 20.00000000 DIN
Height: 200
```

### 6. Build Test Transaction

Test the deterministic UTXO selection algorithm:

```bash
./dinero-wallet-cli build-tx test-wallet din1qtest123456789 8000000000 10000
```

Output:
```
╔══════════════════════════════════════════════════════════════════╗
║                    BUILD TRANSACTION                             ║
╚══════════════════════════════════════════════════════════════════╝

To:     din1qtest123456789
Amount: 80.00000000 DIN
Fee:    0.00010000 DIN
Total:  80.00010000 DIN

Selected UTXOs:
  aaa111bbb222:1 - 30.00000000 DIN    (selected 1st - lowest in sort order)
  abc123def456:0 - 50.00000000 DIN    (selected 2nd)
  coinbase12345:0 - 50.00000000 DIN   (selected 3rd)

Total Input: 130.00000000 DIN
Change:      49.99990000 DIN

✅ Transaction build successful!
Note: This is a simulation. No actual transaction was created.
```

**Key Observation:** UTXOs are selected in deterministic sort order (lowest-first) until the target amount is reached.

### 7. Remove UTXO (Mark as Spent)

```bash
./dinero-wallet-cli remove-utxo test-wallet abc123def456 0
```

This marks the UTXO as spent and updates the balance.

## Use Cases

### Testing Coinbase Maturity

Coinbase outputs require >100 confirmations (101+) to be spendable:

```bash
# Add coinbase at height 50
./dinero-wallet-cli add-utxo test-wallet coinbase1 0 5000000000 50 true

# Set height to 150 (coinbase has exactly 100 confirmations)
./dinero-wallet-cli set-height test-wallet 150
./dinero-wallet-cli balance test-wallet
# Shows as "Immature" ❌

# Set height to 151 (coinbase has 101 confirmations)
./dinero-wallet-cli set-height test-wallet 151
./dinero-wallet-cli balance test-wallet
# Shows as "Confirmed" ✅
```

### Testing Deterministic UTXO Selection

The wallet **always selects UTXOs in the same order** for the same inputs:

```bash
# Same command, always selects same UTXOs in same order
./dinero-wallet-cli build-tx test-wallet din1qtest 5000000000 10000
```

This guarantees byte-identical transaction building for the same inputs.

### Testing Insufficient Funds

```bash
./dinero-wallet-cli build-tx test-wallet din1qtest 200000000000 10000
```

Output:
```
❌ Transaction build failed: Insufficient funds: need 200000010000 una, have 150000000000 una
```

## Complete Workflow Example

```bash
# 1. Create wallet
./dinero-wallet-cli create my-test-wallet

# 2. Add some UTXOs
./dinero-wallet-cli add-utxo my-test-wallet tx1 0 1000000000 100 false
./dinero-wallet-cli add-utxo my-test-wallet tx2 0 2000000000 200 false
./dinero-wallet-cli add-utxo my-test-wallet tx3 0 500000000 300 false

# 3. Set current height
./dinero-wallet-cli set-height my-test-wallet 500

# 4. Check balance
./dinero-wallet-cli balance my-test-wallet

# 5. Build test transaction
./dinero-wallet-cli build-tx my-test-wallet din1qrecipi3nt 2500000000 10000

# 6. List UTXOs to see deterministic ordering
./dinero-wallet-cli list-utxos my-test-wallet

# 7. Get wallet info
./dinero-wallet-cli info my-test-wallet
```

## Important Notes

- ⚠️ This is for **testing only** - no actual blockchain transactions are created
- ✅ Script pubkey is automatically set to a placeholder (would come from actual tx in production)
- ✅ All UTXO selection is **deterministic** (same inputs = same outputs)
- ✅ Coinbase maturity rules are enforced (>100 confirmations required)
- ✅ Balance calculations are **accurate** based on current height

## Next Steps

After validating the wallet behavior with manual UTXO injection, the next step is to implement **minimal blockchain sync** to automatically discover and add UTXOs from the real blockchain.

# RPC gettransaction Tests

Tests for `blockchain.gettransaction` and `wallet.gettransaction` RPC methods.

## Test Files

### Unit Tests: `test_rpc_gettransaction.cpp`
Location: `/tests/test_rpc_gettransaction.cpp`

Tests transaction lookup logic, output type detection, and JSON formatting without requiring a running daemon.

**Tests:**
- Output type detection (Taproot, SegWit v0, P2WPKH, P2WSH)
- Coinbase transaction detection
- Witness version detection
- Confirmation calculation
- JSON field validation
- Edge case handling

**Compile:**
```bash
# Via CMake (recommended - add to CMakeLists.txt):
add_executable(test_rpc_gettransaction tests/test_rpc_gettransaction.cpp)
target_link_libraries(test_rpc_gettransaction
    dinero_consensus
    dinero_wallet
    jsoncpp_static
)

# Run:
./build/test_rpc_gettransaction
```

### Integration Tests: `test_rpc_gettransaction.py`
Location: `/tests/integration/test_rpc_gettransaction.py`

Tests actual RPC endpoints against a running daemon with real blockchain data.

**Prerequisites:**
- Running `dinerod` with `-datadir=/tmp/dinero_data`
- Blockchain with at least block 1 (premine transaction)
- Wallet with at least one transaction

**Run:**
```bash
# Make executable
chmod +x tests/integration/test_rpc_gettransaction.py

# Run tests
./tests/integration/test_rpc_gettransaction.py

# Or with Python directly
python3 tests/integration/test_rpc_gettransaction.py
```

**Tests:**
- `blockchain.gettransaction` with invalid TXID
- `blockchain.gettransaction` with non-existent transaction
- `blockchain.gettransaction` with premine transaction
- Taproot output type detection
- `wallet.gettransaction` with missing transaction
- `wallet.gettransaction` with wallet transaction
- Consistency between blockchain and wallet views

## Manual Testing

### Test blockchain.gettransaction

```bash
# Start daemon
./build/bin/dinerod -datadir=/tmp/dinero_data -daemon

# Get block 1 hash
BLOCK1_HASH=$(./build/bin/dinero-cli -datadir=/tmp/dinero_data blockchain.getblockhash 1)

# Get premine TXID from block 1
PREMINE_TXID=$(./build/bin/dinero-cli -datadir=/tmp/dinero_data blockchain.getblock $BLOCK1_HASH | grep -A 3 '"tx"' | grep -v 'tx\|^\[' | tr -d ' ",')

# Get transaction details
./build/bin/dinero-cli -datadir=/tmp/dinero_data blockchain.gettransaction $PREMINE_TXID
```

**Expected output:**
```json
{
  "txid": "9322fc8a...",
  "blockhash": "00000ceab...",
  "blockheight": 1,
  "confirmations": 105,
  "status": "confirmed",
  "is_coinbase": true,
  "inputs": [...],
  "outputs": [{
    "type": "taproot",
    "value_din": "2627900.00000000"
  }],
  "witness_version": "segwit_v0"
}
```

### Test wallet.gettransaction

```bash
# Get a recent wallet transaction
WALLET_TXID=$(./build/bin/dinero-cli -datadir=/tmp/dinero_data wallet.listtransactions | grep '"txid"' | head -1 | awk -F'"' '{print $4}')

# Get detailed transaction info
./build/bin/dinero-cli -datadir=/tmp/dinero_data wallet.gettransaction $WALLET_TXID
```

**Expected output:**
```json
{
  "txid": "d219c120...",
  "status": "confirmed",
  "confirmations": 105,
  "category": "receive",
  "amount": 262789.99999985896,
  "blockhash": "000068cc...",
  "blockheight": 105,
  "inputs": [{"txid": "...", "vout": 0}],
  "outputs": [
    {"vout": 0, "value": 1.0, "type": "taproot"},
    {"vout": 1, "value": 2627898.99999859, "type": "taproot"}
  ]
}
```

## Test Coverage

### blockchain.gettransaction
- ✅ Transaction not found (returns error or status=not_found)
- ✅ Coinbase transaction detection
- ✅ Taproot output type detection
- ✅ SegWit v0 output type detection
- ✅ Confirmation count calculation
- ✅ Block height and hash
- ✅ Input/output arrays
- ✅ Transaction metadata (version, locktime)

### wallet.gettransaction
- ✅ Transaction not in wallet (returns error with hint)
- ✅ Wallet transaction lookup
- ✅ Category field (send/receive/coinbase)
- ✅ Wallet metadata (labels, amounts)
- ✅ Enrichment with blockchain data
- ✅ Input/output breakdown

### Integration
- ✅ Consistency between blockchain and wallet views
- ✅ Same confirmations reported
- ✅ Same block location
- ✅ Complementary data (blockchain has chain truth, wallet has context)

## Known Issues / Future Enhancements

1. **RPC Authentication in Python tests**
   - Current issue with cookie-based auth in integration tests
   - Workaround: Use CLI for manual testing
   - TODO: Fix Python RPC client auth

2. **Input amounts**
   - Currently not included (requires looking up prevout)
   - Future enhancement: Add `dinero::storage::GetTransaction()` support

3. **Change output detection**
   - Currently all outputs marked as "output"
   - Future enhancement: Compare scriptPubKeys with wallet addresses

4. **Fee calculation**
   - Not currently shown
   - Future enhancement: total_input - total_output

5. **Transaction hex**
   - Not currently included
   - Future enhancement: Add raw hex serialization

## Success Criteria

All tests pass when:
- Unit tests compile and run successfully
- Integration tests pass against running daemon
- Manual CLI tests produce expected output
- Taproot transactions properly identified
- Blockchain and wallet views are consistent

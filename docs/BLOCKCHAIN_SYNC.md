# Reference Wallet Blockchain Synchronization

This guide explains how to synchronize the DineroCoin reference wallet with a blockchain node.

## Overview

The blockchain sync feature allows the reference wallet to:
- Connect to a DineroCoin node via RPC
- Scan blocks for transactions relevant to the wallet's address
- Automatically discover and add UTXOs
- Track spent outputs
- Maintain accurate balance information

## Architecture

The blockchain sync is:
- **Read-only**: Never broadcasts transactions
- **Address-scoped**: Only scans for the wallet's single address
- **Deterministic**: Same blockchain state always produces same UTXO set
- **Simple**: Forward-scan only (no complex reorg handling initially)

## Prerequisites

1. A running DineroCoin node with RPC enabled
2. RPC credentials (username and password)
3. An existing reference wallet

## Quick Start

### 1. Start a DineroCoin Node

First, ensure you have a DineroCoin node running with RPC enabled:

```bash
./dinerod -rpcuser=dinero -rpcpassword=yourpassword -server
```

Note the RPC endpoint (default: `http://127.0.0.1:8332`)

### 2. Create a Wallet

If you haven't already, create a reference wallet:

```bash
./dinero-wallet-cli create my-wallet
```

This will display your wallet address (e.g., `din1q...`).

### 3. Sync with Blockchain

Synchronize your wallet with the blockchain:

```bash
./dinero-wallet-cli sync my-wallet http://127.0.0.1:8332 dinero yourpassword
```

**Parameters:**
- `<wallet>` - Wallet name
- `<rpc_url>` - RPC endpoint URL
- `<rpc_user>` - RPC username
- `<rpc_password>` - RPC password
- `[start_height]` - Optional: height to start scanning from (default: 0)
- `[max_blocks]` - Optional: maximum blocks to scan (default: scan to tip)

### 4. Check Your Balance

After syncing, check your wallet balance:

```bash
./dinero-wallet-cli balance my-wallet
```

Output:
```
╔══════════════════════════════════════════════════════════════════╗
║                    WALLET BALANCE                                ║
╚══════════════════════════════════════════════════════════════════╝

Confirmed:   10.50000000 DIN
Unconfirmed: 0.00000000 DIN
Immature:    0.00000000 DIN
───────────────────────────────────────────────────────────
Total:       10.50000000 DIN
```

## Advanced Usage

### Partial Sync (Scan Specific Range)

To sync only a specific range of blocks:

```bash
# Sync from block 1000 to 2000 (1000 blocks total)
./dinero-wallet-cli sync my-wallet http://127.0.0.1:8332 dinero password 1000 1000
```

### Resume Interrupted Sync

If sync is interrupted, you can resume from where you left off:

```bash
# Check current height
./dinero-wallet-cli info my-wallet
# Shows: Last Block: 1500

# Resume from height 1501
./dinero-wallet-cli sync my-wallet http://127.0.0.1:8332 dinero password 1501
```

### Incremental Sync

For an existing synced wallet, periodically sync new blocks:

```bash
# Get current wallet height
CURRENT_HEIGHT=$(./dinero-wallet-cli info my-wallet | grep "Last Block" | awk '{print $3}')

# Sync from next block
NEXT_HEIGHT=$((CURRENT_HEIGHT + 1))
./dinero-wallet-cli sync my-wallet http://127.0.0.1:8332 dinero password $NEXT_HEIGHT
```

## Sync Process Details

### What Happens During Sync

1. **Connect to Node**: Establishes RPC connection and verifies connectivity
2. **Fetch Block Range**: Determines start and end heights
3. **Scan Blocks**: For each block:
   - Fetches block data via RPC
   - Parses all transactions
   - Checks each output for wallet's address
   - Adds matching UTXOs to database
   - Marks spent UTXOs
4. **Update Metadata**: Updates wallet's last synced height

### Progress Output

During sync, you'll see verbose output:

```
╔══════════════════════════════════════════════════════════════════╗
║                    BLOCKCHAIN SYNC                               ║
╚══════════════════════════════════════════════════════════════════╝

Wallet:       my-wallet
Address:      din1qxv908ny37t6m6kms89jc5uf858zgt7s4fa8pn8
RPC URL:      http://127.0.0.1:8332
Start Height: 0
Max Blocks:   Sync to tip

Connected to blockchain node at http://127.0.0.1:8332
Starting sync from height 0 to 1543

Scanned block 100/1543 (2 txs, 0 UTXOs)
  Found UTXO: abc123...def456:0 (10.00000000 DIN)
Scanned block 200/1543 (5 txs, 1 UTXOs)
Scanned block 300/1543 (3 txs, 1 UTXOs)
...

Sync completed!
  Blocks scanned: 1543
  Transactions found: 3
  UTXOs added: 3
  UTXOs spent: 0

✅ Blockchain sync completed successfully!

Updated Balance:
  Confirmed:   30.50000000 DIN
  Unconfirmed: 0.00000000 DIN
  Immature:    0.00000000 DIN
  ───────────────────────────────────────────────────────────
  Total:       30.50000000 DIN
```

## UTXO Detection

The sync process detects:

### Incoming UTXOs
- Scans transaction outputs (`vout`) in each block
- Matches outputs to wallet's address
- Extracts amount and script pubkey
- Marks coinbase outputs appropriately
- Adds UTXO to wallet database

### Spent UTXOs
- Scans transaction inputs (`vin`) in each block
- Checks if input spends a UTXO in wallet
- Marks UTXO as spent in database
- Updates balance accordingly

## Deterministic Behavior

The sync process guarantees deterministic results:

1. **Same blockchain state = same UTXO set**
   - Syncing the same blocks always produces identical results
   - No randomness or non-deterministic behavior

2. **Address-scoped scanning**
   - Only wallet's single address is considered
   - No address derivation or discovery

3. **Forward-scan only**
   - Blocks are processed in height order
   - No look-ahead or backtracking

## Coinbase Maturity

The wallet correctly handles coinbase maturity:

- Coinbase outputs require >100 confirmations (101+) to be spendable
- During sync, coinbase UTXOs are marked with `is_coinbase=true`
- Balance calculation respects maturity rules
- Immature coinbase appears in "Immature" balance category

## Error Handling

### Connection Errors

If the wallet cannot connect to the node:

```
❌ Blockchain sync error: Failed to initialize blockchain sync:
   Failed to connect to blockchain node at http://127.0.0.1:8332
```

**Solution**: Verify node is running and RPC endpoint is correct.

### Authentication Errors

If RPC credentials are incorrect:

```
❌ Blockchain sync error: RPC request failed: Unauthorized
```

**Solution**: Verify RPC username and password.

### Invalid Block Range

If start height is beyond blockchain tip:

```
❌ Blockchain sync error: Failed to get block hash for height 999999
```

**Solution**: Check current blockchain height and adjust start height.

## Integration with Manual UTXO Injection

The blockchain sync feature is complementary to manual UTXO injection:

- **Manual injection**: For testing without a node (see MANUAL_UTXO_INJECTION.md)
- **Blockchain sync**: For production use with a live node

You can mix both approaches:
1. Use manual injection for testing
2. Use blockchain sync for real transactions
3. Use `list-utxos` to see all UTXOs regardless of source

## Performance Considerations

### Sync Speed

- Initial sync of full blockchain can take time
- Speed depends on:
  - Node RPC performance
  - Network latency
  - Number of blocks to scan
  - Transaction density in blocks

### Optimization Tips

1. **Use batch_size parameter** (future enhancement):
   - Fetch multiple blocks at once
   - Reduces RPC round-trips

2. **Sync incrementally**:
   - Don't resync from genesis each time
   - Resume from last synced height

3. **Use fast initial sync**:
   - If you know wallet was created recently
   - Start from creation block height

## RPC Compatibility

The blockchain sync uses these RPC methods:

- `getblockchaininfo` - Get current blockchain state
- `getblockhash` - Get block hash for height
- `getblock` - Get full block data with transactions

**Requirements:**
- Node must support these RPC methods
- Transaction data must include:
  - `txid` - Transaction ID
  - `vout` - Output array with amount and scriptPubKey
  - `vin` - Input array with spent txid/vout
  - Block `height` - Block height

## Security Considerations

1. **RPC credentials**: Store securely, never commit to version control
2. **Connection security**: Use secure connection (HTTPS) for remote nodes
3. **Node trust**: Wallet trusts node's blockchain data completely
4. **No broadcast**: Sync is read-only, never broadcasts transactions

## Troubleshooting

### Sync Hangs or Stalls

1. Check node is responsive: `curl http://127.0.0.1:8332`
2. Verify RPC timeout (default: 30 seconds)
3. Check network connectivity

### Missing Transactions

1. Verify correct address: `./dinero-wallet-cli address my-wallet`
2. Check transaction is confirmed in a block
3. Verify sync covered the block height
4. Use block explorer to confirm transaction exists

### Incorrect Balance

1. Resync from genesis: `./dinero-wallet-cli sync my-wallet ... 0`
2. Check for duplicate UTXOs: `./dinero-wallet-cli list-utxos my-wallet`
3. Verify current height: `./dinero-wallet-cli info my-wallet`

## Future Enhancements

Planned improvements:

1. **Reorg handling**: Automatic chain reorganization detection
2. **Mempool tracking**: Monitor unconfirmed transactions
3. **Parallel sync**: Fetch multiple blocks concurrently
4. **Checkpoint sync**: Fast sync using trusted checkpoints
5. **SPV mode**: Light client sync without full node

## Example Workflows

### New Wallet, Full Sync

```bash
# Create wallet
./dinero-wallet-cli create production-wallet

# Record address
ADDRESS=$(./dinero-wallet-cli address production-wallet)
echo "Wallet address: $ADDRESS"

# Full sync from genesis
./dinero-wallet-cli sync production-wallet http://localhost:8332 dinero password

# Check balance
./dinero-wallet-cli balance production-wallet
```

### Existing Wallet, Incremental Sync

```bash
# Get current height
INFO=$(./dinero-wallet-cli info existing-wallet)
CURRENT=$(echo "$INFO" | grep "Last Block" | awk '{print $3}')

# Sync new blocks
NEXT=$((CURRENT + 1))
./dinero-wallet-cli sync existing-wallet http://localhost:8332 dinero password $NEXT
```

### Test Wallet with Known Block Range

```bash
# Create test wallet
./dinero-wallet-cli create test-wallet

# Sync only blocks 1000-2000 (for testing)
./dinero-wallet-cli sync test-wallet http://localhost:8332 dinero password 1000 1000
```

## See Also

- [MANUAL_UTXO_INJECTION.md](MANUAL_UTXO_INJECTION.md) - Manual UTXO injection for testing
- [WALLET_OVERVIEW.md](WALLET_OVERVIEW.md) - Reference wallet architecture
- [RPC_API.md](RPC_API.md) - DineroCoin RPC API documentation

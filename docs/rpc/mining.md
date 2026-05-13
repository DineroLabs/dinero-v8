# Mining RPC Methods

Mining RPC methods provide secure, wallet-aware mining address management with strict validation and persistence.

## generatetoaddress

Mines one or more blocks immediately to a specified address. This method is intended for **regtest and testnet only** for testing and development workflows.

**Behavior (Bitcoin Core Compatible):**
- Mines exactly N blocks sequentially
- Each block builds on the current chain tip (fresh template per block)
- Returns an array of N block hashes
- All blocks are broadcast to connected peers

**Syntax:**
```bash
generatetoaddress <nblocks> <address>
```

**Parameters:**
1. `nblocks` (integer, required): Number of blocks to mine (1-1000)
2. `address` (string, required): Address to receive block rewards (must match network HRP)

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "generatetoaddress",
  "params": [10, "rdin1px32rxxxmu03rh6mvp43q4jwvpl3utz206rlfuvg5pv5wk2wmgwgqpxxg8n"],
  "id": 1
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "blocks": [
      "0000abcd1234...",
      "0000abcd5678...",
      "0000abcd9abc..."
    ],
    "message": "Generated 10 blocks"
  }
}
```

**Example (CLI):**
```bash
# Mine 10 blocks to a regtest address
./dinero-cli -datadir=/tmp/regtest generatetoaddress 10 rdin1px32rxxxmu03rh6mvp43q4jwvpl3utz206rlfuvg5pv5wk2wmgwgqpxxg8n

# Mine 100 blocks for coinbase maturity testing
./dinero-cli -datadir=/tmp/regtest generatetoaddress 100 rdin1q...
```

**Invariants (Correctness Guarantees):**
- `generatetoaddress N` **always** mines exactly N blocks (not fewer)
- Each block's `prev_block_hash` points to the hash of the previous block
- Chain height increases by exactly N after completion
- No cached block templates—fresh tip is fetched on every iteration

**Error Handling:**
```json
// Invalid block count
{
  "error": {
    "code": -32602,
    "message": "Invalid parameters: nblocks must be between 1 and 1000"
  }
}

// Invalid address format
{
  "error": {
    "code": -32602,
    "message": "Invalid address format"
  }
}

// Block rejection (e.g., stale prevhash)
{
  "error": {
    "code": -32000,
    "message": "Block rejected: stale-prevhash: Block parent X does not match current tip Y"
  }
}
```

**Network Restrictions:**
- **Regtest**: ✅ Enabled (instant mining, no PoW)
- **Testnet**: ✅ Enabled (real PoW, may be slow)
- **Mainnet**: ❌ Disabled (security risk—never enable on mainnet)

---

## mining.setaddress

Sets the mining address for the active or scoped wallet with comprehensive validation.

**Request (positional parameters):**
```json
{
  "jsonrpc": "2.0",
  "method": "mining.setaddress",
  "params": ["rdin1qz8nwny729fx4u529l0pt2l4hghn60fkmts3zd4"],
  "id": 1
}
```

**Request (named parameters):**
```json
{
  "jsonrpc": "2.0",
  "method": "mining.setaddress",
  "params": {"address": "rdin1qz8nwny729fx4u529l0pt2l4hghn60fkmts3zd4"},
  "id": 1
}
```

**Response (success):**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "address": "rdin1qz8nwny729fx4u529l0pt2l4hghn60fkmts3zd4",
    "wallet": "my_wallet",
    "network": "rdin",
    "ismine": true,
    "scriptPubKey": "001411e6e993ca2a4d5e5145fbc2b57eb745e7a7a6db",
    "status": "Mining address set successfully"
  }
}
```

**Wallet-Scoped Usage:**
```bash
# Set mining address for specific wallet
curl -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","method":"mining.setaddress","params":["rdin1q..."],"id":1}' \
  http://127.0.0.1:20996/wallet/mining_wallet
```

## mining.getaddress

Retrieves the current mining address for the active or scoped wallet.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "mining.getaddress",
  "params": [],
  "id": 1
}
```

**Response (configured address):**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "address": "rdin1qz8nwny729fx4u529l0pt2l4hghn60fkmts3zd4",
    "wallet": "my_wallet",
    "network": "rdin",
    "ismine": true,
    "source": "configured"
  }
}
```

**Response (no address configured):**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "address": null,
    "wallet": "my_wallet",
    "network": "rdin",
    "source": "none",
    "message": "No mining address configured"
  }
}
```

## Security & Validation

### Address Ownership Validation
Mining addresses **must** be owned by the target wallet to ensure mining rewards are spendable:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -5,
    "message": "Address not owned by wallet: mining rewards would be unspendable"
  }
}
```

### Network HRP Validation
Mining addresses must match the active network's HRP to prevent cross-network errors:

**Wrong Network (Bitcoin mainnet on regtest):**
```bash
curl -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","method":"mining.setaddress","params":["bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4"],"id":1}' \
  http://127.0.0.1:20996/
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -5,
    "message": "Invalid address format"
  }
}
```

### Network HRP Reference
- **Mainnet**: `din1...` (HRP: din)
- **Testnet**: `tdin1...` (HRP: tdin)
- **Regtest**: `rdin1...` (HRP: rdin)

## Persistence

Mining addresses are persisted in the wallet SQLite database with network scoping:

- **Table**: `settings`
- **Key**: `mining_address`
- **Scope**: Wallet name + Network HRP
- **Survival**: Persists across daemon restarts and wallet reloads

**Database Schema:**
```sql
CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT,
    wallet TEXT,
    network TEXT,
    verified_at INTEGER,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);
```

## Error Handling

**No Active Wallet:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -18,
    "message": "No active wallet. Load a wallet first or use wallet-scoped URL."
  }
}
```

**Invalid Address Format:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -5,
    "message": "Invalid address format"
  }
}
```

**Address Not Owned:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -5,
    "message": "Address not owned by wallet: mining rewards would be unspendable"
  }
}
```

## Complete Example

```bash
#!/bin/bash
# Complete mining address setup example

# 1. Start daemon
DATADIR="/tmp/mining-test"
./build/bin/dinerod --regtest --datadir="$DATADIR" --rpcport=20996 &
sleep 5

# 2. Auth
AUTH="$(cat "$DATADIR/.cookie")"

# 3. Create and load wallet
curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","method":"wallet.create","params":{"name":"mining_wallet"},"id":1}' \
  http://127.0.0.1:20996/

curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","method":"wallet.load","params":{"name":"mining_wallet"},"id":2}' \
  http://127.0.0.1:20996/

# 4. Generate mining address
ADDR=$(curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","method":"wallet.getnewaddress","id":3}' \
  http://127.0.0.1:20996/wallet/mining_wallet | jq -r .result.address)

# 5. Set mining address (wallet-scoped)
curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data "{\"jsonrpc\":\"2.0\",\"method\":\"mining.setaddress\",\"params\":[\"$ADDR\"],\"id\":4}" \
  http://127.0.0.1:20996/wallet/mining_wallet

# 6. Verify mining address
curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","method":"mining.getaddress","id":5}' \
  http://127.0.0.1:20996/wallet/mining_wallet

# 7. Test persistence (restart daemon and check)
pkill dinerod && sleep 2
./build/bin/dinerod --regtest --datadir="$DATADIR" --rpcport=20996 &
sleep 5

curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","method":"wallet.load","params":{"name":"mining_wallet"},"id":6}' \
  http://127.0.0.1:20996/

curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","method":"mining.getaddress","id":7}' \
  http://127.0.0.1:20996/wallet/mining_wallet
```

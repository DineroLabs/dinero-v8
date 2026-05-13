# DineroCoin CLI RPC Parity Matrix

## Current RPC Coverage Analysis

### ✅ **Implemented CLI Commands**

| Daemon RPC Method | CLI Command | Status | Notes |
|-------------------|-------------|---------|-------|
| `wallet.create` | `wallet create` | ✅ Complete | Creates new HD wallet |
| `wallet.load` | `wallet load` | ✅ Complete | Loads existing wallet |
| `wallet.encrypt` | `wallet encrypt` | ✅ Complete | Encrypts wallet with passphrase |
| `wallet.lock` | `wallet lock` | ✅ Complete | Locks encrypted wallet |
| `wallet.unlock` | `wallet unlock` | ✅ Complete | Unlocks with passphrase |
| `wallet.balance` | `wallet balance` | ✅ Complete | Gets confirmed/unconfirmed balance |
| `wallet.history` | `wallet history` | ✅ Complete | Transaction history with paging |
| `wallet.utxos` | `wallet utxos` | ✅ Complete | Lists unspent outputs |
| `wallet.addresses` | `wallet addresses` | ✅ Complete | Lists wallet addresses |
| `wallet.getnewaddress` | `wallet getnewaddress` | ✅ Complete | Generates new address |
| `wallet.label` | `wallet label` | ✅ Complete | Sets address label |
| `tx.send` | `send` | ✅ Complete | Send transaction with coin control |
| `mining.info` | `mining info` | ✅ Complete | Mining status and hashrate |
| `mining.start` | `mining start` | ✅ Complete | Start mining with thread count |
| `mining.stop` | `mining stop` | ✅ Complete | Stop mining |
| `mining.setaddress` | `mining setaddress` | ✅ Complete | Set mining payout address |
| `mining.getaddress` | `mining getaddress` | ✅ Complete | Get current mining address |
| `node.info` | `nodeinfo print` | ✅ Complete | Node status and blockchain info |
| `rpc.methods` | `rpc methods` | ✅ Complete | List available RPC methods |

### ❌ **Missing CLI Commands (High Priority)**

| Daemon RPC Method | Missing CLI Command | Priority | Implementation |
|-------------------|-------------------|----------|----------------|
| `getnetworkinfo` | `chain networkinfo` | HIGH | Network peers, version info |
| `getmempoolinfo` | `chain mempoolinfo` | HIGH | Mempool size, fees |
| `getblockchaininfo` | `chain info` | HIGH | Block count, difficulty, chain tips |
| `listwallets` | `wallet list` | HIGH | Show available wallets |
| `validateaddress` | `wallet validateaddress` | MEDIUM | Address validation and info |
| `estimatefee` | `chain estimatefee` | MEDIUM | Fee estimation for transactions |
| `getblock` | `chain getblock` | MEDIUM | Get block by hash/height |
| `gettransaction` | `chain gettransaction` | MEDIUM | Get transaction details |
| `getaddressinfo` | `wallet addressinfo` | LOW | Address metadata and labels |
| `backupwallet` | `wallet backup` | LOW | Backup wallet to file |
| `restorewallet` | `wallet restore` | LOW | Restore wallet from backup |

### 🔧 **Implementation Plan**

#### Phase 1: Critical Chain Info Commands (HIGH)
```bash
# Network information
dinero-cli chain networkinfo
{
  "version": "1.0.0",
  "protocol_version": 1,
  "connections": 8,
  "networks": ["ipv4", "ipv6"],
  "relay_fee": 0.0001
}

# Mempool information  
dinero-cli chain mempoolinfo
{
  "size": 42,
  "bytes": 15680,
  "usage": 89120,
  "max_mempool": 300000000,
  "mempool_min_fee": 0.0001
}

# Blockchain information
dinero-cli chain info
{
  "chain": "main",
  "blocks": 12345,
  "headers": 12345,
  "best_block_hash": "000000abc...",
  "difficulty": 1.0,
  "verification_progress": 1.0
}
```

#### Phase 2: Wallet Management Commands (HIGH)
```bash
# List available wallets
dinero-cli wallet list
{
  "wallets": [
    {"name": "default", "loaded": true, "encrypted": false},
    {"name": "savings", "loaded": false, "encrypted": true}
  ]
}

# Validate address
dinero-cli wallet validateaddress din1abc123...
{
  "is_valid": true,
  "address": "din1abc123...",
  "script_type": "witness_v0_keyhash",
  "is_mine": true,
  "is_watchonly": false,
  "label": "My Address"
}
```

#### Phase 3: Transaction and Block Commands (MEDIUM)
```bash
# Fee estimation
dinero-cli chain estimatefee 6
{
  "feerate": 0.0001,
  "blocks": 6
}

# Get block details
dinero-cli chain getblock 000000abc...
{
  "hash": "000000abc...",
  "height": 12345,
  "time": 1694123456,
  "tx": ["txid1", "txid2"],
  "size": 1024
}

# Get transaction
dinero-cli chain gettransaction abc123...
{
  "txid": "abc123...",
  "amount": 1.5,
  "confirmations": 6,
  "blockhash": "000000abc...",
  "time": 1694123456
}
```

### 🚀 **Enhanced Command Groups**

#### Wallet Commands (Complete)
- ✅ `wallet create` - Create new wallet
- ✅ `wallet load` - Load existing wallet  
- ✅ `wallet encrypt` - Encrypt wallet
- ✅ `wallet lock` - Lock wallet
- ✅ `wallet unlock` - Unlock wallet
- ✅ `wallet balance` - Get balance
- ✅ `wallet history` - Transaction history
- ✅ `wallet utxos` - List UTXOs
- ✅ `wallet addresses` - List addresses
- ✅ `wallet getnewaddress` - Generate address
- ✅ `wallet label` - Set address label
- ❌ `wallet list` - List wallets (MISSING)
- ❌ `wallet validateaddress` - Validate address (MISSING)
- ❌ `wallet backup` - Backup wallet (MISSING)
- ❌ `wallet restore` - Restore wallet (MISSING)

#### Chain Commands (Partial)
- ✅ `chain getbestblockhash` - Best block hash
- ✅ `chain getblockcount` - Block count
- ❌ `chain networkinfo` - Network info (MISSING)
- ❌ `chain mempoolinfo` - Mempool info (MISSING)
- ❌ `chain info` - Blockchain info (MISSING)
- ❌ `chain estimatefee` - Fee estimation (MISSING)
- ❌ `chain getblock` - Get block (MISSING)
- ❌ `chain gettransaction` - Get transaction (MISSING)

#### Mining Commands (Complete)
- ✅ `mining info` - Mining status
- ✅ `mining start` - Start mining
- ✅ `mining stop` - Stop mining
- ✅ `mining setaddress` - Set mining address
- ✅ `mining getaddress` - Get mining address
- ✅ `mining generatetoaddress` - Generate blocks (regtest)

#### Send Commands (Complete)
- ✅ `send` - Send transaction with coin control

#### RPC Commands (Complete)
- ✅ `rpc call` - Direct RPC passthrough
- ✅ `rpc methods` - List RPC methods

### 📊 **Coverage Statistics**

| Command Group | Implemented | Missing | Coverage |
|---------------|-------------|---------|----------|
| Wallet | 11 | 4 | 73% |
| Chain | 2 | 6 | 25% |
| Mining | 6 | 0 | 100% |
| Send | 1 | 0 | 100% |
| RPC | 2 | 0 | 100% |
| **Total** | **22** | **10** | **69%** |

### 🎯 **Implementation Priority**

1. **Immediate (HIGH)**: `chain networkinfo`, `chain mempoolinfo`, `chain info`, `wallet list`
2. **Soon (MEDIUM)**: `wallet validateaddress`, `chain estimatefee`, `chain getblock`
3. **Later (LOW)**: `wallet backup`, `wallet restore`, `chain gettransaction`

### 🔧 **Command Template for Missing Methods**

```cpp
// Template for new chain commands
ErrorHandler::ExitCode cmd_chain_networkinfo(EnhancedContext& ctx, const std::vector<std::string>& args) {
    try {
        Json::Value result = ctx.client.call("getnetworkinfo", Json::Value());
        Json::Value output = OutputContract::createStableOutput(result, "chain", "networkinfo");
        
        if (ctx.options.redact_sensitive) {
            output = SecurityChecks::redactSensitiveFields(output);
        }
        
        OutputContract::printOutput(output, ctx.options.format, ctx.options.color_output);
        return ErrorHandler::SUCCESS;
        
    } catch (const RpcHttpError& e) {
        ErrorHandler::printError(e.what(), ErrorHandler::mapRpcError(e.code, e.what()), ctx.options.format);
        return ErrorHandler::mapRpcError(e.code, e.what());
    }
}
```

This matrix provides a clear roadmap for achieving 100% RPC parity and closing the obvious gaps in CLI coverage.

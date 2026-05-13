# Dinero RPC Schema v1

This directory contains JSON Schema definitions for Dinero's RPC API. These schemas ensure consistent interfaces across all UI implementations (Qt6, Tauri, web, mobile).

## Schema Version

**Version**: `din.rpc.v1`  
**Status**: Stable (locked for mainnet)  
**Compatibility**: All schemas are backward-compatible within v1.x

## Core Methods

### Blockchain & Network
| Method | Description | Status |
|--------|-------------|---------|
| `getblockchaininfo` | Blockchain status and statistics | ✅ Stable |
| `getnetworkinfo` | Network and peer information | ✅ Stable |
| `getmempoolinfo` | Mempool statistics and fee info | ✅ Stable |
| `getblock` | Block information with verbosity | ✅ Stable |
| `getrawtransaction` | Transaction data with decoding | ✅ Stable |

### Wallet Operations  
| Method | Description | Status |
|--------|-------------|---------|
| `getnewaddress` | Generate new receiving address | ✅ Stable |
| `getbalance` | Wallet balance (confirmed/unconfirmed) | ✅ Stable |
| `listunspent` | UTXO list for coin control | ✅ Stable |
| `listtransactions` | Wallet transaction history | ✅ Stable |
| `validateaddress` | Address validation and info | ✅ Stable |

### PSBT Workflow (Safe Sends)
| Method | Description | Status |
|--------|-------------|---------|
| `psbt.create` | Create PSBT with inputs/outputs | ✅ Stable |
| `psbt.fund` | Add UTXOs, change, calculate fees | ✅ Stable |
| `psbt.sign` | Sign PSBT with wallet keys | ✅ Stable |
| `psbt.submit` | Broadcast signed PSBT | ✅ Stable |
| `estimatesmartfee` | Fee estimation for confirmation target | ✅ Stable |

### Mining
| Method | Description | Status |
|--------|-------------|---------|
| `mining.start` | Start mining operations | ✅ Stable |
| `mining.stop` | Stop mining operations | ✅ Stable |
| `mining.status` | Mining performance metrics | ✅ Stable |

### Wallet Security & Lifecycle
| Method | Description | Status |
|--------|-------------|---------|
| `wallet.encrypt` | Encrypt wallet with passphrase | ✅ Stable |
| `wallet.unlock` | Unlock encrypted wallet | ✅ Stable |
| `wallet.lock` | Lock encrypted wallet | ✅ Stable |
| `wallet.changePassphrase` | Change wallet passphrase | ✅ Stable |
| `getwalletinfo` | Wallet status and encryption info | ✅ Stable |

### Backup & Recovery
| Method | Description | Status |
|--------|-------------|---------|
| `wallet.backup` | Create encrypted wallet backup | ✅ Stable |
| `wallet.restore` | Restore wallet from backup | ✅ Stable |
| `wallet.rescan` | Rescan blockchain for transactions | ✅ Stable |
| `wallet.abortrescan` | Abort running rescan | ✅ Stable |

### Advanced UTXO & Fee Control
| Method | Description | Status |
|--------|-------------|---------|
| `utxo.lock` | Lock/unlock specific UTXOs | ✅ Stable |
| `utxo.listlocks` | List locked UTXOs | ✅ Stable |
| `tx.bumpfee` | Bump transaction fee (RBF) | ✅ Stable |
| `tx.cpfp.create` | Create child-pays-for-parent tx | ✅ Stable |

### Node Operations & Diagnostics
| Method | Description | Status |
|--------|-------------|---------|
| `getpeerinfo` | Connected peer information | ✅ Stable |
| `getconnectioncount` | Number of peer connections | ✅ Stable |
| `health.status` | Comprehensive node health | ✅ Stable |

### Explorer & Deep Analysis
| Method | Description | Status |
|--------|-------------|---------|
| `getrawmempool` | Mempool transactions (verbose) | ✅ Stable |
| `gettxout` | UTXO information | ✅ Stable |
| `gettxoutsetinfo` | UTXO set statistics | ✅ Stable |
| `search` | Universal search (txid/hash/address) | ✅ Stable |

### Real-time Events (WebSocket)
| Method | Description | Status |
|--------|-------------|---------|
| `events.subscribe` | Subscribe to real-time events | ✅ Stable |
| `events.unsubscribe` | Unsubscribe from events | ✅ Stable |

## Currency Units

All monetary values are provided in **dual format**:
- `*_una`: Integer values in una (smallest unit)
- `*_din`: Formatted string values in DIN (human-readable)

**Ratio**: 1 DIN = 1,000,000 una (6 decimal places)

## Usage

### Validation
```bash
# Validate RPC response against schema
ajv validate -s getblockchaininfo.json -d response.json
```

### Code Generation
These schemas can generate client code for:
- TypeScript interfaces
- Rust structs  
- Python dataclasses
- Go structs

### Cross-UI Compatibility

Any UI implementation must:
1. Support all methods in this schema
2. Handle both `*_una` and `*_din` formats
3. Respect enum values and constraints
4. Implement proper error handling

## Network-Specific Behavior

- **Mainnet**: All methods available, send operations require confirmation
- **Testnet**: All methods available, reduced security warnings
- **Regtest**: All methods available, mining enabled by default

## Future Versions

- `din.rpc.v2`: Reserved for post-mainnet enhancements
- Versioning follows semantic versioning within major versions
- Breaking changes require new major version

## Shared Types

- **`_shared/envelope.schema.json`**: Standard JSON-RPC envelope with Dinero extensions
- **`_shared/types.schema.json`**: Common type definitions (Una, TxId, Address, etc.)

## Coverage Analysis

**✅ Complete GUI Support**: These schemas cover **100%** of professional desktop GUI workflows:

### Core Panels (Essential)
- **Status Panel**: `getblockchaininfo`, `getnetworkinfo`, `getmempoolinfo`, `health.status`
- **Wallet Panel**: `getbalance`, `getnewaddress`, `listunspent`, `listtransactions`, `getwalletinfo`
- **Send Panel**: Full PSBT workflow (`psbt.*`, `validateaddress`, `estimatesmartfee`)
- **Mining Panel**: `mining.start`, `mining.stop`, `mining.status`
- **Explorer Panel**: `getblock`, `getrawtransaction`, `search`

### Professional Features (Advanced)
- **Security Tab**: `wallet.encrypt`, `wallet.unlock`, `wallet.lock`, `wallet.changePassphrase`
- **Backup Tab**: `wallet.backup`, `wallet.restore`, `wallet.rescan`, `wallet.abortrescan`
- **Advanced Controls**: `utxo.lock`, `tx.bumpfee`, `tx.cpfp.create`, `utxo.listlocks`
- **Diagnostics Tab**: `getpeerinfo`, `getconnectioncount`, `health.status`
- **Deep Explorer**: `getrawmempool`, `gettxout`, `gettxoutsetinfo`

### Modern UX (Real-time)
- **Live Updates**: `events.subscribe` for blocks, transactions, mining, health
- **WebSocket Events**: Real-time without polling, modern responsive interface

**🔒 Mainnet Safety**: PSBT workflow + wallet encryption + backup system ensures enterprise-grade security

**🎯 Zero CLI Dependency**: GUI handles 100% of workflows - no terminal required**

## Contributing

When adding new RPC methods:
1. Create schema file: `methodname.json`
2. Reference `_shared/types.schema.json` for common types
3. Follow dual currency format (`*_una` + `*_din`)
4. Update this README
5. Ensure backward compatibility
6. Test with existing UI implementations

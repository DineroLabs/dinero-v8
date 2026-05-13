# Dinero RPC vNext - Canonical Naming Convention

**Status:** vNext-Final Migration (2025-11-04)
**Total Methods:** 138
**Schema File:** `docs/rpc_schema_vnext.json`

## Naming Convention

All RPC methods MUST use dotted namespace notation:

```
<category>.<action>
```

### Categories

| Category | Purpose | Example Methods |
|----------|---------|-----------------|
| `blockchain.*` | Chain state, blocks, difficulty | `blockchain.getblockcount`, `blockchain.submitblock` |
| `wallet.*` | Wallet operations, addresses, transactions | `wallet.getbalance`, `wallet.sendtoaddress` |
| `mining.*` | Mining control, hashrate, rewards | `mining.start`, `mining.info`, `mining.gettemplate` |
| `network.*` | P2P connections, peers | `network.getinfo`, `network.addnode` |
| `mempool.*` | Transaction pool | `mempool.getinfo`, `mempool.getraw` |
| `bridge.*` | Cross-chain bridge, fiat rates, ARP | `bridge.getrate`, `bridge.convert`, `bridge.getarp`, `bridge.setarp` |
| `contract.*` | Smart contracts, escrow | `contract.create`, `contract.release` |
| `multiasset.*` | Multi-asset support | `multiasset.stats`, `multiasset.createescrow` |
| `p2p.*` | P2P marketplace | `p2p.createoffer`, `p2p.acceptoffer` |
| `payment.*` | Payment monitoring | `payment.watch`, `payment.status` |
| `auth.*` | Authentication, sessions | `auth.requesttoken`, `auth.whoami` |
| `rpc.*` | RPC introspection | `rpc.discover`, `rpc.info` |
| `server.*` | Server status, health | `server.health`, `server.getinfo` |
| `telemetry.*` | Metrics, monitoring | `telemetry.metrics`, `telemetry.health` |
| `consensus.*` | Consensus verification | `consensus.checkdb` |
| `economics.*` | Supply, economics | `economics.getsupply`, `economics.getminerstats` |
| `hwallet.*` | Hardware wallet, PSBT | `hwallet.exportpsbttofile`, `hwallet.importpsbtfromfile`, `hwallet.analyzepsbt`, `hwallet.enumeratehwdevices` |
| `ws.*` | WebSocket management | `ws.subscribe`, `ws.replay`, `ws.getconnections`, `ws.gettopicstats`, `ws.getstatus` |

## Legacy to vNext Migration Map

### Blockchain Methods
```
getblockcount          → blockchain.getblockcount
getblock               → blockchain.getblock
getblockhash           → blockchain.getblockhash
getbestblockhash       → blockchain.getbestblockhash
getblockheader         → blockchain.getblockheader
getblockchaininfo      → blockchain.getinfo
getdifficulty          → blockchain.getdifficulty
submitblock            → blockchain.submitblock
invalidateblock        → blockchain.invalidateblock
getsupply              → economics.getsupply
```

### Wallet Methods
```
getbalance             → wallet.getbalance
sendtoaddress          → wallet.sendtoaddress
getnewaddress          → wallet.getnewaddress
getwalletinfo          → wallet.getinfo
listaddresses          → wallet.listaddresses
listunspent            → wallet.listunspent
listtransactions       → wallet.listtransactions
sendrawtransaction     → wallet.sendrawtransaction
createrawtransaction   → wallet.createrawtransaction
signrawtransaction...  → wallet.signrawtransactionwithwallet
validateaddress        → wallet.validateaddress
dumpprivkey            → wallet.dumpprivkey
importprivkey          → wallet.importprivkey
encryptwallet          → wallet.encrypt
walletlock             → wallet.lock
walletunlock           → wallet.unlock
walletpassphrase...    → wallet.passphrasechange
walletrescan           → wallet.rescan
backupwallet           → wallet.backup
restorewallet          → wallet.restore
createhdwallet         → wallet.createhd
```

### Mining Methods
```
getmininginfo          → mining.getinfo
getblocktemplate       → mining.gettemplate
setgenerate            → mining.start / mining.stop
generatetoaddress      → mining.generatetoaddress
getminerstats          → economics.getminerstats
```

### Network Methods
```
getnetworkinfo         → network.getinfo
getpeerinfo            → network.getpeerinfo
getconnectioncount     → network.getconnectioncount
addnode                → network.addnode
getserverinfo          → server.getinfo
```

### Mempool Methods
```
getmempoolinfo         → mempool.getinfo
getrawmempool          → mempool.getraw
```

### Server/Health Methods
```
gethealth              → server.health (or telemetry.health)
getmetrics             → telemetry.metrics
getverificationsummary → telemetry.getverificationsummary
getnodeidentity        → telemetry.getnodeidentity
```

## Methods Already Using vNext Naming

These methods are ALREADY correctly named:

- ✅ `mining.start`, `mining.stop`, `mining.info`, `mining.setaddress`
- ✅ `bridge.getrate`, `bridge.convert`, `bridge.status`, `bridge.providers`, `bridge.getarp`, `bridge.setarp`
- ✅ `contract.createescrow`, `contract.status`, `contract.list`, `contract.release`
- ✅ `multiasset.*` (all methods)
- ✅ `p2p.*` (all methods)
- ✅ `payment.*` (all methods)
- ✅ `auth.*` (all methods)
- ✅ `rpc.discover`, `rpc.info`
- ✅ `hwallet.exportpsbttofile`, `hwallet.importpsbtfromfile`, `hwallet.analyzepsbt`, `hwallet.enumeratehwdevices`
- ✅ `ws.subscribe`, `ws.replay`, `ws.getconnections`, `ws.gettopicstats`, `ws.getstatus`
- ✅ `wallet.getsyncstate`, `wallet.getstatus`

## Migration Status

**✅ COMPLETED (2025-11-04):**

All RPC methods now use proper dotted notation! Final cleanup included:

**Phase 1 - Daemon Method Registrations:**
- ✅ Fixed `bridge.getarp` and `bridge.setarp` (was `getarp`, `setarp`)
- ✅ Fixed `wallet.getsyncstate` and `wallet.getstatus` (was `getsyncstate`, `getwalletstatus`)
- ✅ Fixed hardware wallet methods to use `hwallet.*` prefix (was flat names)
- ✅ Fixed WebSocket methods to use `ws.*` prefix (was camelCase flat names)

**Phase 2 - Client Code Updates:**
- ✅ Updated GUI hardware wallet widget (4 method call sites)
- ✅ Updated GUI to match new method names
- ✅ Updated test suite (`tests/test_gui_rpc.py`)

**Phase 3 - Testing:**
- ✅ All 13 renamed methods tested and working
- ✅ Build successful (daemon + GUI)
- ✅ Integration tests passing

**Final State:**
- 0 flat-named methods ✅
- 150+ methods all using `<category>.<action>` format ✅
- All registrations use vNext RpcRegistry pattern ✅

## Client Impact

### CLI
- Update ~30 method calls in `src/cli/commands.cpp`
- Remove hardcoded help; use `rpc.discover`

### Miner
- Update `getblocktemplate` → `mining.gettemplate`
- Update `submitblock` → `blockchain.submitblock`

### Qt GUI
- Update all `RpcClient::call()` invocations in `gui/src/`
- Query `rpc.discover` on startup for dynamic feature toggles

## Verification

After migration, verify with:

```bash
# No legacy flat names in client code
grep -R '"get' src/cli src/miner src/gui | grep -v 'blockchain\.' | grep -v 'wallet\.' | grep -v 'mining\.'

# Daemon exposes only dotted names
dinero-cli rpc.discover | jq '.methods[] | .name' | grep -v '\.'
# (should return empty or only intentional flat names)
```

## Notes

- **No aliases:** For a clean break, we do NOT register legacy flat names as aliases
- **Breaking change:** Old clients will NOT work with vNext-Final daemon
- **Coordinated release:** All binaries (daemon, CLI, GUI, miner) must be updated together
- **Schema as source of truth:** `rpc.discover` output is the canonical API reference

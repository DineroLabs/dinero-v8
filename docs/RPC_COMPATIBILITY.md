# RPC API Compatibility Contract

**Status:** Phase Z.3 - RPC Compatibility Guarantees
**Date:** 2025-12-31
**Objective:** Establish contractual API stability for RPC clients

---

## Philosophy

**"Your integration will not break."**

Every RPC method has a stability level:
- **STABLE**: Guaranteed backward compatible (changes require deprecation cycle)
- **EXPERIMENTAL**: May change in minor versions (not ready for production)
- **DEPRECATED**: Scheduled for removal (replacement method documented)
- **INTERNAL**: Not for external use (may change or be removed without notice)

---

## API Versioning

### Current Version

**api_version:** 1
**Released:** v1.0.0
**Status:** STABLE (frozen for v1.0.x series)

### Version Detection

```bash
# Check API version
dinero-cli rpc.apihash

# Get full capabilities
dinero-cli rpc.capabilities
```

**Response:**
```json
{
  "api_version": 1,
  "api_hash": "sha256:abc123...",
  "server_version": "1.0.0",
  "namespaces": ["wallet", "blockchain", "mining", "mempool", "p2p", "rpc"]
}
```

---

## Stability Levels

### STABLE Methods

**Guarantee:**
- **Method name**: Never renamed (aliases allowed)
- **Required parameters**: Never removed or reordered
- **Optional parameters**: New ones may be added (always at end)
- **Return structure**: Existing fields never removed (new fields may be added)
- **Error codes**: Error codes never change meaning
- **Deprecation cycle**: Minimum 1 major version (announce → deprecate → remove)

**Example:**
```json
// v1.0 (STABLE)
wallet.getbalance() → {"balance": 100.0}

// v1.1 (backward compatible - new optional field)
wallet.getbalance() → {"balance": 100.0, "unconfirmed": 10.0}

// v2.0 (still backward compatible)
wallet.getbalance() → {"balance": 100.0, "unconfirmed": 10.0, "immature": 5.0}
```

### EXPERIMENTAL Methods

**Warning:**
- May change signature in minor versions
- May be renamed or removed
- Not recommended for production integrations
- Clearly marked in documentation

**Example:**
```json
// v1.0 (EXPERIMENTAL)
lightning.experimental.bolt12invoice(params)

// v1.1 (may change)
lightning.bolt12.createinvoice(params)  // renamed
```

**Policy:**
- Experimental methods stabilize after 2 minor releases
- If not stabilized by next major version → removed

### DEPRECATED Methods

**Timeline:**
```
v1.0: Method exists (STABLE)
v1.1: Method deprecated (warning issued, replacement documented)
v2.0: Method removed (only replacement available)
```

**Deprecation Warning:**
```json
{
  "result": {"balance": 100.0},
  "warning": "Method 'getbalance' is deprecated. Use 'wallet.getbalance' instead. Will be removed in v2.0."
}
```

**Required in Deprecation Notice:**
1. Replacement method name
2. Migration example
3. Removal timeline (major version)

### INTERNAL Methods

**Policy:**
- Not documented in public API
- Used only by DineroCoin internal tools
- May change or be removed without notice
- Do not use in external integrations

**Examples:**
- `storage.compactstorage` (internal maintenance)
- `consensus.checkdb` (internal validation)
- `addrman.maintenance` (internal cleanup)

---

## Breaking Change Policy

### What Constitutes a Breaking Change

**Breaking changes (require deprecation cycle):**

1. **Method Removed**
   - **Policy**: Deprecate → 1 major version → remove
   - **Example**: `getinfo` → deprecated v1.1 → removed v2.0

2. **Required Parameter Added**
   - **Policy**: NOT ALLOWED (use optional parameter with sensible default)
   - **Example**: ❌ `sendtoaddress(address)` → `sendtoaddress(address, amount)` (BREAKING)
   - **Example**: ✅ `sendtoaddress(address, amount=0.0)` (backward compatible)

3. **Parameter Reordered**
   - **Policy**: NOT ALLOWED (maintain parameter order)
   - **Example**: ❌ `sendtoaddress(address, amount)` → `sendtoaddress(amount, address)` (BREAKING)

4. **Return Field Removed**
   - **Policy**: NOT ALLOWED (use deprecation warning, keep field for 1 major version)
   - **Example**: ❌ Remove `getinfo.blocks` (BREAKING)
   - **Example**: ✅ Deprecate `getinfo.blocks`, add `blockchain.getblockcount` (backward compatible)

5. **Error Code Changed**
   - **Policy**: NOT ALLOWED (error codes are part of API contract)
   - **Example**: ❌ Change -4 (insufficient funds) to -32 (BREAKING)

6. **Type Changed**
   - **Policy**: NOT ALLOWED (field types must remain stable)
   - **Example**: ❌ `balance` from `number` to `string` (BREAKING)

**Non-breaking changes (allowed):**

1. **New Optional Parameter**
   - **Policy**: ALLOWED (always at end, sensible default)
   - **Example**: ✅ `getblock(hash)` → `getblock(hash, verbosity=2)`

2. **New Return Field**
   - **Policy**: ALLOWED (clients ignore unknown fields)
   - **Example**: ✅ `getinfo` → add `getinfo.version` field

3. **New Method**
   - **Policy**: ALLOWED (does not affect existing methods)

4. **Method Aliasing**
   - **Policy**: ALLOWED (canonical name + aliases)
   - **Example**: ✅ `getinfo` → alias for `blockchain.getinfo`

---

## Namespace Organization

### Canonical Namespaces (v1.0)

**Core Namespaces:**
- `blockchain.*` - Blockchain queries (blocks, chain state)
- `wallet.*` - Wallet operations (addresses, transactions, balances)
- `mining.*` - Mining control (start, stop, templates)
- `mempool.*` - Mempool queries (pending transactions)
- `p2p.*` - P2P network (peers, sync status)
- `rpc.*` - RPC introspection (help, methods, capabilities)

**Extended Namespaces:**
- `ln.*` - Lightning Network operations
- `zk.*` - Zero-knowledge operations (stealth addresses, confidential transactions)
- `multiaccount.*` - Multi-account wallet operations
- `auth.*` - Authentication and session management
- `logging.*` - Log configuration and retrieval

**Internal Namespaces:**
- `storage.*` - Internal storage maintenance
- `consensus.*` - Internal consensus validation
- `addrman.*` - Internal address manager

### Flat Method Aliases

**Policy:** Flat aliases maintained forever for backward compatibility.

**Example:**
```bash
# v1.0 (both valid)
dinero-cli getinfo
dinero-cli blockchain.getinfo

# v1.1 (both still valid)
dinero-cli getinfo
dinero-cli blockchain.getinfo
```

**Canonical vs Alias:**
- **Canonical**: `blockchain.getinfo` (namespaced)
- **Alias**: `getinfo` (flat, backward compatible)

**Guarantee:** Flat aliases never removed.

---

## JSON-RPC 2.0 Compliance

DineroCoin RPC server implements **JSON-RPC 2.0** specification.

### Standard Error Codes

**Reserved JSON-RPC 2.0 error codes:**
```json
{
  "-32700": "Parse error (invalid JSON)",
  "-32600": "Invalid request (malformed JSON-RPC)",
  "-32601": "Method not found",
  "-32602": "Invalid parameters",
  "-32603": "Internal error"
}
```

**DineroCoin-specific error codes:**
```json
{
  "-1": "Miscellaneous error",
  "-2": "Invalid address or key",
  "-3": "RPC server in safe mode",
  "-4": "Insufficient funds",
  "-5": "Invalid parameter",
  "-6": "Wallet locked",
  "-7": "Wallet unlock needed",
  "-8": "Invalid transaction",
  "-10": "Node not connected",
  "-20": "Database error",
  "-25": "Wallet already loaded",
  "-26": "Wallet not found",
  "-27": "Wallet already exists"
}
```

**Error Code Guarantee:**
- Error codes never change meaning
- New error codes may be added (always negative integers)
- Deprecated error codes kept for 1 major version

---

## Authentication

### Cookie-Based Authentication

**File:** `~/.dinero/.cookie`
**Format:** `username:password` (generated on daemon start)

**Example:**
```bash
# Automatic (dinero-cli reads cookie)
dinero-cli getblockchaininfo

# Manual (for custom integrations)
curl --user "$(cat ~/.dinero/.cookie)" \
     --data-binary '{"jsonrpc":"2.0","id":"1","method":"blockchain.getinfo","params":[]}' \
     http://127.0.0.1:20998/
```

### Static Credentials

**File:** `~/.dinero/dinero.conf`
**Format:**
```conf
rpcuser=your_username
rpcpassword=your_password
```

**Example:**
```bash
curl --user "your_username:your_password" \
     --data-binary '{"jsonrpc":"2.0","id":"1","method":"blockchain.getinfo","params":[]}' \
     http://127.0.0.1:20998/
```

### Session Tokens (EXPERIMENTAL - v1.1+)

**Status:** EXPERIMENTAL
**Available:** v1.1.0+

**Request token:**
```bash
dinero-cli auth.requesttoken --label "MyApp"
```

**Response:**
```json
{
  "token": "eyJhbGc...",
  "expires": 1704067200,
  "label": "MyApp"
}
```

**Use token:**
```bash
curl -H "Authorization: Bearer eyJhbGc..." \
     --data-binary '{"jsonrpc":"2.0","id":"1","method":"blockchain.getinfo","params":[]}' \
     http://127.0.0.1:20998/
```

---

## WebSocket API (EXPERIMENTAL)

**Status:** EXPERIMENTAL (v1.0)
**Endpoint:** `ws://127.0.0.1:20998/ws`

**Subscribe to events:**
```json
{
  "method": "ws.subscribe",
  "params": {
    "topic": "blocks"
  }
}
```

**Event types:**
- `blocks` - New block notifications
- `transactions` - New transaction notifications
- `mempool` - Mempool change notifications
- `mining` - Mining status updates

**Stability Warning:**
- WebSocket API is EXPERIMENTAL in v1.0
- May change in v1.1 without deprecation cycle
- Production use not recommended until v1.1 STABLE

---

## OpenRPC Introspection

DineroCoin implements **OpenRPC 1.3.2** for machine-readable API documentation.

### Get Full Schema

```bash
dinero-cli rpc.openrpc
```

**Response:** Complete OpenRPC 1.3.2 schema with all methods, parameters, return types.

### Get Method Schema

```bash
dinero-cli rpc.schema --method "wallet.getbalance"
```

**Response:**
```json
{
  "name": "wallet.getbalance",
  "params": [
    {"name": "minconf", "type": "integer", "required": false, "default": 1}
  ],
  "result": {
    "type": "object",
    "properties": {
      "balance": {"type": "number"}
    }
  }
}
```

### Get Methods by Namespace

```bash
dinero-cli rpc.namespaces
```

**Response:**
```json
{
  "wallet": ["wallet.getbalance", "wallet.sendtoaddress", ...],
  "blockchain": ["blockchain.getinfo", "blockchain.getblock", ...],
  "mining": ["mining.start", "mining.stop", ...]
}
```

---

## API Hash Verification

**Purpose:** Detect API changes between versions.

**Get API hash:**
```bash
dinero-cli rpc.apihash
```

**Response:**
```json
{
  "api_version": 1,
  "api_hash": "sha256:0a1b2c3d4e5f...",
  "methods_count": 339,
  "stable_methods_count": 287,
  "experimental_methods_count": 42,
  "deprecated_methods_count": 10
}
```

**Hash includes:**
- All STABLE method names
- All STABLE method parameter signatures
- All STABLE method return types
- All error codes

**Hash excludes:**
- EXPERIMENTAL methods (may change)
- INTERNAL methods (not part of public API)
- Documentation strings
- Optional parameter defaults

**Use case:**
```bash
# Integration test: Verify API has not changed
EXPECTED_HASH="sha256:0a1b2c3d4e5f..."
ACTUAL_HASH=$(dinero-cli rpc.apihash | jq -r '.api_hash')

if [ "$EXPECTED_HASH" != "$ACTUAL_HASH" ]; then
  echo "API has changed! Review release notes."
  exit 1
fi
```

---

## Deprecation Process

### Step 1: Announcement (N - 1 major version)

**v1.0:** Method exists (STABLE)
- `getinfo` → returns blockchain info

### Step 2: Deprecation (N major version)

**v1.1:** Method deprecated
- `getinfo` → deprecated, use `blockchain.getinfo`
- Warning issued in response
- Migration guide published

**Deprecation response:**
```json
{
  "result": {"blocks": 12345, "difficulty": 1.0},
  "warning": "Method 'getinfo' is deprecated and will be removed in v2.0. Use 'blockchain.getinfo' instead."
}
```

**Migration guide:**
```bash
# Old (deprecated)
dinero-cli getinfo

# New (recommended)
dinero-cli blockchain.getinfo
```

### Step 3: Removal (N + 1 major version)

**v2.0:** Method removed
- `getinfo` → error -32601 (method not found)
- Only `blockchain.getinfo` available

**Removal timeline:** Minimum 1 major version (12+ months).

---

## Backward Compatibility Guarantees

### v1.0.x Series

**Frozen:** All STABLE methods frozen for v1.0.x series.

**Allowed changes:**
- ✅ New optional parameters (at end)
- ✅ New return fields
- ✅ New EXPERIMENTAL methods
- ✅ Bug fixes (without changing behavior)

**NOT allowed:**
- ❌ Breaking changes to STABLE methods
- ❌ Removing STABLE methods
- ❌ Removing required parameters
- ❌ Changing error codes

### v1.1.x Series (Future)

**Policy:** Backward compatible with v1.0.x.

**Changes allowed:**
- Stabilize EXPERIMENTAL methods
- Add new methods
- Deprecate old methods (not remove)
- Add new namespaces

**Breaking changes:** None (deferred to v2.0).

### v2.0 (Future)

**Policy:** Breaking changes allowed.

**Changes allowed:**
- Remove deprecated methods (deprecated in v1.x)
- Change default values (documented in release notes)
- Restructure namespaces (aliases maintained)

**Guarantee:** All deprecations announced in v1.x.

---

## Client Library Recommendations

### Version Pinning

**Recommended:** Pin client library to specific API version.

**Example (Python):**
```python
from dinerocoin import RpcClient

client = RpcClient(
    url="http://127.0.0.1:20998",
    api_version=1  # Pin to API v1
)

# Raises error if API version mismatch
client.connect()
```

### Capability Detection

**Recommended:** Use `rpc.capabilities` to detect features.

**Example (Python):**
```python
capabilities = client.call("rpc.capabilities")

if "lightning" in capabilities["namespaces"]:
    # Lightning methods available
    ln_info = client.call("ln.listchannels")
else:
    # Lightning not available
    print("Lightning Network not enabled")
```

### Graceful Degradation

**Recommended:** Handle missing methods gracefully.

**Example (Python):**
```python
try:
    result = client.call("wallet.getbalance")
except MethodNotFoundError:
    # Fallback to older method
    result = client.call("getbalance")
```

---

## Testing API Compatibility

### Integration Test Example

```bash
#!/bin/bash
# Test API compatibility

# 1. Check API version
API_VERSION=$(dinero-cli rpc.capabilities | jq -r '.api_version')
if [ "$API_VERSION" != "1" ]; then
  echo "ERROR: Expected API version 1, got $API_VERSION"
  exit 1
fi

# 2. Check API hash (detect unexpected changes)
API_HASH=$(dinero-cli rpc.apihash | jq -r '.api_hash')
EXPECTED_HASH="sha256:0a1b2c3d4e5f..."
if [ "$API_HASH" != "$EXPECTED_HASH" ]; then
  echo "WARNING: API hash changed. Review release notes."
fi

# 3. Test critical methods
dinero-cli blockchain.getinfo || exit 1
dinero-cli wallet.getbalance || exit 1
dinero-cli mining.getinfo || exit 1

echo "API compatibility: OK"
```

---

## Migration Guides

### Migrating from Flat to Namespaced Methods

**Old (flat aliases):**
```bash
dinero-cli getinfo
dinero-cli getbalance
dinero-cli getmininginfo
```

**New (namespaced):**
```bash
dinero-cli blockchain.getinfo
dinero-cli wallet.getbalance
dinero-cli mining.getinfo
```

**Both valid forever:** Flat aliases never removed.

### Migrating from v1.0 to v1.1 (Future)

**Step 1:** Check deprecation warnings
```bash
dinero-cli getinfo 2>&1 | grep -i "deprecated"
```

**Step 2:** Update method calls
```bash
# Replace deprecated methods
getinfo → blockchain.getinfo
getbalance → wallet.getbalance
```

**Step 3:** Test on testnet
```bash
dinerod --testnet
dinero-cli --testnet blockchain.getinfo
```

**Step 4:** Deploy to mainnet
```bash
# Stop old daemon
dinero-cli stop

# Install new version
sudo cp dinerod-v1.1.0 /usr/local/bin/dinerod

# Start new daemon
dinerod

# Verify
dinero-cli rpc.capabilities
```

---

## Audit Trail

Phase Z.3 establishes **RPC API compatibility guarantees**:

1. **Phase D** - Consensus frozen ✅
2. **Phase E** - Safety infrastructure ✅
3. **Phase Z.1** - Reproducible builds ✅
4. **Phase Z.2** - Configuration guarantees ✅
5. **Phase Z.3** - RPC compatibility contract ← **YOU ARE HERE** 🔨

**Next:** Phase Z.4 (Release Checklist)

---

**Phase Z.3: RPC Compatibility Contract - Complete**

**Integration guarantee:**
- ✅ API versioning established (api_version=1)
- ✅ Stability levels defined (STABLE, EXPERIMENTAL, DEPRECATED, INTERNAL)
- ✅ Breaking change policy documented
- ✅ Deprecation cycle: 1 major version
- ✅ Backward compatibility guaranteed for v1.0.x
- ✅ OpenRPC introspection available
- ✅ Error codes frozen
- ✅ Migration paths documented

**Your integration will not break.**

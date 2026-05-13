# RPC Naming Policy

This document defines the naming conventions and policies for DineroCoin RPC methods.

## Canonical RPCs

All **canonical RPC methods** MUST be namespaced using the following structure:

- `blockchain.*` - Blockchain queries, mining, and chain operations
- `mempool.*` - Mempool queries, transaction submission, and fee estimation
- `wallet.*` - Wallet operations, address management, and transaction signing
- Other namespaces as needed (e.g., `network.*`, `utreexo.*`, `lightning.*`)

### Examples
```
blockchain.getblockcount
mempool.getinfo
wallet.getbalance
```

### Rule
**All new RPCs MUST be namespaced.** Flat Bitcoin-style names are forbidden for new methods.

## Legacy RPCs

**Legacy RPC methods** are flat Bitcoin-style names inherited for backward compatibility:

```
getblockcount
getmempoolinfo
sendtoaddress
```

These methods are **supported via aliases only** — they resolve to canonical namespaced methods through the alias registry.

### Rule
**No new flat RPCs allowed.** Legacy names exist solely for backward compatibility.

## Aliases

The alias system provides zero-risk backward compatibility:

### Principles
- **One canonical method** - Each operation has exactly one namespaced implementation
- **At most one legacy alias** - No proliferation of alternative names
- **Same handler** - Aliases resolve to the canonical method's handler
- **No logic duplication** - Canonical and alias execute identical code paths

### Architecture
```
getmempoolinfo  ─┐
                 ├─ alias resolution ──► mempool.getinfo handler ──► MempoolService
mempool.getinfo ─┘
```

### Validation
Aliases MUST:
- Point to an existing canonical method
- Map to the same handler (enforced by registry)
- Not create circular references
- Not override canonical methods

## Deprecation Policy

### Current Status (v1.x)
- **All aliases are fully supported** with no warnings or restrictions
- No removal timeline has been set
- Aliases are considered a **stable backward compatibility layer**

### Future Considerations (v2.0+)
Alias removal will only be considered for major version bumps (v2.0+) and requires:
- Clear migration documentation
- Advance notice to users and integrators
- Consensus among core contributors

**Note:** It is acceptable to maintain aliases indefinitely (Bitcoin Core does this).

## Implementation

### Registering a Canonical Method
```cpp
g_rpcRegistry.registerHandler("mempool.getinfo",
                             rpc_mempool_getinfo,
                             RegisterMode::Overwrite,
                             "mempool");
```

### Registering an Alias
```cpp
// Always register alias AFTER canonical method
g_rpcRegistry.registerAlias("getmempoolinfo", "mempool.getinfo");
```

### Introspection
The registry provides metadata for tooling and help output:
```cpp
bool isAlias = g_rpcRegistry.isAlias("getmempoolinfo");  // true
auto info = g_rpcRegistry.getAliasInfo("getmempoolinfo");
// info->canonical_name = "mempool.getinfo"
// info->message = "Use mempool.getinfo"
```

## Rationale

### Why Namespaces?
- **Clarity** - `wallet.getbalance` is unambiguous
- **Scalability** - Avoids name collisions as RPC surface grows
- **Organization** - Groups related methods logically
- **Professionalism** - Modern API design standard

### Why Keep Aliases?
- **Stability** - No breaking changes for existing users/tools
- **Compatibility** - CLI, GUI, scripts, and bots continue working
- **Migration** - Users can transition at their own pace

### Why One Canonical Name?
- **Single source of truth** - No ambiguity in documentation
- **Maintainability** - Changes happen in one place
- **Testability** - One code path to verify

## Examples

### ✅ Correct
```cpp
// Canonical method
g_rpcRegistry.registerHandler("blockchain.getblock", handler, ...);

// Legacy alias
g_rpcRegistry.registerAlias("getblock", "blockchain.getblock");
```

### ❌ Incorrect
```cpp
// DON'T: Create new flat methods
g_rpcRegistry.registerHandler("getnewthing", handler, ...);

// DON'T: Create multiple aliases
g_rpcRegistry.registerAlias("getinfo", "blockchain.getinfo");
g_rpcRegistry.registerAlias("info", "blockchain.getinfo");  // ❌

// DON'T: Point aliases to different handlers
g_rpcRegistry.registerHandler("getinfo", handler1, ...);
g_rpcRegistry.registerAlias("info", "othermethod");  // ❌
```

## Summary

| Aspect | Policy |
|--------|--------|
| **New RPCs** | MUST be namespaced |
| **Legacy names** | Aliases only, fully supported |
| **Aliases** | One per canonical method, same handler |
| **Deprecation** | None planned for v1.x |
| **Breaking changes** | Only in major versions (v2.0+) |

---

**Last Updated:** December 2025
**Status:** Active Policy (v1.0+)

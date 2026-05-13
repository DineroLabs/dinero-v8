# Dinero C++ Daemon - Tier-1 (Authoritative)

## What This Repo IS

- **The authority** - defines consensus rules
- **Fork choice** - decides which chain is valid
- **Block validation** - accepts or rejects blocks
- **Network protocol** - P2P message handling
- **Mining coordination** - block production

## What This Repo is NOT

- ❌ NOT verification-only code (that's Tier-3)
- ❌ NOT wallet UI code (that's apps/)
- ❌ NOT Lightning (separate repo)

## The Rule

**Never add another language runtime here.**

C++ Tier-1 may *consume* outputs (RPC, files, libs).
It must *never contain* Rust, Lightning, or wallet UI code.

## Related Repositories

| Repo | Purpose | Authority |
|------|---------|-----------|
| `dinero` (this) | Consensus daemon | **AUTHORITATIVE** |
| `dinero-rust` | Verification service | Non-authoritative |
| `lightning` | Payment channels | Layer 2 |

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j8
```

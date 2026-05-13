# Dynamic Port Configuration System

## Problem Solved

Previously, P2P ports were hardcoded in multiple places:
- `src/consensus/chainparams_impl.cpp` → port 20999
- `include/config/seed_nodes.h` → port 19003 ❌ **WRONG**
- DNS resolution code → hardcoded fallback ports

This caused **peer connection failures** because:
1. The daemon would try to connect to `173.249.195.59:19003` (wrong port)
2. The production server actually listens on `173.249.195.59:20999`
3. Result: 0 peers, GUI shows "No connections"

---

## Solution: Centralized Port Configuration

**Single Source of Truth:** `src/consensus/chainparams_impl.cpp`

```cpp
static ChainParams g_mainnet = {
    .p2p_port = 20999,     // ✅ ONLY place where mainnet P2P port is defined
    // ...
};

static ChainParams g_testnet = {
    .p2p_port = 21000,     // ✅ ONLY place where testnet P2P port is defined
    // ...
};

static ChainParams g_regtest = {
    .p2p_port = 21001,     // ✅ ONLY place where regtest P2P port is defined
    // ...
};
```

---

## How It Works

### 1. Seed Node Storage (No Ports)

`include/config/seed_nodes.h` now stores **only IP addresses**:

```cpp
const std::vector<SeedNode> MAINNET_SEED_IPS = {
    {"172.93.160.131", 0, "us-west", true},   // Port = 0 (placeholder)
    {"173.249.195.59", 0, "us-east", true},   // Port = 0 (placeholder)
};
```

### 2. Dynamic Port Resolution at Runtime

`src/daemon/main.cpp` sets ports from `ChainParams`:

```cpp
// Select chain parameters first
dinero::SelectParams(dinero::Chain::MAINNET);
const auto& params = dinero::Params();

// Get seed nodes (with port=0)
auto seeds = dinero::config::getSeedNodes("mainnet");

// Set port dynamically from ChainParams
for (auto& seed : seeds) {
    seed.port = params.p2p_port;  // ✅ 20999 from ChainParams
}

// Now resolve DNS and connect
auto resolved = DNSResolver::resolve(seed.hostname, seed.port);
```

### 3. DNS Seed Resolution

```cpp
// OLD (hardcoded):
auto resolved_addrs = DNSResolver::resolveSeeds(
    dns_seeds,
    config.testnet ? 13999 : 23999  // ❌ Hardcoded
);

// NEW (dynamic):
auto resolved_addrs = DNSResolver::resolveSeeds(
    dns_seeds,
    params.p2p_port  // ✅ From ChainParams
);
```

---

## Benefits

1. **Single Source of Truth**: Port defined once in `chainparams_impl.cpp`
2. **No Port Drift**: Impossible for seed nodes to have wrong ports
3. **Network-Aware**: Automatic switching (mainnet=20999, testnet=21000, regtest=21001)
4. **Easy to Change**: Update one line in ChainParams, affects entire system
5. **GUI Compatibility**: Works seamlessly with `network.getpeerinfo` RPC

---

## Testing

```bash
# Build with new dynamic port system
cmake --build build --target dinerod -j8

# Test mainnet connections
./build/dinerod --datadir=/tmp/test-peers -daemon

# Wait 10 seconds, then check peers
sleep 10
./build/dinero-cli -datadir=/tmp/test-peers network.getpeerinfo

# Expected output:
# [
#   {
#     "addr": "173.249.195.59:20999",  // ✅ Correct port!
#     "connected": true
#   }
# ]
```

---

## Code Locations

| File | Purpose | Port Source |
|------|---------|-------------|
| `src/consensus/chainparams_impl.cpp` | **Port definition** | Single source of truth |
| `include/config/seed_nodes.h` | Seed IP addresses | Port = 0 (placeholder) |
| `src/daemon/main.cpp` | **Runtime port resolution** | `params.p2p_port` |
| `gui/src/mainwindow.cpp` | RPC `network.getpeerinfo` | Reads from daemon |

---

## Future Improvements

1. **DNS SRV Records**: Use DNS SRV for dynamic port discovery
   ```
   _dinero._tcp.seed1.dinero-coin.com → 20999
   ```

2. **Port Override**: Allow `--seednode=IP:PORT` to override default

3. **Multiple Ports**: Support nodes listening on multiple ports (IPv4/IPv6)

---

## Migration Notes

If you have **old datadirs** with cached peers using wrong ports:

```bash
# Clear old peer cache
rm ~/.dinero/peers.dat

# Restart daemon (will use new correct ports)
dinerod
```

---

**Status**: ✅ Implemented (2025-11-05)
**Impact**: Fixes "0 peers" issue in GUI
**Breaking**: No (backward compatible - just fixes bug)

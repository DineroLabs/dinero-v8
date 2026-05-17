# Dinero Chain Parameters

## ⚠️ IMMUTABLE GENESIS VALUES - DO NOT MODIFY

These values define the Dinero mainnet genesis block. **Any modification requires a complete network reset.**

### Genesis Block (Height 0)

```
Block Hash:   f01568ded692203556e0cc8a6c14a2bf3eea141edef2200bc7670276c78d52aa
Merkle Root:  b9ddc343101ae7fa6d57776900e30fc692358341c82d6f9b2d0e64f26483f027
Timestamp:    1760472333 (2025-10-14 14:05:33 UTC)
nVersion:     1
nBits:        0x1d3fffff (CPU-friendly difficulty)
nNonce:       0
Motto:        "Dinero: Real Money for Free People - Genesis Block 2025"
```

### Genesis Coinbase Transaction

```
TXID:         b9ddc343101ae7fa6d57776900e30fc692358341c82d6f9b2d0e64f26483f027
Version:      1
Inputs:       1 (coinbase)
Outputs:      3
Locktime:     0
```

**Output 0:** 0 DIN (OP_RETURN burn)
- `OP_RETURN "Dinero Genesis Burn"`

**Output 1:** 99 DIN
- Test output for initial distribution

**Output 2:** 1,000,000 DIN (Premine)
- Address: `din1q0gqj8ush5026e5q97jkw2mhxg3sngjjlxcpjzn`
- Script: P2WPKH `0014 7e0027e0e55eaacd520b5792d6dc61a104649393`

### Genesis Coinbase Hex (Canonical)

```
01000000010000000000000000000000000000000000000000000000000000000000000000
ffffffff39003744696e65726f3a205265616c204d6f6e657920666f7220467265652050
656f706c65202d2047656e6573697320426c6f636b2032303235ffffffff030000000000
000000156a1344696e65726f2047656e65736973204275726e0003164e020000000200ac
00407a10f35a00001600147e0027e0e55eaacd520b5792d6dc61a10464939300000000
```

## Network Parameters

### Mainnet

```
Name:                mainnet
HRP:                 din
Magic Bytes:         0xd9b4bef9
RPC Port:            20997
HTTP API Port:       8080
WebSocket Port:      8081
P2P Port:            20999
```

### Consensus Rules

```
PoW Algorithm:       SHA256d
Target Spacing:      600 seconds (10 minutes)
Difficulty Adjust:   Every 2016 blocks (~2 weeks)
PoW Limit:           0x1d00ffff
Max Block Size:      1,000,000 bytes (1 MB)
Dust Threshold:      546 una
Min Relay Fee:       1000 sat/kB
Coinbase Maturity:   100 blocks
```

### Supply Schedule

```
Total Supply:        97,850,000 DIN
Phase 1:             18,000,000 DIN (initial emission)
Phase 2:             80,000,000 DIN (halving every 800,000 blocks)
Genesis Premine:     1,000,000 DIN
```

## Verification

### At Runtime

The daemon performs these checks on startup:

1. **Link-time sentinel:** `chainparams_impl.cpp` must be compiled
2. **Runtime tripwire:** `genesisCoinbaseHex` must not be empty
3. **Transaction count:** Genesis block must have exactly 1 transaction
4. **Merkle validation:** Coinbase TXID must equal merkle root
5. **Hash validation:** Block hash must match hardcoded value

### Manual Verification

```bash
# Verify genesis hash
./dinero-cli getblockhash 0
# Expected: f01568ded692203556e0cc8a6c14a2bf3eea141edef2200bc7670276c78d52aa

# Verify genesis block
./dinero-cli getblock f01568ded692203556e0cc8a6c14a2bf3eea141edef2200bc7670276c78d52aa 2

# Verify premine UTXO (after maturity)
./dinero-cli gettxout b9ddc343101ae7fa6d57776900e30fc692358341c82d6f9b2d0e64f26483f027 2

# Verify total supply
./dinero-cli gettxoutsetinfo
```

### Expected Startup Output

```
[GENESIS OK] hash=f01568ded692203556e0cc8a6c14a2bf3eea141edef2200bc7670276c78d52aa
[GENESIS OK] merkle=b9ddc343101ae7fa6d57776900e30fc692358341c82d6f9b2d0e64f26483f027
[GENESIS OK] txid=b9ddc343101ae7fa6d57776900e30fc692358341c82d6f9b2d0e64f26483f027
[GENESIS OK] vtx=1
```

## Reproducible Builds

### Build Environment

```
Compiler:      AppleClang 17.0.0.17000013
CMake:         3.20+
C++ Standard:  C++17
Platform:      macOS (Darwin 24.6.0)
Build Type:    RelWithDebInfo
```

### Build Commands

```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

### Verification

```bash
# Check linker sentinel
strings build/dinerod | grep 'chainparams_impl:'
# Expected: chainparams_impl: src/consensus/chainparams_impl.cpp

# Verify genesis hash at runtime
./build/dinerod --version
./build/dinero-cli getblockhash 0
```

## Security Notes

1. **Never modify:** Genesis coinbase hex, motto, timestamps, or difficulty
2. **Never disable:** Any of the 5 validation tripwires
3. **Always verify:** Genesis hash after any chainparams changes
4. **CI enforcement:** Automated test must verify `getblockhash 0` matches

## Historical Record

- **Genesis Mined:** 2025-10-14 14:05:33 UTC
- **Network Launch:** TBD
- **Repository Tag:** `genesis-v1`
- **Commit:** TBD (tag after freeze)

---

**⚠️ WARNING:** Modifying any value in this document requires a complete network reset and new genesis block. All existing blocks and transactions will be invalidated.

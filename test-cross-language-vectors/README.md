# Cross-Language Test Vectors

Canonical test vectors ensuring bit-for-bit compatibility across all DineroCoin implementations.

## Purpose

These vectors are the **single source of truth** for deterministic behavior. Every implementation (Rust DPI, C++ daemon, Swift wallet, WASM) MUST produce identical results for identical inputs.

If your implementation disagrees with these vectors, your implementation is wrong.

## Directory Structure

```
test-cross-language-vectors/
├── v1/                          # Version 1 vectors (IMMUTABLE)
│   ├── serialization.json       # Transaction encoding, txid/wtxid, weights
│   ├── fees.json                # Fee calculation, dust thresholds
│   ├── coin_selection.json      # UTXO selection policies
│   ├── descriptors.json         # BIP86 address derivation
│   └── scanner.json             # Wallet block scanning
└── README.md
```

## Versioning Rules

### v1 is IMMUTABLE

Once merged, vectors in `v1/` **never change**. This guarantees:
- Old implementations remain valid
- No silent breaking changes
- Clear upgrade path via new versions

### Adding New Vectors

To add new test cases:
1. Add to existing `v1/*.json` files if extending coverage
2. Create `v2/` only if semantics change (rare)

### Breaking Changes

If consensus rules change (hard fork), create a new version directory:
```
v2/                              # Post-fork vectors
├── serialization.json
└── ...
```

## Test Seed

All vectors use a deterministic 64-byte seed:

```
000102030405060708090a0b0c0d0e0f
101112131415161718191a1b1c1d1e1f
202122232425262728292a2b2c2d2e2f
303132333435363738393a3b3c3d3e3f
```

**WARNING**: This seed is for testing only. Never use in production.

## Consuming Vectors

### Rust (DPI)

```rust
// Vectors are validated in crates/dpi/tests/cross_language_vectors.rs
cargo test --test cross_language_vectors
```

### C++ (Daemon)

```cpp
#include <nlohmann/json.hpp>
#include <fstream>

std::ifstream f("test-cross-language-vectors/v1/serialization.json");
auto vectors = nlohmann::json::parse(f);

for (auto& tx : vectors["transactions"]) {
    auto result = compute_txid(tx["tx_hex"]);
    assert(result == tx["txid"]);
}
```

### Swift (iOS Wallet)

```swift
import Foundation

let url = Bundle.main.url(forResource: "descriptors", withExtension: "json")!
let data = try! Data(contentsOf: url)
let vectors = try! JSONDecoder().decode(DescriptorVectors.self, from: data)

for addr in vectors.addressDerivation[0].addresses {
    let derived = descriptor.deriveAddress(at: addr.index)
    XCTAssertEqual(derived, addr.address)
}
```

### TypeScript/WASM

```typescript
import vectors from './test-cross-language-vectors/v1/fees.json';

for (const test of vectors.fee_estimates) {
  const result = estimateFee(test.template, test.fee_rate_una_per_vb);
  expect(result.fee).toBe(test.expected_fee);
}
```

## Vector File Format

Each JSON file contains:
- `$schema`: JSON Schema reference
- `version`: Semantic version (e.g., "1.0.0")
- `description`: Human-readable purpose
- Test cases with inputs and expected outputs
- `notes`: Implementation guidance

## Key Constants

| Constant | Value | Description |
|----------|-------|-------------|
| Coin Type | 1447 | SLIP-0044 registered for DineroCoin |
| Dust Threshold | 546 una | Minimum non-dust output |
| Gap Limit | 20 | BIP44 standard address gap |
| 1 DIN | 100,000,000 una | Smallest unit conversion |

## Validation

Run the Rust validation suite:

```bash
cd crates/dpi
cargo test --test cross_language_vectors
```

All 24 tests must pass before any release.

# Contributing to DineroCoin

## 🔧 Build Tips

### Build in "fail-complete" mode (recommended)

When fixing build errors—especially after large refactors or cross-platform changes—use **fail-complete** mode to surface *all* compilation errors in one run:

```bash
cmake --build build --parallel -- -k
```

**Why this matters:**
- Reports all missing includes and errors at once
- Avoids CI "whack-a-mole" (fix → push → fail → repeat)
- Matches the GitHub Actions CI configuration
- Saves significant time during development

**Tip:** Always prefer `-k` when resolving build failures locally so your fix passes CI on the first push.

---

## Header Hygiene Rules

If a file uses:
- `std::string` → `#include <string>`
- `std::vector` → `#include <vector>`
- `std::array` → `#include <array>`
- `std::optional` → `#include <optional>`
- `std::queue` → `#include <queue>`
- `std::sort/min/max/find/reverse` → `#include <algorithm>`
- `std::runtime_error` → `#include <stdexcept>`
- `uint*_t` → `#include <cstdint>`
- `memcpy/strlen/strchr` → `#include <cstring>`

**Do not rely on transitive includes.** If you use a symbol, include its header in the same file.

## Placeholder Policy

Use clear macros and compile-time guards:

```cpp
#include "dinero/core/todo.h"

// For placeholders that need implementation
auto Miner::enableTurbo() -> void { DIN_TODO("implement Miner::enableTurbo"); }

// For optional features
void UtxoDb::open(...) {
  DIN_REQUIRE_ROCKSDB();
  // real code when enabled
}
```

## Platform Abstraction

- **Never call OS APIs directly** from core code
- **Use platform interfaces** in `include/dinero/platform/`
- **Add new platform code** under `src/platform/{posix,windows,apple}/`

## JSON Usage

Use the adapter layer instead of direct library calls:

```cpp
#include "dinero/compat/json.h"

djson::value v = djson::parse(str);
std::string result = djson::stringify(v);
```

## Core is Qt-Free

- **No Qt types** in `src/core/`, `src/daemon/`, or `include/dinero/core/`
- **Qt code only** in `src/gui/` and `include/dinero/gui/`
- **Run audit script**: `bash scripts/audit_no_qt.sh`

---

## Architecture Invariants

All PRs touching **consensus**, **state representation**, **privacy**, or **off-chain protocols** must be reviewed against:

**[docs/architecture/layered_feature_compatibility.md](./architecture/layered_feature_compatibility.md)**

### Layer Separation Rules

1. **Lower layers never trust higher layers**
2. **Higher layers never weaken lower layers**

### Prohibited Patterns

- ❌ ZK proofs as consensus validation authorities
- ❌ Snapshots/accumulators implying UTXO validity
- ❌ Off-chain protocols trusting unverified state
- ❌ Wallet optimizations bypassing script evaluation

**PRs violating layer boundaries will be rejected.**

### Architecture Compliance Checklist

Before submitting protocol changes, verify:
- [ ] Does not allow higher layers to bypass consensus validation
- [ ] Does not introduce implicit trust in proofs/snapshots
- [ ] Maintains independent verifiability of all consensus rules
- [ ] Preserves script evaluation as the sole spending authority

See [docs/architecture/](./architecture/) for complete architectural documentation.

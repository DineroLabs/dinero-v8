# ONE DB Definition — Architectural Invariant

**Status**: Authority Contract (Non-Negotiable)
**Created**: 2025-12-20
**Purpose**: Enforce single database authority

---

## The Invariant

**There is exactly ONE chain database in the entire process.**

It is constructed once, owned once, and passed explicitly.

Anything else is a compile-time violation.

---

## The Seven Questions

### 1. What is it called?

**`ChainDB`**

This is the canonical name. No aliases, no wrappers, no "ChainDB2" or "ChainDBAdapter".

Any code that needs blockchain state queries or mutates it references `ChainDB`.

### 2. Who owns it?

**`ChainManager` owns the ChainDB instance.**

Ownership means:
- ChainManager holds the `std::unique_ptr<ChainDB>`
- ChainManager controls its lifetime
- ChainManager decides when to close/flush it
- No other component may construct a competing instance

ChainManager is the authority over chain state. The database is its memory.

### 3. Who constructs it?

**`DaemonApp::Init()` constructs the ChainDB, then transfers ownership to ChainManager.**

Construction sequence:
```cpp
// In DaemonApp::Init()
auto chain_db = std::make_unique<ChainDB>();
chain_db->init(data_dir / "chaindb");

// Transfer ownership
chain_manager_->setChainDB(std::move(chain_db));
```

After this point:
- DaemonApp has no reference to ChainDB
- ChainManager owns it exclusively
- No second construction is legal

### 4. Who may write?

**ONLY `BlockAcceptor` may write to ChainDB.**

Write authority is enforced at compile-time via `ChainWriteToken`:

```cpp
class ChainWriteToken {
    friend class BlockAcceptor;  // ONLY BlockAcceptor can construct
private:
    ChainWriteToken() = default;
};
```

All write methods require this token:
```cpp
Status ChainDB::putBlock(const ChainWriteToken& token, ...);
Status ChainDB::putCoin(const ChainWriteToken& token, ...);
Status ChainDB::setTip(const ChainWriteToken& token, ...);
```

**Exception**: ChainManager may write during initialization (genesis block) before BlockAcceptor exists. This is a bootstrap token created locally and destroyed after genesis.

**Illegal**:
- Mempool writing UTXOs
- Network writing blocks directly
- RPC mutating chain state
- Tests constructing a second writable DB

### 5. Who may read?

**Anyone with a `const ChainDB&` reference.**

Read access is unrestricted but explicit:
- Mempool queries UTXOs via `ChainManager::getChainDB()`
- Network serves blocks via `const ChainDB&`
- RPC queries chain tip via `const ChainDB&`
- Wallet tracks balance via `const ChainDB&`

Reads do NOT require a token. Reads cannot mutate state.

**Illegal**:
- Caching a non-const pointer
- Casting away const
- Cloning the database
- Creating a "read-only view" that has its own file handles

### 6. What is the lifetime?

**ChainDB lives for the entire daemon lifetime.**

```
Created:  DaemonApp::Init()
Lives:    Until DaemonApp::Shutdown()
Destroyed: ChainManager destructor (via unique_ptr)
```

Sequence:
1. **DaemonApp starts** → constructs ChainDB
2. **Ownership transfer** → ChainManager takes ownership
3. **Normal operation** → ChainDB remains open, serves queries, accepts writes
4. **Shutdown signal** → DaemonApp initiates shutdown
5. **ChainManager destroyed** → unique_ptr destructor calls `ChainDB::close()`
6. **RocksDB closed** → DB files flushed, handles released

**Illegal**:
- Closing and reopening ChainDB mid-operation
- Constructing a "temporary" ChainDB for tests that later leaks
- Opening the same data directory in two processes
- Restarting ChainDB without restarting the daemon

### 7. How is it passed?

**Ownership**: `std::unique_ptr<ChainDB>` (move semantics, single owner)
**Read access**: `const ChainDB&` (references, not pointers)
**Write access**: `ChainWriteToken + ChainDB&` (BlockAcceptor only)

Passing rules:

**Construction → Ownership Transfer**:
```cpp
auto db = std::make_unique<ChainDB>();
chain_manager_->setChainDB(std::move(db));  // Ownership moves
// db is now nullptr, ChainManager owns it
```

**Read Access (Dependency Injection)**:
```cpp
class Mempool {
    explicit Mempool(const ChainDB& chain_db)
        : chain_db_(chain_db) {}
private:
    const ChainDB& chain_db_;  // Reference, not ownership
};
```

**Write Access (Compile-Time Restricted)**:
```cpp
class BlockAcceptor {
    void ConnectBlock(ChainDB& db, const Block& block) {
        ChainWriteToken token;  // Only BlockAcceptor can construct
        db.putBlock(token, block.hash, block);
    }
};
```

**Illegal**:
- Passing raw pointers (`ChainDB*`)
- Shared ownership (`std::shared_ptr<ChainDB>`)
- Global variables (`ChainDB* g_chain_db`)
- Service locators (`getChainDB()` that returns mutable access)

---

## Enforcement Checklist

Before ANY code is written, verify:

- [ ] There is exactly ONE `ChainDB` instance in the process
- [ ] ChainManager holds the `unique_ptr<ChainDB>`
- [ ] DaemonApp constructs it, then moves ownership
- [ ] BlockAcceptor is the only writer (enforced by `ChainWriteToken`)
- [ ] All readers receive `const ChainDB&` references
- [ ] Lifetime equals daemon lifetime
- [ ] No adapters, no wrappers, no "convenience DBs"

If ANY of these is false, the code is rejected before review.

---

## What This Destroys

This definition makes the following **architecturally impossible**:

❌ **Second database instance**
   - Only one `unique_ptr<ChainDB>` exists
   - Cannot construct a second without violating ownership

❌ **Ambiguous write authority**
   - Only BlockAcceptor has `ChainWriteToken`
   - Compiler rejects writes from other components

❌ **Leaked mutable access**
   - Readers get `const&`, cannot mutate
   - Casting away const is detectable in review

❌ **Adapters that mask dual databases**
   - No wrappers allowed
   - Direct `ChainDB&` references only

❌ **Test databases that leak to production**
   - Tests must use the same ownership model
   - No "test-only DB" exceptions

❌ **Lifetime ambiguity**
   - ChainDB lives exactly as long as ChainManager
   - No close/reopen cycles

---

## Violations Are Compilation Errors (Not Runtime Bugs)

The goal is to make violations **uncompilable**, not unrunnable.

**Example 1: Illegal Write (Won't Compile)**
```cpp
class Mempool {
    void addToUTXO(ChainDB& db, const Coin& coin) {
        // ERROR: no ChainWriteToken in scope
        db.putCoin(???, txid, vout, coin);  // Compiler error: missing token
    }
};
```

**Example 2: Illegal Second DB (Won't Compile)**
```cpp
class NetworkManager {
    NetworkManager() {
        // ERROR: ChainManager already owns the unique_ptr
        auto second_db = std::make_unique<ChainDB>();  // Violates invariant
    }
};
```

**Example 3: Illegal Mutable Caching (Won't Compile)**
```cpp
class RPC {
    RPC(const ChainDB& db)
        : cached_db_(db) {}  // OK: const reference

    void modifyChain() {
        // ERROR: cached_db_ is const, cannot call non-const methods
        cached_db_.putBlock(...);  // Compiler error: discards qualifiers
    }
private:
    const ChainDB& cached_db_;
};
```

If the compiler accepts the code, the invariant is preserved.

---

## The Contract (One Paragraph)

**ChainDB is a unique resource owned exclusively by ChainManager, constructed once by DaemonApp during initialization, living for the daemon's lifetime, with write authority restricted to BlockAcceptor via compile-time ChainWriteToken enforcement, while all other components receive read-only const references through explicit dependency injection, making it architecturally impossible to create dual databases, leak write access, or violate single-writer authority.**

---

## What Comes Next

1. **Verify existing code against this definition**
   Any code that violates these rules is deleted, not adapted.

2. **Rebuild components to fit this model**
   ChainManager, BlockAcceptor, Mempool, Network, RPC must conform.

3. **Reject PRs that violate the invariant**
   No exceptions. No "just for this feature". No "temporary workaround".

This document is law. The compiler enforces it.

---

**END OF ONE DB DEFINITION**

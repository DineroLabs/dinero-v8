# SQLiteManager Cleanup Plan

**File:** `src/database/sqlite_manager.cpp`

---

## Methods to DELETE (Explorer/Mempool/Peers):

### **1. initializeBlockchainDB()** (lines ~96-105)
```cpp
bool SQLiteManager::initializeBlockchainDB() {
    // ... DELETE entire method
}
```

### **2. initializeMempoolDB()** (lines ~174-183)
```cpp
bool SQLiteManager::initializeMempoolDB() {
    // ... DELETE entire method
}
```

### **3. initializePeersDB()** (lines ~201-210)
```cpp
bool SQLiteManager::initializePeersDB() {
    // ... DELETE entire method
}
```

### **4. createBlockchainSchema()** (lines ~611-640)
```cpp
bool SQLiteManager::createBlockchainSchema() {
    // ... DELETE entire method
}
```

### **5. createMempoolSchema()** (lines ~745-776)
```cpp
bool SQLiteManager::createMempoolSchema() {
    // ... DELETE entire method
}
```

### **6. createPeersSchema()** (lines ~819-850)
```cpp
bool SQLiteManager::createPeersSchema() {
    // ... DELETE entire method
}
```

---

## Header File Changes

**File:** `include/database/sqlite_manager.h`

Already cleaned (methods already removed):
- ✅ `getBlockchainDB()` - REMOVED
- ✅ `getMempoolDB()` - REMOVED
- ✅ `getPeersDB()` - REMOVED

---

## Status

**Implementation file:** ⏳ TODO - Remove 6 obsolete methods
**Header file:** ✅ DONE - Already cleaned

The methods are dead code since the header no longer declares them.
They cannot be called, but should be removed for cleanup.

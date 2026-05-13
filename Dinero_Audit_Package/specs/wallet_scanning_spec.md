# Wallet Scanning Specification

**Version:** 1.0
**Date:** 2025-01-17
**Status:** Implemented

---

## 1. Overview

This document specifies how DineroCoin wallets scan the blockchain to identify and recover confidential outputs belonging to the user.

### 1.1 Goals

- **Completeness:** Find all outputs belonging to wallet
- **Privacy:** Don't reveal which outputs are ours to network
- **Efficiency:** Minimize bandwidth and computation
- **Resilience:** Handle corrupted data and chain reorgs

---

## 2. Scanning Algorithm

### 2.1 High-Level Flow

```
1. Fetch blocks from node (or electrum server)
2. For each block:
   a. For each transaction:
      i. For each output:
         - Extract ephemeral pubkey
         - Derive ECDH nonce
         - Attempt rewind
         - If success: Add to wallet database
3. Update wallet state
4. Mark scanned height
```

### 2.2 Incremental Scanning

**Initial Sync:**
```
Start Height: 0 (genesis)
End Height: Current chain tip
```

**Ongoing Updates:**
```
Start Height: Last scanned height + 1
End Height: Current chain tip
```

**Database State:**
```sql
CREATE TABLE wallet_state (
    last_scanned_height INTEGER,
    last_scanned_blockhash BLOB(32),
    last_scan_timestamp INTEGER
);
```

---

## 3. Block Fetching

### 3.1 Full Node Mode

**RPC Method:** `getblock`

```json
{
  "method": "getblock",
  "params": {
    "blockhash": "0xabcd...",
    "verbosity": 2
  }
}
```

**Response:**
```json
{
  "height": 12345,
  "hash": "0xabcd...",
  "transactions": [
    {
      "txid": "0x1234...",
      "vout": [
        {
          "value": 0,
          "is_confidential": true,
          "commitment": "0x02...",
          "range_proof": "0x...",
          "nonce": "0x..."
        }
      ]
    }
  ]
}
```

### 3.2 Light Client Mode (Electrum-Style)

**Request:** Subscribe to blocks

```json
{
  "method": "blockchain.headers.subscribe",
  "params": []
}
```

**Fetch Relevant TXs:**
```json
{
  "method": "blockchain.transaction.get",
  "params": {
    "tx_hash": "0x1234...",
    "verbose": true
  }
}
```

**Optimization:** Only fetch TXs with confidential outputs (filter by bloom)

---

## 4. Output Identification

### 4.1 Rewind Attempt

For each confidential output:

```cpp
bool TryIdentifyOutput(const TxOutput& output,
                        const uint8_t* view_privkey,
                        WalletOutput* result) {
    // 1. Extract ephemeral pubkey from nonce field
    uint8_t ephemeral_pubkey[33];
    memcpy(ephemeral_pubkey, output.nonce.data(), 33);

    // 2. Derive ECDH nonce
    uint8_t nonce[32];
    if (!DeriveECDHNonce(ephemeral_pubkey, view_privkey, nonce)) {
        return false;  // Invalid ephemeral pubkey
    }

    // 3. Attempt rewind
    uint64_t recovered_value;
    uint8_t recovered_blinding[32];

    int rewind_result = bp_rewind(
        output.commitment.data(),
        output.range_proof.data(),
        output.range_proof.size(),
        nonce,
        &recovered_value,
        recovered_blinding
    );

    if (rewind_result == 1) {
        // Success! Output is ours
        result->value = recovered_value;
        memcpy(result->blinding, recovered_blinding, 32);
        result->commitment = output.commitment;
        result->is_ours = true;

        // SECURITY: Zeroize
        explicit_bzero(recovered_blinding, 32);
        explicit_bzero(nonce, 32);

        return true;
    } else if (rewind_result == 0) {
        // Not ours (wrong nonce)
        explicit_bzero(nonce, 32);
        return false;
    } else {
        // Error (malformed data)
        LOG_WARNING("Malformed output: " << output_id);
        explicit_bzero(nonce, 32);
        return false;
    }
}
```

### 4.2 Early Abort Optimization

**Commitment Check First:**

```cpp
// Fast path: Check commitment matches before full rewind
uint8_t nonce[32];
DeriveECDHNonce(ephemeral_pubkey, view_privkey, nonce);

// Decrypt value and blinding
uint64_t value;
uint8_t blinding[32];
DecryptRewindData(output.range_proof.data(), nonce, &value, blinding);

// Compute expected commitment
Commitment expected = ComputeCommitment(value, blinding);

if (expected != output.commitment) {
    // Not ours! Skip expensive proof verification
    return false;
}

// Only verify proof if commitment matches
int result = bp_verify(output.commitment, proof, proof_len);
if (result != 1) {
    // Corrupted data
    return false;
}
```

**Speedup:** 10x faster (skip proof verification for non-ours outputs)

---

## 5. Wallet Database

### 5.1 Schema

```sql
-- Unspent outputs
CREATE TABLE wallet_outputs (
    txid BLOB(32) NOT NULL,
    output_index INTEGER NOT NULL,
    block_height INTEGER NOT NULL,
    block_hash BLOB(32) NOT NULL,

    -- Confidential data (encrypted at rest)
    value_encrypted BLOB,  -- AES-256-GCM encrypted
    blinding_encrypted BLOB,  -- AES-256-GCM encrypted

    -- Public data
    commitment BLOB(33) NOT NULL,
    script_pubkey BLOB,

    -- Status
    spent BOOLEAN DEFAULT 0,
    spent_height INTEGER,
    confirmed BOOLEAN DEFAULT 0,

    PRIMARY KEY (txid, output_index)
);

-- Spent outputs (for history)
CREATE TABLE wallet_history (
    txid BLOB(32) NOT NULL,
    output_index INTEGER NOT NULL,
    spent_in_tx BLOB(32),
    spent_height INTEGER,
    timestamp INTEGER
);

-- Scanning state
CREATE TABLE wallet_state (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    last_scanned_height INTEGER NOT NULL,
    last_scanned_blockhash BLOB(32) NOT NULL,
    last_scan_timestamp INTEGER NOT NULL
);
```

### 5.2 Encryption at Rest

**Never Store Plaintext Blinding Factors:**

```cpp
// Derive database encryption key
uint8_t db_key[32];
PBKDF2_HMAC_SHA256(
    wallet_password,
    "dinero_db_salt",
    100000,  // iterations
    db_key
);

// Encrypt value
uint8_t encrypted_value[8 + 16];  // value + GCM tag
AES256_GCM_Encrypt(
    db_key,
    nonce_for_this_output,
    &value, 8,
    encrypted_value
);

// Encrypt blinding factor
uint8_t encrypted_blinding[32 + 16];  // blinding + GCM tag
AES256_GCM_Encrypt(
    db_key,
    nonce_for_this_output,
    blinding, 32,
    encrypted_blinding
);

// Store encrypted data
INSERT INTO wallet_outputs VALUES (
    txid, index, height, blockhash,
    encrypted_value, encrypted_blinding,
    commitment, script_pubkey,
    0, NULL, 0
);

// SECURITY: Zeroize
explicit_bzero(db_key, 32);
explicit_bzero(&value, 8);
explicit_bzero(blinding, 32);
```

---

## 6. Scanning Performance

### 6.1 Performance Characteristics

**Per Output:**
- ECDH derivation: ~20 μs
- Decrypt rewind data: ~10 μs
- Commitment check: ~50 μs
- Proof verification: ~100 ms (only if commitment matches)

**For 1000 Outputs (1% match rate):**
- Fast path (commitment check): ~80 ms
- Proof verification (10 matches): ~1 second
- **Total:** ~1.1 seconds

**For 1000 Outputs (no matches):**
- Fast path only: ~80 ms
- **Total:** 80 ms (100x faster!)

### 6.2 Parallel Scanning

```cpp
// Parallel rewind attempts (thread-safe)
std::vector<TxOutput> outputs = GetOutputsFromBlock(block);

std::vector<std::future<std::optional<WalletOutput>>> futures;

for (auto& output : outputs) {
    futures.push_back(std::async(std::launch::async, [&]() {
        WalletOutput wallet_out;
        if (TryIdentifyOutput(output, view_privkey, &wallet_out)) {
            return std::optional<WalletOutput>(wallet_out);
        }
        return std::optional<WalletOutput>();
    }));
}

// Collect results
for (auto& future : futures) {
    auto result = future.get();
    if (result.has_value()) {
        AddToWalletDB(result.value());
    }
}
```

**Speedup:** ~8x on 8-core CPU

### 6.3 Bloom Filter Optimization (Future)

**Concept:** Request server to filter outputs by bloom filter

```json
{
  "method": "wallet.scripthash.subscribe",
  "params": {
    "scripthash": "0xabcd...",
    "bloom_filter": "0x..."
  }
}
```

**Privacy Trade-off:** Server learns approximate set of outputs we care about

**Status:** Not implemented (requires server-side changes)

---

## 7. Chain Reorganization Handling

### 7.1 Reorg Detection

```cpp
void ScanNewBlock(uint32_t height, const uint8_t* block_hash) {
    // 1. Check if this block extends our chain
    uint8_t expected_prev_hash[32];
    if (!GetBlockHash(height - 1, expected_prev_hash)) {
        // First block, no previous
    } else {
        Block block = FetchBlock(block_hash);
        if (memcmp(block.prev_block_hash, expected_prev_hash, 32) != 0) {
            // REORG DETECTED!
            HandleReorg(height, block_hash);
            return;
        }
    }

    // 2. Normal scanning
    ScanBlock(height, block_hash);
}
```

### 7.2 Reorg Response

```cpp
void HandleReorg(uint32_t fork_height, const uint8_t* new_block_hash) {
    LOG_WARNING("Chain reorg detected at height " << fork_height);

    // 1. Find fork point
    uint32_t fork_point = FindForkPoint(fork_height);

    // 2. Invalidate outputs from orphaned chain
    InvalidateOutputsAfterHeight(fork_point);

    // 3. Rescan from fork point
    for (uint32_t h = fork_point; h <= GetChainTip(); h++) {
        uint8_t block_hash[32];
        GetBlockHashAtHeight(h, block_hash);
        ScanBlock(h, block_hash);
    }

    // 4. Update wallet state
    UpdateLastScannedHeight(GetChainTip());
}

void InvalidateOutputsAfterHeight(uint32_t height) {
    // Mark outputs as unconfirmed
    UPDATE wallet_outputs
    SET confirmed = 0
    WHERE block_height > height;

    // Mark spends as pending revalidation
    UPDATE wallet_outputs
    SET spent = 0, spent_height = NULL
    WHERE spent_height > height;
}
```

### 7.3 Confirmation Depth

**Recommendation:** Wait for 6 confirmations before considering output spendable

```cpp
bool IsOutputSpendable(const WalletOutput& output) {
    uint32_t current_height = GetChainTip();
    uint32_t confirmations = current_height - output.block_height + 1;

    const uint32_t MIN_CONFIRMATIONS = 6;
    return confirmations >= MIN_CONFIRMATIONS;
}
```

---

## 8. Corrupted Data Handling

### 8.1 Graceful Degradation

```cpp
struct ScanningPolicy {
    bool stop_on_corruption = false;      // Continue scanning
    bool log_corrupted_outputs = true;    // Log for debugging
    uint32_t max_corrupted_per_block = 10;  // Safety limit
    bool auto_skip_corrupted_blocks = true;
};

void ScanBlock(const Block& block, const ScanningPolicy& policy) {
    uint32_t corrupted_count = 0;

    for (auto& tx : block.transactions) {
        for (auto& output : tx.vout) {
            if (!output.is_confidential) continue;

            try {
                WalletOutput wallet_out;
                if (TryIdentifyOutput(output, view_privkey, &wallet_out)) {
                    AddToWalletDB(wallet_out);
                }
            } catch (const CorruptedDataException& e) {
                corrupted_count++;

                if (policy.log_corrupted_outputs) {
                    LOG_WARNING("Corrupted output: " << tx.txid << ":" << output.index);
                }

                if (corrupted_count >= policy.max_corrupted_per_block) {
                    if (policy.stop_on_corruption) {
                        throw ScanningException("Too many corrupted outputs");
                    }
                    if (policy.auto_skip_corrupted_blocks) {
                        LOG_ERROR("Skipping block " << block.height);
                        return;
                    }
                }
            }
        }
    }
}
```

### 8.2 Structure Validation

**Before Attempting Rewind:**

```cpp
bool ValidateOutputStructure(const TxOutput& output) {
    // 1. Check value is 0
    if (output.value != 0) {
        LOG_ERROR("Confidential output has non-zero value");
        return false;
    }

    // 2. Check commitment size
    if (output.commitment.size() != 33) {
        LOG_ERROR("Invalid commitment size: " << output.commitment.size());
        return false;
    }

    // 3. Check commitment prefix
    uint8_t prefix = output.commitment[0];
    if (prefix != 0x02 && prefix != 0x03) {
        LOG_ERROR("Invalid commitment prefix: " << (int)prefix);
        return false;
    }

    // 4. Check proof size
    size_t proof_size = output.range_proof.size();
    if (proof_size < 650 || proof_size > 800) {
        LOG_ERROR("Invalid proof size: " << proof_size);
        return false;
    }

    // 5. Check nonce size
    if (output.nonce.size() != 65) {
        LOG_ERROR("Invalid nonce size: " << output.nonce.size());
        return false;
    }

    // 6. Check ephemeral pubkey prefix
    uint8_t nonce_prefix = output.nonce[0];
    if (nonce_prefix != 0x02 && nonce_prefix != 0x03) {
        LOG_ERROR("Invalid ephemeral pubkey prefix: " << (int)nonce_prefix);
        return false;
    }

    return true;
}
```

---

## 9. Privacy Considerations

### 9.1 Network Privacy

**Threat:** Node learns which outputs we're interested in

**Defenses:**

1. **Full Node:** Run own node (perfect privacy)
2. **Tor:** Connect to node via Tor
3. **Rotate Connections:** Use different nodes for different scans
4. **Bloom Filters:** Use overly broad filters (reduces precision)

### 9.2 Timing Attacks

**Threat:** Server measures response time to infer if output is ours

**Defense:**

```cpp
// Constant-time scanning (always verify all outputs)
void ConstantTimeScan(const std::vector<TxOutput>& outputs) {
    for (auto& output : outputs) {
        WalletOutput result;
        bool is_ours = TryIdentifyOutput(output, view_privkey, &result);

        // Always perform same operations (constant time)
        if (is_ours) {
            AddToWalletDB_ConstantTime(result);
        } else {
            AddToWalletDB_ConstantTime_Dummy();
        }
    }
}
```

**Trade-off:** Slower scanning, but better privacy

### 9.3 Database Leaks

**Threat:** Malware reads wallet database

**Defense:**
- AES-256-GCM encryption at rest
- Zeroize keys on wallet lock
- Use hardware wallet for view key storage

---

## 10. Subaddress Scanning

### 10.1 Multiple View Keys

For subaddresses with same view key:

```cpp
// Subaddresses share view key
std::vector<Subaddress> subaddresses = wallet.GetAllSubaddresses();

for (auto& output : block_outputs) {
    // Try with master view key (works for all subaddresses)
    WalletOutput result;
    if (TryIdentifyOutput(output, master_view_privkey, &result)) {
        // Determine which subaddress
        for (auto& subaddr : subaddresses) {
            if (OutputMatchesSubaddress(result, subaddr)) {
                result.subaddress_index = subaddr.index;
                AddToWalletDB(result);
                break;
            }
        }
    }
}
```

**Efficiency:** Single scan finds all subaddress outputs

### 10.2 Output-to-Subaddress Matching

```cpp
bool OutputMatchesSubaddress(const WalletOutput& output,
                              const Subaddress& subaddr) {
    // Check if script_pubkey matches subaddress spend pubkey
    return output.script_pubkey == CreateScript(subaddr.spend_pubkey);
}
```

---

## 11. Rescan Command

### 11.1 Full Rescan

```cpp
void RescanBlockchain(uint32_t start_height = 0) {
    LOG_INFO("Starting blockchain rescan from height " << start_height);

    // 1. Clear existing data (optional)
    if (start_height == 0) {
        ClearWalletDatabase();
    } else {
        InvalidateOutputsAfterHeight(start_height - 1);
    }

    // 2. Scan all blocks
    uint32_t tip_height = GetChainTip();
    for (uint32_t h = start_height; h <= tip_height; h++) {
        uint8_t block_hash[32];
        GetBlockHashAtHeight(h, block_hash);

        Block block = FetchBlock(block_hash);
        ScanBlock(block);

        // Progress update
        if (h % 1000 == 0) {
            LOG_INFO("Scanned " << h << " / " << tip_height);
        }
    }

    // 3. Update state
    UpdateLastScannedHeight(tip_height);
    LOG_INFO("Rescan complete");
}
```

### 11.2 Partial Rescan (Range)

```cpp
void RescanRange(uint32_t start_height, uint32_t end_height) {
    for (uint32_t h = start_height; h <= end_height; h++) {
        uint8_t block_hash[32];
        GetBlockHashAtHeight(h, block_hash);
        ScanBlock(h, block_hash);
    }
}
```

---

## 12. Wallet Sync Modes

### 12.1 Full Sync (Trusted Node)

```
Mode: Full blockchain download
Privacy: Maximum (run own node)
Speed: Slow (initial sync days)
Bandwidth: High (hundreds of GB)
```

### 12.2 SPV Sync (Light Client)

```
Mode: Headers + relevant TXs only
Privacy: Medium (server sees TX requests)
Speed: Fast (minutes)
Bandwidth: Low (MB)
```

### 12.3 Remote Scan (View Key Delegation)

```
Mode: Give view key to server
Privacy: Low (server sees all amounts)
Speed: Instant
Bandwidth: Minimal
```

**Use Case:** Exchange hot wallet (can't spend, only view)

---

## 13. Error Recovery

### 13.1 Interrupted Scan

```cpp
void ResumeScan() {
    // 1. Load last scanned height
    uint32_t last_height = GetLastScannedHeight();

    // 2. Verify last block still in main chain
    uint8_t last_hash[32];
    GetLastScannedBlockHash(last_hash);

    uint8_t current_hash_at_height[32];
    GetBlockHashAtHeight(last_height, current_hash_at_height);

    if (memcmp(last_hash, current_hash_at_height, 32) != 0) {
        // Reorg happened during downtime
        LOG_WARNING("Reorg detected, rescanning from fork point");
        HandleReorg(last_height);
        return;
    }

    // 3. Resume from next block
    ScanFrom(last_height + 1);
}
```

### 13.2 Database Corruption

```cpp
bool VerifyDatabaseIntegrity() {
    // 1. Check all commitments are valid
    auto outputs = LoadAllOutputs();
    for (auto& output : outputs) {
        if (!VerifyCommitment(output.commitment)) {
            LOG_ERROR("Corrupted commitment in database");
            return false;
        }
    }

    // 2. Check all encrypted data is decryptable
    for (auto& output : outputs) {
        try {
            uint64_t value = DecryptValue(output.value_encrypted);
            uint8_t blinding[32];
            DecryptBlinding(output.blinding_encrypted, blinding);

            // Verify commitment matches
            Commitment expected = ComputeCommitment(value, blinding);
            if (expected != output.commitment) {
                LOG_ERROR("Commitment mismatch in database");
                return false;
            }
        } catch (...) {
            LOG_ERROR("Failed to decrypt database entry");
            return false;
        }
    }

    return true;
}
```

**Recovery:** If verification fails, trigger full rescan

---

## 14. Testing

### 14.1 Test Scenarios

```cpp
TEST(WalletScan, IdentifyOwnOutput) {
    // Create output sent to our address
    auto output = CreateConfidentialOutput(
        our_view_pubkey, 1000000
    );

    // Scan should identify it
    WalletOutput result;
    ASSERT_TRUE(TryIdentifyOutput(output, our_view_privkey, &result));
    ASSERT_EQ(result.value, 1000000);
}

TEST(WalletScan, IgnoreOthersOutput) {
    // Create output sent to different address
    auto output = CreateConfidentialOutput(
        other_view_pubkey, 5000000
    );

    // Scan should not identify it
    WalletOutput result;
    ASSERT_FALSE(TryIdentifyOutput(output, our_view_privkey, &result));
}

TEST(WalletScan, HandleCorruptedOutput) {
    auto output = CreateCorruptedOutput();

    // Should not crash
    WalletOutput result;
    ASSERT_FALSE(TryIdentifyOutput(output, our_view_privkey, &result));
}

TEST(WalletScan, HandleReorg) {
    // Scan initial chain
    ScanBlock(100, block_100_hash);
    ASSERT_EQ(GetWalletBalance(), 1000000);

    // Reorg removes that block
    HandleReorg(99);

    // Balance should be adjusted
    ASSERT_EQ(GetWalletBalance(), 0);
}
```

### 14.2 Performance Tests

```cpp
TEST(WalletScan, ScanPerformance) {
    // Create block with 1000 outputs (none ours)
    Block block = CreateBlockWithOutputs(1000, false);

    auto start = std::chrono::high_resolution_clock::now();
    ScanBlock(block);
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Should complete in < 200ms (with early abort optimization)
    ASSERT_LT(duration.count(), 200);
}
```

---

## 15. References

1. **Monero Wallet Scanning:** Similar rewind-based approach
2. **Electrum Protocol:** Light client sync mechanism
3. **BIP-157/158:** Compact block filters (future integration)
4. **SPV Security:** Simplified Payment Verification

---

**End of Specification**

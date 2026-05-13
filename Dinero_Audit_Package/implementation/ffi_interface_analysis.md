# FFI Interface Analysis

**Version:** 1.0
**Date:** 2025-01-17
**Audience:** Security Auditors

---

## 1. Overview

This document analyzes the Foreign Function Interface (FFI) boundary between Rust (Bulletproofs implementation) and C++ (DineroCoin core).

### 1.1 Architecture

```
┌─────────────────┐
│  C++ Core       │
│  (DineroCoin)   │
└────────┬────────┘
         │ FFI Calls
         │ (C ABI)
         ▼
┌─────────────────┐
│  Rust FFI Layer │
│  (lib.rs)       │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Bulletproofs   │
│  (Dalek 4.0)    │
└─────────────────┘
```

### 1.2 Security Boundary

**Trust Relationship:**
- C++ trusts Rust to validate proofs correctly
- Rust trusts C++ to provide valid buffer sizes
- Both sides must handle errors gracefully

**Key Risk:** Buffer overflows at FFI boundary

---

## 2. Function Inventory

### 2.1 Core Functions

```c
// Generate standard Bulletproof
int bp_generate(
    uint64_t value,
    const uint8_t* blind_ptr,
    uint8_t* proof_out,
    size_t* proof_len_out
);

// Generate rewindable Bulletproof
int bp_generate_with_nonce(
    uint64_t value,
    const uint8_t* blind_ptr,
    const uint8_t* nonce_ptr,
    uint8_t* proof_out,
    size_t* proof_len_out
);

// Verify single proof
int bp_verify(
    const uint8_t* commitment_ptr,
    const uint8_t* proof_ptr,
    size_t proof_len
);

// Verify multiple proofs (batch)
int bp_verify_batch(
    const uint8_t** commitment_ptrs,
    const uint8_t** proof_ptrs,
    const size_t* proof_lens,
    size_t count
);

// Rewind proof to recover value
int bp_rewind(
    const uint8_t* commitment_ptr,
    const uint8_t* proof_ptr,
    size_t proof_len,
    const uint8_t* nonce_ptr,
    uint64_t* value_out,
    uint8_t* blind_out
);
```

### 2.2 Return Codes

```
 1  = Success / Valid
 0  = Not ours (for rewind) / Invalid (for verify)
-1  = Error (malformed input, internal failure)
```

---

## 3. Buffer Safety Analysis

### 3.1 Input Buffers

**`blind_ptr` (32 bytes):**
```rust
// Validation in Rust
let blinding_slice = unsafe {
    if blind_ptr.is_null() {
        return -1;
    }
    std::slice::from_raw_parts(blind_ptr, 32)
};
```

**Risk:** If C++ passes buffer < 32 bytes → out-of-bounds read

**Mitigation:**
- ✅ Null pointer check
- ✅ Fixed size (32 bytes)
- ⚠️ No dynamic size validation (trusts caller)

**Auditor Check:** Verify all C++ call sites pass 32-byte buffers.

**`nonce_ptr` (32 bytes):**
```rust
let nonce_slice = unsafe {
    if nonce_ptr.is_null() {
        return -1;
    }
    std::slice::from_raw_parts(nonce_ptr, 32)
};
```

**Same analysis as `blind_ptr`.**

**`proof_ptr` (variable size):**
```rust
// Critical validation
if proof_len == 0 || proof_len > MAX_PROOF_SIZE {
    return -1;  // Reject invalid sizes
}

if proof_ptr.is_null() {
    return -1;  // Reject null pointer
}

let proof_slice = unsafe {
    std::slice::from_raw_parts(proof_ptr, proof_len)
};
```

**Risk:** If `proof_len` > actual buffer size → out-of-bounds read

**Mitigation:**
- ✅ Null pointer check
- ✅ Maximum size validation (MAX_PROOF_SIZE = 2048)
- ✅ Zero-length rejection
- ⚠️ Still trusts C++ to pass correct `proof_len`

**Auditor Check:** Verify C++ never passes `proof_len` > actual buffer size.

**`commitment_ptr` (33 bytes):**
```rust
// CRITICAL: Fixed-size assumption
let commitment_slice = unsafe {
    if commitment_ptr.is_null() {
        return -1;
    }
    std::slice::from_raw_parts(commitment_ptr, 33)
};
```

**Risk:** If C++ passes buffer < 33 bytes → out-of-bounds read

**Mitigation:**
- ✅ Null pointer check
- ✅ Fixed size (33 bytes)
- ⚠️ No dynamic size validation

**Recommendation:** Add explicit size parameter for commitments in future API.

### 3.2 Output Buffers

**`proof_out` (up to 2048 bytes):**
```rust
// Check output buffer is provided
if proof_out.is_null() || proof_len_out.is_null() {
    return -1;
}

// Generate proof
let proof_bytes = proof.to_bytes();

// CRITICAL: Check buffer size before writing
let required_size = proof_bytes.len();
if required_size > 2048 {
    return -1;  // Proof too large
}

// Write to output buffer
let proof_out_slice = unsafe {
    std::slice::from_raw_parts_mut(proof_out, required_size)
};
proof_out_slice.copy_from_slice(&proof_bytes);

// Write length
unsafe {
    *proof_len_out = required_size;
}
```

**Risk:** If C++ provides buffer < `required_size` → buffer overflow

**Mitigation:**
- ✅ Null pointer checks
- ✅ Size limit (2048 bytes max)
- ⚠️ Trusts C++ to provide large enough buffer

**Current C++ Practice:**
```cpp
uint8_t proof[2048];  // Always allocate max size
size_t proof_len;
bp_generate(value, blinding, proof, &proof_len);
```

**Auditor Check:** Verify all C++ call sites allocate ≥ 2048 bytes for `proof_out`.

**`value_out` and `blind_out`:**
```rust
if value_out.is_null() || blind_out.is_null() {
    return -1;
}

unsafe {
    *value_out = decrypted_value;
}

let blind_out_slice = unsafe {
    std::slice::from_raw_parts_mut(blind_out, 32)
};
blind_out_slice.copy_from_slice(&decrypted_blind);
```

**Risk:** If `blind_out` buffer < 32 bytes → buffer overflow

**Mitigation:**
- ✅ Null pointer checks
- ✅ Fixed size (32 bytes)
- ⚠️ Trusts C++ to provide 32-byte buffer

---

## 4. Panic Safety

### 4.1 Panic Boundaries

**All FFI functions wrapped:**
```rust
#[no_mangle]
pub extern "C" fn bp_verify(
    commitment_ptr: *const u8,
    proof_ptr: *const u8,
    proof_len: size_t,
) -> c_int {
    // Catch any panics
    let result = std::panic::catch_unwind(|| {
        bp_verify_impl(commitment_ptr, proof_ptr, proof_len)
    });

    match result {
        Ok(val) => val,
        Err(_) => {
            eprintln!("PANIC in bp_verify");
            -1  // Return error code
        }
    }
}
```

**Purpose:** Prevent Rust panics from unwinding into C++ (undefined behavior)

**Auditor Check:** Verify ALL public FFI functions use `catch_unwind`.

### 4.2 Panic Sources

**Potential panics:**
1. Out-of-memory (allocation failure)
2. Integer overflow (unlikely with checked arithmetic)
3. Array indexing bugs
4. Dalek library panics

**Mitigation:** All panics caught and converted to error code -1.

---

## 5. Memory Management

### 5.1 Ownership Rules

**Inputs (borrowed):**
```rust
// Rust borrows, does NOT take ownership
let blinding_slice = std::slice::from_raw_parts(blind_ptr, 32);
// blinding_slice is valid only during function call
// C++ still owns the buffer
```

**Outputs (C++-owned):**
```rust
// Rust writes to C++-owned buffer
let proof_out_slice = std::slice::from_raw_parts_mut(proof_out, size);
proof_out_slice.copy_from_slice(&data);
// C++ must eventually free this buffer
```

**No Rust Allocations Returned:** All output buffers are C++-owned.

### 5.2 Zeroization

**Sensitive Data Handling:**
```rust
// After using blinding factor
let mut blinding_scalar = Scalar::from_bytes_mod_order(blinding_slice);

// ... use blinding_scalar ...

// SECURITY: Zeroize before dropping
zeroize::Zeroize::zeroize(&mut blinding_scalar);
```

**Auditor Check:**
- [ ] All blinding factors zeroized
- [ ] All nonces zeroized
- [ ] All decrypted values zeroized
- [ ] No sensitive data logged

**File:** `third_party/bulletproofs_ffi/src/lib.rs`

**Lines to review:**
- `bp_generate_with_nonce`: Lines 200-250
- `bp_rewind`: Lines 400-500

---

## 6. Thread Safety

### 6.1 Global State

**Ristretto Generators:**
```rust
lazy_static! {
    static ref BP_GENS: BulletproofGens = BulletproofGens::new(64, 1);
    static ref PC_GENS: PedersenGens = PedersenGens::default();
}
```

**Thread Safety:** `lazy_static` ensures thread-safe initialization

**Immutable:** Generators are never modified after creation

**Conclusion:** FFI functions are thread-safe for concurrent calls.

### 6.2 C++ Side Thread Safety

**Requirement:** C++ must not modify buffers during FFI call

Example **UNSAFE** code:
```cpp
// Thread 1
bp_verify(commitment, proof, len);

// Thread 2 (concurrent)
memset(proof, 0, len);  // ❌ Modifying buffer during verify!
```

**Recommendation:** C++ should use mutexes around sensitive buffers.

---

## 7. Error Propagation

### 7.1 Rust Error Handling

```rust
fn bp_verify_impl(
    commitment_ptr: *const u8,
    proof_ptr: *const u8,
    proof_len: size_t,
) -> c_int {
    // 1. Validate inputs
    if proof_ptr.is_null() || commitment_ptr.is_null() {
        return -1;
    }

    if proof_len < 650 || proof_len > 800 {
        return -1;  // Size check
    }

    // 2. Parse proof
    let proof_slice = unsafe {
        std::slice::from_raw_parts(proof_ptr, proof_len)
    };

    let proof = match RangeProof::from_bytes(&proof_slice) {
        Ok(p) => p,
        Err(_) => return -1,  // Malformed proof
    };

    // 3. Verify
    let result = proof.verify_single(&bp_gens, &pc_gens, &transcript, &commitment, 64);

    match result {
        Ok(_) => 1,      // Valid
        Err(_) => 0,     // Invalid
    }
}
```

**Error Mapping:**
- Parse error → -1 (malformed)
- Verification failure → 0 (invalid)
- Success → 1 (valid)

### 7.2 C++ Error Handling

```cpp
int result = bp_verify(commitment, proof, proof_len);

if (result == -1) {
    // Malformed input or internal error
    return ValidationError::MALFORMED_PROOF;
}

if (result == 0) {
    // Proof is invalid (verified but wrong)
    return ValidationError::PROOF_VERIFY_FAILED;
}

// result == 1: Proof is valid
```

**Auditor Check:** Verify C++ distinguishes between -1 and 0 return codes.

---

## 8. Known Issues and TODOs

### 8.1 Missing Size Parameters

**Current API:**
```c
int bp_verify(
    const uint8_t* commitment_ptr,  // Assumed 33 bytes
    const uint8_t* proof_ptr,
    size_t proof_len
);
```

**Improved API (future):**
```c
int bp_verify_safe(
    const uint8_t* commitment_ptr,
    size_t commitment_len,          // Explicit size
    const uint8_t* proof_ptr,
    size_t proof_len
);
```

**Status:** Not implemented (low priority, no known exploits)

### 8.2 Batch Verification Optimization

**Current Implementation:**
```rust
pub extern "C" fn bp_verify_batch(...) -> c_int {
    // Sequential verification with early exit
    for i in 0..count {
        if bp_verify(commitments[i], proofs[i], lens[i]) != 1 {
            return 0;  // First invalid proof fails batch
        }
    }
    return 1;
}
```

**Limitation:** Not true batch verification (no performance benefit)

**Optimal Approach:** Use Dalek's `verify_multiple` for 2-3x speedup

**Status:** TODO - Marked in code at line 482

**File:** `third_party/bulletproofs_ffi/src/lib.rs:482`

---

## 9. Attack Surface

### 9.1 Input Fuzzing Targets

**High Priority:**
1. `bp_verify(commitment, proof, proof_len)` - Variable-size proof
2. `bp_rewind(commitment, proof, proof_len, nonce, ...)` - Multiple inputs
3. `bp_verify_batch(commitments, proofs, lens, count)` - Array of pointers

**Test Cases:**
```
- proof_len = 0
- proof_len = 1
- proof_len = 2^64 - 1 (overflow)
- proof_len = valid, but proof_ptr[proof_len-1] is past buffer end
- commitment with invalid prefix (not 0x02/0x03)
- All zero buffers
- All 0xFF buffers
- Random data
```

### 9.2 Boundary Conditions

**Edge Cases:**
```
value = 0
value = 2^64 - 1
blinding = all zeros
blinding = all 0xFF
nonce = all zeros
proof at minimum size (650 bytes)
proof at maximum size (800 bytes)
batch_count = 0
batch_count = 1
batch_count = 10000
```

---

## 10. Auditor Checklist

### 10.1 Code Review

- [ ] All FFI functions use `catch_unwind`
- [ ] All pointers checked for null before dereferencing
- [ ] All buffer sizes validated against limits
- [ ] No unchecked arithmetic (use `checked_add`, etc.)
- [ ] All sensitive data zeroized
- [ ] No logging of sensitive data
- [ ] Error codes correctly mapped
- [ ] Thread-safe (no mutable globals)

### 10.2 C++ Call Site Review

File: `src/consensus/confidential_validation.cpp`

- [ ] All `bp_*` calls provide correctly sized buffers
- [ ] All return codes checked and handled
- [ ] Buffer allocations use `constexpr` sizes (not runtime)
- [ ] No buffer reuse without clearing
- [ ] Proper error propagation to consensus layer

File: `src/wallet/confidential_transaction.cpp`

- [ ] Same checks as above
- [ ] Sensitive buffers zeroized after use
- [ ] No accidental logging of proofs/blindings

### 10.3 Testing

- [ ] Unit tests cover all FFI functions
- [ ] Fuzz tests for variable-size inputs
- [ ] Boundary condition tests
- [ ] Error handling tests
- [ ] Concurrent access tests

**Test File:** `third_party/bulletproofs_ffi/tests/ffi_tests.rs`

---

## 11. Comparison with Best Practices

### 11.1 Mozilla FFI Guidelines

**Compliance:**
- ✅ Use `repr(C)` for FFI types
- ✅ Use `extern "C"` for ABI compatibility
- ✅ Catch panics at boundary
- ✅ Document safety invariants
- ⚠️ Missing explicit buffer size parameters

### 11.2 Rust FFI Security

**Good Practices Followed:**
- ✅ Minimize unsafe code
- ✅ Validate all inputs
- ✅ Use bounds checking
- ✅ Zeroize sensitive data
- ✅ No memory leaks (all Rust allocations freed)

**Improvements Needed:**
- ⚠️ Add explicit size parameters
- ⚠️ Add fuzzing CI integration
- ⚠️ Document all safety invariants

---

## 12. References

1. **Rust FFI Omnibus:** https://jakegoulding.com/rust-ffi-omnibus/
2. **Mozilla FFI Guide:** https://mozilla.github.io/firefox-browser-architecture/text/0006-rust-components.html
3. **Nomicon (Unsafe Rust):** https://doc.rust-lang.org/nomicon/ffi.html
4. **Dalek Bulletproofs:** https://docs.rs/bulletproofs/4.0.0/

---

**End of Analysis**

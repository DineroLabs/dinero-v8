# ARM SHA Hardware Extensions - Full Implementation

## 🎯 **Goal: 3-5x Speedup on Apple Silicon**

**Current:** 1.24x with basic NEON  
**Target:** 3-5x with ARM Crypto Extensions  
**Timeline:** 2-3 days of focused work

---

## 📊 **What's Missing?**

### Current Implementation (src/crypto/sha256_neon.cpp)
The current code uses **generic NEON intrinsics** for vector operations:
- `vror32()` - rotation via shift+insert
- `veorq_u32()` - XOR operations
- `vaddq_u32()` - addition
- Manual SHA-256 round calculations

**Problem:** These are general-purpose vector ops, not SHA-specific hardware acceleration.

### What We Need: ARM Crypto Instructions
Apple Silicon M1/M2/M3 have **dedicated SHA-256 hardware** via these instructions:

| Instruction | Purpose | Speedup |
|---|---|---|
| `vsha256hq_u32` | SHA-256 hash update (rounds 0-3) | ~10x vs scalar |
| `vsha256h2q_u32` | SHA-256 hash update (rounds 4-7) | ~10x vs scalar |
| `vsha256su0q_u32` | Message schedule update 0 | ~5x vs scalar |
| `vsha256su1q_u32` | Message schedule update 1 | ~5x vs scalar |

**These are single instructions that do what takes 20-50 scalar instructions!**

---

## 🔧 **Implementation Plan**

### Step 1: Include ARM Crypto Headers
```cpp
// src/crypto/sha256_arm_sha.cpp
#if defined(__ARM_FEATURE_CRYPTO)
#include <arm_neon.h>
#include <arm_acle.h>  // ARM Crypto Extensions
#endif
```

### Step 2: Implement SHA-256 Transform Using Hardware Instructions
```cpp
/**
 * SHA-256 block transform using ARM SHA Extensions
 * Processes one 64-byte block
 */
void sha256_block_arm_sha(uint32_t state[8], const uint8_t block[64]) {
    // Load state into vector registers
    // state0 = {a, b, c, d}
    // state1 = {e, f, g, h}
    uint32x4_t state0 = vld1q_u32(&state[0]);
    uint32x4_t state1 = vld1q_u32(&state[4]);
    
    // Save original state for final addition
    uint32x4_t state0_orig = state0;
    uint32x4_t state1_orig = state1;
    
    // Load message words (big-endian)
    uint32x4_t msg0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 0)));
    uint32x4_t msg1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 16)));
    uint32x4_t msg2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 32)));
    uint32x4_t msg3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 48)));
    
    // Message schedule array
    uint32x4_t tmp0, tmp1, tmp2;
    
    // Rounds 0-3
    tmp0 = vaddq_u32(msg0, vld1q_u32(&K256[0]));
    tmp1 = state0;
    state0 = vsha256hq_u32(state0, state1, tmp0);     // ← HARDWARE SHA-256!
    state1 = vsha256h2q_u32(state1, tmp1, tmp0);      // ← HARDWARE SHA-256!
    
    // Rounds 4-7
    tmp0 = vaddq_u32(msg1, vld1q_u32(&K256[4]));
    tmp1 = state0;
    state0 = vsha256hq_u32(state0, state1, tmp0);
    state1 = vsha256h2q_u32(state1, tmp1, tmp0);
    
    // Rounds 8-11
    tmp0 = vaddq_u32(msg2, vld1q_u32(&K256[8]));
    tmp1 = state0;
    state0 = vsha256hq_u32(state0, state1, tmp0);
    state1 = vsha256h2q_u32(state1, tmp1, tmp0);
    
    // Rounds 12-15
    tmp0 = vaddq_u32(msg3, vld1q_u32(&K256[12]));
    tmp1 = state0;
    state0 = vsha256hq_u32(state0, state1, tmp0);
    state1 = vsha256h2q_u32(state1, tmp1, tmp0);
    
    // Message schedule extension for rounds 16-63
    for (int i = 16; i < 64; i += 16) {
        // Schedule update using hardware instructions
        msg0 = vsha256su0q_u32(msg0, msg1);           // ← HARDWARE SCHEDULE!
        msg0 = vsha256su1q_u32(msg0, msg2, msg3);     // ← HARDWARE SCHEDULE!
        
        tmp0 = vaddq_u32(msg0, vld1q_u32(&K256[i]));
        tmp1 = state0;
        state0 = vsha256hq_u32(state0, state1, tmp0);
        state1 = vsha256h2q_u32(state1, tmp1, tmp0);
        
        // Rotate message schedule
        tmp2 = msg0;
        msg0 = msg1;
        msg1 = msg2;
        msg2 = msg3;
        msg3 = tmp2;
        
        // Continue for remaining rounds...
    }
    
    // Add original state
    state0 = vaddq_u32(state0, state0_orig);
    state1 = vaddq_u32(state1, state1_orig);
    
    // Store result
    vst1q_u32(&state[0], state0);
    vst1q_u32(&state[4], state1);
}
```

### Step 3: Double SHA-256 for Mining
```cpp
/**
 * Double SHA-256 using ARM SHA Extensions
 * Optimized for 80-byte block headers (mining)
 */
void SHA256d_ARM_SHA(const uint8_t* data, size_t blocks, uint8_t* out) {
    for (size_t i = 0; i < blocks; i++) {
        uint32_t state[8];
        uint8_t tmp[32];
        
        // First SHA-256
        memcpy(state, H256, sizeof(state));
        
        // Process first 64 bytes
        sha256_block_arm_sha(state, data + i * 80);
        
        // Process remaining 16 bytes + padding
        uint8_t final_block[64] = {0};
        memcpy(final_block, data + i * 80 + 64, 16);
        final_block[16] = 0x80;  // Padding
        // Add length (80 bytes = 640 bits)
        final_block[62] = 0x02;
        final_block[63] = 0x80;
        sha256_block_arm_sha(state, final_block);
        
        // Output to tmp (big-endian)
        for (int j = 0; j < 8; j++) {
            tmp[j*4 + 0] = (state[j] >> 24) & 0xff;
            tmp[j*4 + 1] = (state[j] >> 16) & 0xff;
            tmp[j*4 + 2] = (state[j] >> 8) & 0xff;
            tmp[j*4 + 3] = state[j] & 0xff;
        }
        
        // Second SHA-256
        memcpy(state, H256, sizeof(state));
        sha256_block_arm_sha(state, tmp);
        
        // Finalize (32 bytes = 256 bits)
        memset(final_block, 0, 64);
        final_block[0] = 0x80;
        final_block[62] = 0x01;
        final_block[63] = 0x00;
        sha256_block_arm_sha(state, final_block);
        
        // Output final hash (big-endian)
        for (int j = 0; j < 8; j++) {
            out[i*32 + j*4 + 0] = (state[j] >> 24) & 0xff;
            out[i*32 + j*4 + 1] = (state[j] >> 16) & 0xff;
            out[i*32 + j*4 + 2] = (state[j] >> 8) & 0xff;
            out[i*32 + j*4 + 3] = state[j] & 0xff;
        }
    }
}
```

### Step 4: Update sha256_simd.cpp
```cpp
#elif defined(DINERO_ARM64)
    extern void SHA256d_NEON(const uint8_t* data, size_t blocks, uint8_t* out);
    
    #if defined(__ARM_FEATURE_CRYPTO)
        extern void SHA256d_ARM_SHA_Real(const uint8_t* data, size_t blocks, uint8_t* out);
        
        inline void SHA256d_ARM_SHA(const uint8_t* data, size_t blocks, uint8_t* out) {
            SHA256d_ARM_SHA_Real(data, blocks, out);  // Use real hardware!
        }
    #else
        inline void SHA256d_ARM_SHA(const uint8_t* data, size_t blocks, uint8_t* out) {
            SHA256d_NEON(data, blocks, out);  // Fallback
        }
    #endif
#endif
```

---

## 📋 **Files to Create/Modify**

### New Files
1. **`src/crypto/sha256_arm_sha.cpp`** (~200 lines)
   - Hardware-accelerated SHA-256 transform
   - Double SHA-256 for mining
   - Proper padding and finalization

### Modified Files
1. **`src/crypto/sha256_simd.cpp`**
   - Remove inline stub for `SHA256d_ARM_SHA`
   - Add proper extern declaration
   - Add compile-time check for `__ARM_FEATURE_CRYPTO`

2. **`CMakeLists.txt`**
   ```cmake
   add_library(dinero_crypto STATIC
     ...
     src/crypto/sha256_neon.cpp
     src/crypto/sha256_arm_sha.cpp  # Add this
   )
   
   # Enable ARM Crypto Extensions
   if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
       target_compile_options(dinero_crypto PRIVATE -march=armv8-a+crypto)
   endif()
   ```

---

## 🧪 **Testing Plan**

### 1. Correctness Test
```cpp
// Verify output matches reference SHA-256
uint8_t header[80] = { /* test data */ };
uint8_t hash_scalar[32], hash_arm_sha[32];

sha256d_scalar(header, 80, hash_scalar);
SHA256d_ARM_SHA(header, 1, hash_arm_sha);

assert(memcmp(hash_scalar, hash_arm_sha, 32) == 0);
```

### 2. Performance Benchmark
```bash
$ ./bench-simd

Expected Results:
Scalar:       2.10 MH/s (baseline)
NEON:         2.60 MH/s (1.24x) ← current
ARM SHA (hw): 7.50 MH/s (3.57x) ← target
```

### 3. Mining Test
```bash
$ ./dinero-miner --address din1q... --threads 8

Expected:
- 3-5x hashrate increase
- "ARM SHA (hw)" detected
- Blocks found correctly
```

---

## ⏱️ **Time Estimate**

| Task | Time | Complexity |
|---|---|---|
| Research ARM Crypto intrinsics | 2 hours | Low |
| Implement `sha256_block_arm_sha()` | 4 hours | Medium |
| Implement `SHA256d_ARM_SHA()` | 2 hours | Low |
| Add CMake flags | 30 min | Low |
| Correctness testing | 2 hours | Medium |
| Performance tuning | 2 hours | Medium |
| Documentation | 1 hour | Low |
| **Total** | **~14 hours** | **2 days** |

---

## 🎯 **Expected Benchmark Results**

### Before (Current)
```
Scalar:    2.10 MH/s
NEON:      2.60 MH/s
Speedup:   1.24x
```

### After (ARM SHA Extensions)
```
Scalar:     2.10 MH/s
ARM SHA:    7.50 MH/s  (3.57x)
Speedup:    3.57x
```

### Full Mining Example (8 cores)
```
Current:  ~21 MH/s total (8 cores × 2.6 MH/s)
ARM SHA:  ~60 MH/s total (8 cores × 7.5 MH/s)

Improvement: 2.86x network-wide hashrate increase!
```

---

## 🚀 **Why This Matters**

### For Apple Silicon Miners
- **M1 Mac Mini:** 8 cores → 60 MH/s (vs 21 MH/s current)
- **M1 Pro:** 10 cores → 75 MH/s
- **M3 Max:** 16 cores → 120 MH/s

### For Network
- Most development happens on Macs
- Early miners likely on Apple Silicon
- 3x speedup = competitive advantage for Phase 1

### For You (Developer)
- Your M-series Mac becomes 3x more efficient
- Mining while developing becomes viable
- Dogfooding your own optimization!

---

## 📚 **References**

### Apple Documentation
- [ARM NEON Intrinsics](https://developer.apple.com/documentation/kernel/arm_neon)
- [Arm C Language Extensions](https://developer.arm.com/documentation/101028/latest/)

### Code Examples
- **Bitcoin Core:** `src/crypto/sha256_arm_shani.cpp`
  ```bash
  # View Bitcoin Core's implementation
  curl -s https://raw.githubusercontent.com/bitcoin/bitcoin/master/src/crypto/sha256_arm_shani.cpp
  ```

- **Crypto++ Library:** `sha_simd.cpp`
  ```bash
  git clone https://github.com/weidai11/cryptopp
  cat cryptopp/sha_simd.cpp  # See ARM SHA usage
  ```

### Intrinsics Guide
```cpp
// Key intrinsics you'll use:

// SHA-256 hash rounds
uint32x4_t vsha256hq_u32(uint32x4_t hash_abcd, uint32x4_t hash_efgh, uint32x4_t wk);
uint32x4_t vsha256h2q_u32(uint32x4_t hash_efgh, uint32x4_t hash_abcd, uint32x4_t wk);

// Message schedule
uint32x4_t vsha256su0q_u32(uint32x4_t w0_3, uint32x4_t w4_7);
uint32x4_t vsha256su1q_u32(uint32x4_t tw0_3, uint32x4_t w8_11, uint32x4_t w12_15);

// Byte reversal (endianness)
uint8x16_t vrev32q_u8(uint8x16_t vec);  // Reverse bytes in each 32-bit word
```

---

## 💡 **Quick Start (Copy-Paste Ready)**

Want to implement this **right now**? Here's the fastest path:

1. **Copy Bitcoin Core's ARM SHA implementation:**
   ```bash
   cd /Users/haydarevich/Documents/DineroCoin
   curl -o src/crypto/sha256_arm_sha_ref.cpp \
     https://raw.githubusercontent.com/bitcoin/bitcoin/master/src/crypto/sha256_arm_shani.cpp
   ```

2. **Adapt to Dinero:**
   - Rename functions to match our naming
   - Remove Bitcoin-specific dependencies
   - Hook into `SHA256d_BlockHeader()`

3. **Update CMake:**
   ```cmake
   target_compile_options(dinero_crypto PRIVATE -march=armv8-a+crypto)
   ```

4. **Test:**
   ```bash
   cmake --build build-clean -j8
   ./build-clean/bench-simd
   ```

**Expected time if copying Bitcoin Core:** ~4-6 hours (vs 14 hours from scratch)

---

## 🎯 **Decision Matrix**

| Option | Time | Speedup | Effort | Recommendation |
|---|---|---|---|---|
| **Keep current NEON** | 0 days | 1.24x | None | ⚠️ OK for launch |
| **Implement ARM SHA** | 2 days | 3-5x | Medium | ✅ **High ROI** |
| **Copy Bitcoin Core** | 0.5 days | 3-5x | Low | ✅ **Best option** |
| **Wait for later** | N/A | 1.24x | None | ❌ Missed opportunity |

---

## 🏁 **Recommended Action**

**Option: Copy & Adapt Bitcoin Core's Implementation**

**Timeline:**
- **Day 1 (4 hours):** Copy, adapt, integrate
- **Day 2 (2 hours):** Test, benchmark, document
- **Total:** ~6 hours / 0.75 days

**Result:**
- 3-5x speedup on Apple Silicon
- Production-grade code (Bitcoin Core quality)
- Minimal risk (proven implementation)

**ROI:**
- 6 hours work → 3x network hashrate
- Best time investment for mining optimization

---

**Want me to implement this now? I can adapt Bitcoin Core's ARM SHA code in ~4-6 hours of work.** ⚡



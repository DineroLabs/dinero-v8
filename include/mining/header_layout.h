#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
//  Dinero BlockHeader v1 (128 bytes) - FROZEN
//
//  SINGLE SOURCE OF TRUTH for:
//    - C++ Consensus code
//    - Mining Coordinator
//    - Stratum Server
//    - CUDA Kernels
//    - OpenCL Kernels
//    - GPU Backends
//
//  WARNING: ANY CHANGE HERE AFFECTS ALL MINING COMPONENTS
//  If you modify this file, you MUST rebuild ALL mining code.
//
//  See: docs/BLOCKHEADER_V1_FINALIZATION_PLAN.md
// ═══════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// Header Field Offsets (in bytes)
// ─────────────────────────────────────────────────────────────────────────────
#define DINERO_HEADER_VERSION_OFFSET         0     // 4 bytes  - Block version
#define DINERO_HEADER_PREVHASH_OFFSET        4     // 32 bytes - Previous block hash
#define DINERO_HEADER_MERKLEROOT_OFFSET      36    // 32 bytes - Merkle root of transactions
#define DINERO_HEADER_UTREEXO_OFFSET         68    // 32 bytes - Utreexo accumulator root
#define DINERO_HEADER_TIMESTAMP_OFFSET       100   // 8 bytes  - Unix timestamp
#define DINERO_HEADER_DIFFICULTY_OFFSET      108   // 4 bytes  - Difficulty target (compact)
#define DINERO_HEADER_NONCE_OFFSET           112   // 4 bytes  - Mining nonce
#define DINERO_HEADER_RESERVED_OFFSET        116   // 12 bytes - Reserved (MUST be zero)

// ─────────────────────────────────────────────────────────────────────────────
// Header Field Sizes (in bytes)
// ─────────────────────────────────────────────────────────────────────────────
#define DINERO_HEADER_VERSION_SIZE           4
#define DINERO_HEADER_PREVHASH_SIZE          32
#define DINERO_HEADER_MERKLEROOT_SIZE        32
#define DINERO_HEADER_UTREEXO_SIZE           32
#define DINERO_HEADER_TIMESTAMP_SIZE         8
#define DINERO_HEADER_DIFFICULTY_SIZE        4
#define DINERO_HEADER_NONCE_SIZE             4
#define DINERO_HEADER_RESERVED_SIZE          12

// ─────────────────────────────────────────────────────────────────────────────
// Total Header Size - CONSENSUS-FINAL
// ─────────────────────────────────────────────────────────────────────────────
#define DINERO_HEADER_SIZE_BYTES             128  // CONSENSUS-FINAL
#define DINERO_HEADER_SIZE_WORDS             32   // 128 / 4 = 32 uint32_t words

// Verify layout consistency (compile-time check)
#if (DINERO_HEADER_VERSION_SIZE + DINERO_HEADER_PREVHASH_SIZE + \
     DINERO_HEADER_MERKLEROOT_SIZE + DINERO_HEADER_UTREEXO_SIZE + \
     DINERO_HEADER_TIMESTAMP_SIZE + DINERO_HEADER_DIFFICULTY_SIZE + \
     DINERO_HEADER_NONCE_SIZE + DINERO_HEADER_RESERVED_SIZE) != DINERO_HEADER_SIZE_BYTES
#error "Header field sizes do not sum to DINERO_HEADER_SIZE_BYTES (128)!"
#endif

// CONSENSUS-FINAL guard (preprocessor level)
#if DINERO_HEADER_SIZE_BYTES != 128
#error "CONSENSUS-FINAL: Header size must be 128 bytes"
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Word Offsets (for GPU kernels - index into uint32_t array)
// ─────────────────────────────────────────────────────────────────────────────
#define DINERO_HEADER_VERSION_WORD           0    // Word 0
#define DINERO_HEADER_PREVHASH_WORD          1    // Words 1-8 (32 bytes)
#define DINERO_HEADER_MERKLEROOT_WORD        9    // Words 9-16 (32 bytes)
#define DINERO_HEADER_UTREEXO_WORD           17   // Words 17-24 (32 bytes)
#define DINERO_HEADER_TIMESTAMP_WORD         25   // Words 25-26 (8 bytes)
#define DINERO_HEADER_DIFFICULTY_WORD        27   // Word 27
#define DINERO_HEADER_NONCE_WORD             28   // Word 28 <- MINERS MODIFY THIS
#define DINERO_HEADER_RESERVED_WORD          29   // Words 29-31 (12 bytes, MUST be zero)

// ─────────────────────────────────────────────────────────────────────────────
// SHA-256 Block Layout for Double-SHA256 (SHA256d)
// ─────────────────────────────────────────────────────────────────────────────
// SHA-256 processes 64-byte (512-bit) blocks.
// 128-byte header requires 2 SHA-256 blocks:
//
//   Block 1: bytes 0-63   (words 0-15)  - First 64 bytes of header
//   Block 2: bytes 64-127 (words 16-31) - Remaining 64 bytes
//
// Padding in Block 2:
//   [0-63]  : Header bytes 64-127 (64 bytes = 16 words, FULL second SHA block)
//   (No padding needed within header itself - padding happens in SHA-256 final block)
//
// For final SHA-256 pass:
//   [0-63]  : Header bytes 64-127 (64 bytes)
//   [64]    : 0x80 (padding start marker)
//   [65-125]: 0x00 (zeros)
//   [126-127]: Message length in bits (1024 = 0x400, big-endian)

#define DINERO_SHA256_BLOCK_SIZE             64   // SHA-256 block size in bytes
#define DINERO_SHA256_BLOCK1_SIZE            64   // First block: bytes 0-63
#define DINERO_SHA256_BLOCK2_SIZE            64   // Second block: bytes 64-127
#define DINERO_SHA256_BLOCK2_PAYLOAD         64   // Phase 3: Block 2 payload (full 64 bytes for 128-byte header)
#define DINERO_SHA256_TOTAL_BITS             1024 // 128 * 8 = 1024 bits

// Position of nonce within SHA-256 blocks:
// Nonce is at header offset 112 (0x70), which is byte 48 of block 2
// Block 2 starts at byte 64, so nonce is at block2[48:52]
// In words: nonce is at header word 28, which is block2 word 12 (28 - 16 = 12)
#define DINERO_SHA256_BLOCK2_NONCE_WORD      12

// ─────────────────────────────────────────────────────────────────────────────
// Comparison with Bitcoin (80-byte header) and Previous Dinero (112-byte)
// ─────────────────────────────────────────────────────────────────────────────
// Bitcoin:         80 bytes  = 20 words, nonce at byte 76,  SHA total = 640 bits
// Dinero old: 112 bytes = 28 words, nonce at byte 76,  SHA total = 896 bits
// Dinero v1:  128 bytes = 32 words, nonce at byte 112, SHA total = 1024 bits
//
// BlockHeader v1 is a clean redesign with:
// - No legacy field duplication
// - 12-byte reserved field for future extensibility
// - Cache-perfect alignment (128 = 2^7 bytes)

#define BITCOIN_HEADER_SIZE_BYTES            80
#define BITCOIN_HEADER_SIZE_WORDS            20

// ─────────────────────────────────────────────────────────────────────────────
// C++ Type Definitions (only when compiling C++)
// ─────────────────────────────────────────────────────────────────────────────
#ifdef __cplusplus

#include <cstdint>
#include <cstring>

namespace dinero {
namespace mining {

/**
 * Packed block header structure - MUST match canonical layout exactly.
 * Use this for serialization and binary operations.
 *
 * BlockHeader v1 (128 bytes) - FROZEN
 */
#pragma pack(push, 1)
struct BlockHeaderV1 {
    uint32_t version;            // 4 bytes  @ offset 0
    uint8_t  prev_block_hash[32]; // 32 bytes @ offset 4
    uint8_t  merkle_root[32];    // 32 bytes @ offset 36
    uint8_t  utreexo_root[32];   // 32 bytes @ offset 68
    uint64_t timestamp;          // 8 bytes  @ offset 100
    uint32_t difficulty;         // 4 bytes  @ offset 108
    uint32_t nonce;              // 4 bytes  @ offset 112
    uint8_t  reserved[12];       // 12 bytes @ offset 116 (MUST be zero)
};
#pragma pack(pop)

// Compile-time verification
static_assert(sizeof(BlockHeaderV1) == DINERO_HEADER_SIZE_BYTES,
    "BlockHeaderV1 size mismatch!");
static_assert(offsetof(BlockHeaderV1, version) == DINERO_HEADER_VERSION_OFFSET,
    "version offset mismatch!");
static_assert(offsetof(BlockHeaderV1, prev_block_hash) == DINERO_HEADER_PREVHASH_OFFSET,
    "prev_block_hash offset mismatch!");
static_assert(offsetof(BlockHeaderV1, merkle_root) == DINERO_HEADER_MERKLEROOT_OFFSET,
    "merkle_root offset mismatch!");
static_assert(offsetof(BlockHeaderV1, utreexo_root) == DINERO_HEADER_UTREEXO_OFFSET,
    "utreexo_root offset mismatch!");
static_assert(offsetof(BlockHeaderV1, timestamp) == DINERO_HEADER_TIMESTAMP_OFFSET,
    "timestamp offset mismatch!");
static_assert(offsetof(BlockHeaderV1, difficulty) == DINERO_HEADER_DIFFICULTY_OFFSET,
    "difficulty offset mismatch!");
static_assert(offsetof(BlockHeaderV1, nonce) == DINERO_HEADER_NONCE_OFFSET,
    "nonce offset mismatch!");
static_assert(offsetof(BlockHeaderV1, reserved) == DINERO_HEADER_RESERVED_OFFSET,
    "reserved offset mismatch!");

} // namespace mining
} // namespace dinero

#endif // __cplusplus

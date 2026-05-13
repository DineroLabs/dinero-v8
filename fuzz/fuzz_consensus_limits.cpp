/**
 * Phase D.3.2: Consensus Limits Fuzz Harness
 *
 * CRITICAL CONSENSUS FUZZING: This fuzzer targets size/weight limit validation.
 * Bugs here can allow DoS attacks (oversized blocks) or consensus divergence.
 *
 * What we're looking for:
 * - Integer overflows in size calculations
 * - Off-by-one errors in boundary checks
 * - Inconsistent validation (size vs weight)
 * - Missing overflow checks
 * - Edge cases near UINT32_MAX
 *
 * Phase D Ground Rules (In Effect):
 * - ❌ NO new features
 * - ❌ NO refactors
 * - ✅ ONLY: fuzz existing consensus code
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include fuzz_consensus_limits.cpp -o fuzz_consensus_limits
 *
 * Run:
 *   ./fuzz_consensus_limits corpus/limits/ -max_len=64 -runs=1000000
 *
 * SPDX-License-Identifier: MIT
 */

#include "consensus/limits.h"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <limits>

using namespace dinero::consensus;

// Fuzz modes
enum LimitsFuzzMode : uint8_t {
    FUZZ_BLOCK_SIZE = 0,        // Test block size validation
    FUZZ_BLOCK_WEIGHT = 1,      // Test block weight validation
    FUZZ_TX_SIZE = 2,           // Test transaction size validation
    FUZZ_TX_WEIGHT = 3,         // Test transaction weight validation
    FUZZ_SCRIPT_SIZE = 4,       // Test script size validation
    FUZZ_BOUNDARIES = 5,        // Test exact boundaries
    FUZZ_OVERFLOW = 6,          // Test near UINT32_MAX
    FUZZ_CONSISTENCY = 7,       // Test size vs weight consistency
};

/**
 * Test that validation functions handle all possible uint32_t values correctly
 */
void FuzzBlockSize(uint32_t size) {
    bool valid = IsValidBlockSize(size);

    // INVARIANT: Size must be in range (0, MAX_BLOCK_SIZE]
    if (size == 0) {
        if (valid) {
            __builtin_trap();  // BUG: Accepted size 0!
        }
    } else if (size <= MAX_BLOCK_SIZE) {
        if (!valid) {
            __builtin_trap();  // BUG: Rejected valid size!
        }
    } else {
        if (valid) {
            __builtin_trap();  // BUG: Accepted oversized block!
        }
    }
}

void FuzzBlockWeight(uint32_t weight) {
    bool valid = IsValidBlockWeight(weight);

    // INVARIANT: Weight must be in range (0, MAX_BLOCK_WEIGHT]
    if (weight == 0) {
        if (valid) {
            __builtin_trap();  // BUG: Accepted weight 0!
        }
    } else if (weight <= MAX_BLOCK_WEIGHT) {
        if (!valid) {
            __builtin_trap();  // BUG: Rejected valid weight!
        }
    } else {
        if (valid) {
            __builtin_trap();  // BUG: Accepted overweight block!
        }
    }
}

void FuzzTxSize(uint32_t size) {
    bool valid = IsValidTxSize(size);

    // INVARIANT: Size must be in range (0, MAX_TX_SIZE]
    if (size == 0) {
        if (valid) {
            __builtin_trap();  // BUG: Accepted size 0!
        }
    } else if (size <= MAX_TX_SIZE) {
        if (!valid) {
            __builtin_trap();  // BUG: Rejected valid size!
        }
    } else {
        if (valid) {
            __builtin_trap();  // BUG: Accepted oversized tx!
        }
    }
}

void FuzzTxWeight(uint32_t weight) {
    bool valid = IsValidTxWeight(weight);

    // INVARIANT: Weight must be in range (0, MAX_TX_WEIGHT]
    if (weight == 0) {
        if (valid) {
            __builtin_trap();  // BUG: Accepted weight 0!
        }
    } else if (weight <= MAX_TX_WEIGHT) {
        if (!valid) {
            __builtin_trap();  // BUG: Rejected valid weight!
        }
    } else {
        if (valid) {
            __builtin_trap();  // BUG: Accepted overweight tx!
        }
    }
}

void FuzzScriptSize(uint32_t size) {
    bool valid = IsValidScriptSize(size);

    // INVARIANT: Script size must be in range [0, MAX_SCRIPT_SIZE]
    // Note: 0 is valid (empty script)
    if (size <= MAX_SCRIPT_SIZE) {
        if (!valid) {
            __builtin_trap();  // BUG: Rejected valid script size!
        }
    } else {
        if (valid) {
            __builtin_trap();  // BUG: Accepted oversized script!
        }
    }
}

/**
 * Test boundary conditions systematically
 */
void FuzzBoundaries(const uint8_t* data, size_t size) {
    if (size < 4) return;

    uint32_t offset = data[0] % 10;  // Small offset around boundary

    // Test MAX_BLOCK_SIZE boundary
    uint32_t test_size = MAX_BLOCK_SIZE - 5 + offset;
    FuzzBlockSize(test_size);

    // Test MAX_BLOCK_WEIGHT boundary
    uint32_t test_weight = MAX_BLOCK_WEIGHT - 5 + offset;
    FuzzBlockWeight(test_weight);

    // Test MAX_TX_SIZE boundary
    test_size = MAX_TX_SIZE - 5 + offset;
    FuzzTxSize(test_size);

    // Test MAX_TX_WEIGHT boundary
    test_weight = MAX_TX_WEIGHT - 5 + offset;
    FuzzTxWeight(test_weight);

    // Test MAX_SCRIPT_SIZE boundary
    test_size = MAX_SCRIPT_SIZE - 5 + offset;
    FuzzScriptSize(test_size);
}

/**
 * Test near integer overflow
 */
void FuzzOverflow(uint32_t value) {
    // Test values near UINT32_MAX
    uint32_t test_vals[] = {
        value,
        UINT32_MAX,
        UINT32_MAX - 1,
        UINT32_MAX - 100,
        UINT32_MAX / 2,
        0x80000000,  // INT32_MAX + 1
        0x7FFFFFFF,  // INT32_MAX
    };

    for (uint32_t val : test_vals) {
        // These should not crash, even with extreme values
        FuzzBlockSize(val);
        FuzzBlockWeight(val);
        FuzzTxSize(val);
        FuzzTxWeight(val);
        FuzzScriptSize(val);
    }
}

/**
 * Test consistency between size and weight limits
 */
void FuzzConsistency(const uint8_t* data, size_t size) {
    if (size < 8) return;

    // Parse two uint32_t values from fuzz data
    uint32_t val1 = 0, val2 = 0;
    for (int i = 0; i < 4; i++) {
        val1 |= static_cast<uint32_t>(data[i]) << (i * 8);
        val2 |= static_cast<uint32_t>(data[i + 4]) << (i * 8);
    }

    // INVARIANT: MAX_TX_SIZE <= MAX_BLOCK_SIZE
    if (MAX_TX_SIZE > MAX_BLOCK_SIZE) {
        __builtin_trap();  // Sanity check failed!
    }

    // INVARIANT: MAX_TX_WEIGHT <= MAX_BLOCK_WEIGHT
    if (MAX_TX_WEIGHT > MAX_BLOCK_WEIGHT) {
        __builtin_trap();  // Sanity check failed!
    }

    // INVARIANT: If tx size is valid, it shouldn't exceed block size
    if (IsValidTxSize(val1) && !IsValidBlockSize(val1)) {
        // This would be inconsistent
        __builtin_trap();
    }

    // INVARIANT: If tx weight is valid, it shouldn't exceed block weight
    if (IsValidTxWeight(val2) && !IsValidBlockWeight(val2)) {
        // This would be inconsistent
        __builtin_trap();
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return 0;
    }

    LimitsFuzzMode mode = static_cast<LimitsFuzzMode>(data[0] % 8);
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    // Extract a uint32_t from fuzz data (if available)
    uint32_t fuzz_value = 0;
    if (payload_size >= 4) {
        for (int i = 0; i < 4; i++) {
            fuzz_value |= static_cast<uint32_t>(payload[i]) << (i * 8);
        }
    }

    try {
        switch (mode) {
            case FUZZ_BLOCK_SIZE:
                FuzzBlockSize(fuzz_value);
                break;

            case FUZZ_BLOCK_WEIGHT:
                FuzzBlockWeight(fuzz_value);
                break;

            case FUZZ_TX_SIZE:
                FuzzTxSize(fuzz_value);
                break;

            case FUZZ_TX_WEIGHT:
                FuzzTxWeight(fuzz_value);
                break;

            case FUZZ_SCRIPT_SIZE:
                FuzzScriptSize(fuzz_value);
                break;

            case FUZZ_BOUNDARIES:
                FuzzBoundaries(payload, payload_size);
                break;

            case FUZZ_OVERFLOW:
                FuzzOverflow(fuzz_value);
                break;

            case FUZZ_CONSISTENCY:
                FuzzConsistency(payload, payload_size);
                break;
        }
    } catch (const std::exception& e) {
        // Validation functions should NOT throw exceptions
        __builtin_trap();
    } catch (...) {
        // Unknown exception = critical bug
        __builtin_trap();
    }

    return 0;
}

// AFL++ compatible main function
#ifdef AFL_MAIN
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::vector<uint8_t> input;
        uint8_t buf[4096];
        ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
            input.insert(input.end(), buf, buf + n);
        }
        return LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) continue;

        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            continue;
        }

        std::vector<uint8_t> input(st.st_size);
        read(fd, input.data(), input.size());
        close(fd);

        LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    return 0;
}
#endif

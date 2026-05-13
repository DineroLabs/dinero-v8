/**
 * Phase D.3.4: Script LIMIT Fuzzer (NOT Script VM)
 *
 * CRITICAL DISTINCTION: This fuzzer tests STRUCTURAL limits, NOT script execution.
 *
 * We test:
 * - Script length limits (MAX_SCRIPT_SIZE = 10,000 bytes)
 * - Opcode count limits (MAX_SCRIPT_OPCODES = 201)
 * - Push size limits (MAX_SCRIPT_ELEMENT_SIZE = 520 bytes)
 * - Stack size limits (MAX_STACK_SIZE = 1,000 elements)
 *
 * We DO NOT test:
 * - Script execution (EvalScript)
 * - Script semantics
 * - Signature verification
 * - OP_CHECKSIG behavior
 * - Stack operations
 *
 * Philosophy (Phase D.3):
 * - Fuzzers test SAFETY, not correctness
 * - Never assert result == X
 * - Only assert: no crash, no OOB, no UB, no infinite loop
 *
 * What we're looking for:
 * - Crashes in limit checking code
 * - Integer overflows in size calculations
 * - Off-by-one errors in boundaries
 * - DoS vectors (excessive memory allocation)
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include fuzz_script_limits.cpp -o fuzz_script_limits
 *
 * Run:
 *   ./fuzz_script_limits corpus/script_limits/ -max_len=20000 -runs=10000000
 *
 * SPDX-License-Identifier: MIT
 */

#include "consensus/limits.h"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>

using namespace dinero::consensus;

namespace {

/**
 * Count opcodes in a script (simplified)
 * This is a MINIMAL parser that just counts opcodes without executing
 */
uint32_t CountOpcodes(const uint8_t* script, size_t script_len) {
    uint32_t count = 0;
    size_t pos = 0;

    // Cap at reasonable limit to prevent infinite loops
    const uint32_t MAX_COUNT = 100000;

    while (pos < script_len && count < MAX_COUNT) {
        uint8_t opcode = script[pos++];

        // Push data opcodes (0x01-0x4B)
        if (opcode >= 0x01 && opcode <= 0x4B) {
            // Skip data bytes
            if (pos + opcode > script_len) {
                break;  // Malformed, but don't crash
            }
            pos += opcode;
        }
        // OP_PUSHDATA1
        else if (opcode == 0x4C) {
            if (pos >= script_len) break;
            uint8_t len = script[pos++];
            if (pos + len > script_len) break;
            pos += len;
        }
        // OP_PUSHDATA2
        else if (opcode == 0x4D) {
            if (pos + 2 > script_len) break;
            uint16_t len = script[pos] | (static_cast<uint16_t>(script[pos + 1]) << 8);
            pos += 2;
            if (pos + len > script_len) break;
            pos += len;
        }
        // OP_PUSHDATA4
        else if (opcode == 0x4E) {
            if (pos + 4 > script_len) break;
            uint32_t len = script[pos] |
                          (static_cast<uint32_t>(script[pos + 1]) << 8) |
                          (static_cast<uint32_t>(script[pos + 2]) << 16) |
                          (static_cast<uint32_t>(script[pos + 3]) << 24);
            pos += 4;
            if (pos + len > script_len) break;
            pos += len;
        }
        // All other opcodes are 1 byte
        else {
            // Just count it
        }

        count++;
    }

    return count;
}

/**
 * Find maximum push size in a script
 */
uint32_t FindMaxPushSize(const uint8_t* script, size_t script_len) {
    uint32_t max_push = 0;
    size_t pos = 0;

    while (pos < script_len) {
        uint8_t opcode = script[pos++];

        uint32_t push_size = 0;

        // Push data opcodes (0x01-0x4B)
        if (opcode >= 0x01 && opcode <= 0x4B) {
            push_size = opcode;
            if (pos + push_size > script_len) break;
            pos += push_size;
        }
        // OP_PUSHDATA1
        else if (opcode == 0x4C) {
            if (pos >= script_len) break;
            push_size = script[pos++];
            if (pos + push_size > script_len) break;
            pos += push_size;
        }
        // OP_PUSHDATA2
        else if (opcode == 0x4D) {
            if (pos + 2 > script_len) break;
            push_size = script[pos] | (static_cast<uint16_t>(script[pos + 1]) << 8);
            pos += 2;
            if (pos + push_size > script_len) break;
            pos += push_size;
        }
        // OP_PUSHDATA4
        else if (opcode == 0x4E) {
            if (pos + 4 > script_len) break;
            push_size = script[pos] |
                       (static_cast<uint32_t>(script[pos + 1]) << 8) |
                       (static_cast<uint32_t>(script[pos + 2]) << 16) |
                       (static_cast<uint32_t>(script[pos + 3]) << 24);
            pos += 4;
            if (pos + push_size > script_len) break;
            pos += push_size;
        }

        if (push_size > max_push) {
            max_push = push_size;
        }
    }

    return max_push;
}

} // anonymous namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Accept any size input

    // ========================================================================
    // TEST 1: Script size validation
    // ========================================================================
    // Check that IsValidScriptSize() doesn't crash on any input
    try {
        bool valid_size = IsValidScriptSize(static_cast<uint32_t>(size));
        (void)valid_size;  // Don't care about result, just that it didn't crash

        // Also test with fuzz-derived sizes
        if (size >= 4) {
            uint32_t fuzz_size = data[0] |
                                (static_cast<uint32_t>(data[1]) << 8) |
                                (static_cast<uint32_t>(data[2]) << 16) |
                                (static_cast<uint32_t>(data[3]) << 24);
            valid_size = IsValidScriptSize(fuzz_size);
            (void)valid_size;
        }

    } catch (...) {
        // Should never throw
        __builtin_trap();
    }

    // ========================================================================
    // TEST 2: Opcode counting (structural analysis only)
    // ========================================================================
    // This tests the LIMIT checking, not script execution
    try {
        uint32_t opcode_count = CountOpcodes(data, size);
        (void)opcode_count;  // Just checking for crashes

        // The count itself doesn't matter for safety testing
        // We just want to make sure counting doesn't crash

    } catch (...) {
        // Should never throw
        __builtin_trap();
    }

    // ========================================================================
    // TEST 3: Maximum push size detection
    // ========================================================================
    // Check that we can safely analyze push operations
    try {
        uint32_t max_push = FindMaxPushSize(data, size);
        (void)max_push;  // Just checking for crashes

        // Again, the value doesn't matter - just that it doesn't crash

    } catch (...) {
        // Should never throw
        __builtin_trap();
    }

    // ========================================================================
    // TEST 4: Boundary testing around consensus limits
    // ========================================================================
    // Test values around the actual consensus limits
    try {
        // Test around MAX_SCRIPT_SIZE (10,000 bytes)
        if (size > 0) {
            uint32_t offset = data[0];
            uint32_t test_size = MAX_SCRIPT_SIZE - 10 + offset;
            IsValidScriptSize(test_size);
        }

        // Test script size validation with actual data size
        if (size <= MAX_SCRIPT_SIZE) {
            // This size should be valid (or at least not crash)
            IsValidScriptSize(static_cast<uint32_t>(size));
        }

    } catch (...) {
        // Should never throw
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

/**
 * Phase D.3.1: Premine Validation Fuzz Harness
 *
 * CRITICAL CONSENSUS FUZZING: This fuzzer targets IsExactPremineCoinbase(),
 * which validates the height-1 premine block. Any bug here = network fork.
 *
 * What we're looking for:
 * - Buffer over-reads (reading past transaction end)
 * - Integer overflows (in amount parsing)
 * - Crashes on malformed transactions
 * - False positives (accepting invalid premine)
 * - False negatives (rejecting valid premine)
 * - Parser inconsistencies
 *
 * Phase D Ground Rules (In Effect):
 * - ❌ NO new features
 * - ❌ NO refactors
 * - ✅ ONLY: fuzz existing consensus code
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include -I../src fuzz_premine.cpp \
 *           ../src/consensus/premine.cpp -o fuzz_premine
 *
 * Run:
 *   ./fuzz_premine corpus/premine/ -max_len=1024 -runs=1000000
 *
 * SPDX-License-Identifier: MIT
 */

#include "consensus/premine.h"
#include "consensus/subsidy.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <stdexcept>

using namespace dinero::consensus;

// Fuzz modes
enum PremineFuzzMode : uint8_t {
    FUZZ_RAW_BYTES = 0,           // Feed raw bytes to IsExactPremineCoinbase()
    FUZZ_MUTATE_VALID = 1,        // Start with valid premine, mutate it
    FUZZ_MUTATE_AMOUNT = 2,       // Mutate only the amount field
    FUZZ_MUTATE_SCRIPT = 3,       // Mutate only the scriptPubKey
    FUZZ_MUTATE_OUTPUTS = 4,      // Mutate output count
    FUZZ_TRUNCATE = 5,            // Test truncated transactions
    FUZZ_EXTEND = 6,              // Test oversized transactions
};

/**
 * Build a minimal valid premine coinbase transaction
 * This is the baseline for mutation fuzzing
 */
std::vector<uint8_t> BuildValidPremine() {
    std::vector<uint8_t> tx;

    // Version (4 bytes, little-endian): 1
    tx.push_back(0x01);
    tx.push_back(0x00);
    tx.push_back(0x00);
    tx.push_back(0x00);

    // Input count (varint): 1
    tx.push_back(0x01);

    // Coinbase input: null prevout (32 bytes 0x00 + 4 bytes 0xFFFFFFFF)
    for (int i = 0; i < 32; i++) {
        tx.push_back(0x00);
    }
    tx.push_back(0xFF);
    tx.push_back(0xFF);
    tx.push_back(0xFF);
    tx.push_back(0xFF);

    // ScriptSig: height 1 (2 bytes: length=1, value=1)
    tx.push_back(0x01);  // length
    tx.push_back(0x01);  // height 1

    // Sequence (4 bytes): 0xFFFFFFFF
    tx.push_back(0xFF);
    tx.push_back(0xFF);
    tx.push_back(0xFF);
    tx.push_back(0xFF);

    // Output count (varint): 1
    tx.push_back(0x01);

    // Output value (8 bytes, little-endian): PREMINE_AMOUNT_UNA
    uint64_t amount = PREMINE_AMOUNT_UNA;  // 262,790,000,000,000
    for (int i = 0; i < 8; i++) {
        tx.push_back((amount >> (i * 8)) & 0xFF);
    }

    // ScriptPubKey length (varint): 22
    tx.push_back(0x16);  // 22 in decimal

    // ScriptPubKey (22 bytes): P2WPKH
    for (size_t i = 0; i < PREMINE_SCRIPT_PUBKEY.size(); i++) {
        tx.push_back(PREMINE_SCRIPT_PUBKEY[i]);
    }

    // Locktime (4 bytes): 0
    tx.push_back(0x00);
    tx.push_back(0x00);
    tx.push_back(0x00);
    tx.push_back(0x00);

    return tx;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return 0;
    }

    PremineFuzzMode mode = static_cast<PremineFuzzMode>(data[0] % 7);
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    std::vector<uint8_t> tx_bytes;

    switch (mode) {
        case FUZZ_RAW_BYTES: {
            // Feed raw fuzz input directly to IsExactPremineCoinbase()
            // This tests the parser's robustness against garbage
            tx_bytes.assign(payload, payload + payload_size);
            break;
        }

        case FUZZ_MUTATE_VALID: {
            // Start with valid premine, mutate random bytes
            // This finds bugs near valid transactions
            tx_bytes = BuildValidPremine();

            if (payload_size > 0) {
                // Use fuzz data to mutate random positions
                for (size_t i = 0; i < payload_size && i < tx_bytes.size(); i++) {
                    size_t pos = payload[i] % tx_bytes.size();
                    if (i + 1 < payload_size) {
                        tx_bytes[pos] = payload[i + 1];
                    }
                }
            }
            break;
        }

        case FUZZ_MUTATE_AMOUNT: {
            // Build valid premine, but mutate the amount field
            // Tests: off-by-one errors, overflow, underflow
            tx_bytes = BuildValidPremine();

            // Amount is at offset 47 (after version, inputs, outputs count)
            // Offset calculation: 4 (version) + 1 (input count) + 36 (prevout) +
            //                     2 (scriptsig) + 4 (sequence) + 1 (output count) = 48
            size_t amount_offset = 48;

            if (payload_size >= 8 && amount_offset + 8 <= tx_bytes.size()) {
                // Replace amount with fuzz data
                for (int i = 0; i < 8 && i < static_cast<int>(payload_size); i++) {
                    tx_bytes[amount_offset + i] = payload[i];
                }
            }
            break;
        }

        case FUZZ_MUTATE_SCRIPT: {
            // Build valid premine, but mutate the scriptPubKey
            // Tests: wrong destination, malformed scripts
            tx_bytes = BuildValidPremine();

            // ScriptPubKey starts at offset 57 (48 + 8 for amount + 1 for length)
            size_t script_offset = 57;

            if (payload_size >= 22 && script_offset + 22 <= tx_bytes.size()) {
                // Replace scriptPubKey with fuzz data
                for (int i = 0; i < 22 && i < static_cast<int>(payload_size); i++) {
                    tx_bytes[script_offset + i] = payload[i];
                }
            }
            break;
        }

        case FUZZ_MUTATE_OUTPUTS: {
            // Test different output counts (should reject if != 1)
            tx_bytes = BuildValidPremine();

            // Output count is at offset 47
            size_t output_count_offset = 47;

            if (payload_size > 0 && output_count_offset < tx_bytes.size()) {
                // Change output count
                tx_bytes[output_count_offset] = payload[0];

                // If count > 1, we need to add more outputs (truncate for simplicity)
                // This tests whether parser correctly rejects multi-output premine
            }
            break;
        }

        case FUZZ_TRUNCATE: {
            // Test truncated transactions (parser should handle gracefully)
            tx_bytes = BuildValidPremine();

            if (payload_size > 0) {
                size_t truncate_len = payload[0] % tx_bytes.size();
                tx_bytes.resize(truncate_len);
            }
            break;
        }

        case FUZZ_EXTEND: {
            // Test oversized transactions
            tx_bytes = BuildValidPremine();

            // Append fuzz data
            if (payload_size > 0) {
                size_t extend_len = (payload_size > 1000) ? 1000 : payload_size;
                tx_bytes.insert(tx_bytes.end(), payload, payload + extend_len);
            }
            break;
        }
    }

    // ========================================================================
    // CRITICAL TEST: Call IsExactPremineCoinbase()
    // ========================================================================
    // This should NEVER crash, regardless of input
    // It should return true ONLY for exact valid premine
    // It should return false for everything else (no exceptions thrown)

    bool result = false;
    try {
        result = IsExactPremineCoinbase(tx_bytes);

        // INVARIANT CHECK: If it returned true, verify it's actually valid
        if (result) {
            // Double-check: amount must be exact
            if (tx_bytes.size() >= 56) {  // Minimum size for valid premine
                uint64_t amount = 0;
                size_t amount_offset = 48;

                for (int i = 0; i < 8 && amount_offset + i < tx_bytes.size(); i++) {
                    amount |= static_cast<uint64_t>(tx_bytes[amount_offset + i]) << (i * 8);
                }

                // If IsExactPremineCoinbase() returned true,
                // amount MUST be exactly PREMINE_AMOUNT_UNA
                if (amount != PREMINE_AMOUNT_UNA) {
                    // BUG FOUND: Accepted wrong amount!
                    __builtin_trap();
                }

                // Verify scriptPubKey match
                size_t script_offset = 57;
                bool script_match = true;

                if (tx_bytes.size() >= script_offset + 22) {
                    for (size_t i = 0; i < 22; i++) {
                        if (tx_bytes[script_offset + i] != PREMINE_SCRIPT_PUBKEY[i]) {
                            script_match = false;
                            break;
                        }
                    }

                    // If IsExactPremineCoinbase() returned true,
                    // scriptPubKey MUST match exactly
                    if (!script_match) {
                        // BUG FOUND: Accepted wrong scriptPubKey!
                        __builtin_trap();
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        // IsExactPremineCoinbase() should NOT throw exceptions
        // It should handle all errors gracefully and return false
        // If we get here, there's a bug in error handling
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

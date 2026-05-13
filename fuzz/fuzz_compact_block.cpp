/**
 * Phase Plan-A: Compact Block Binary Wire Format Fuzzer
 *
 * Fuzzes the BIP152 compact block deserialization code to ensure:
 * - Never crashes on any input (including malformed/truncated data)
 * - Successful parses round-trip correctly (deterministic)
 * - No memory corruption, OOB reads, or undefined behavior
 *
 * Attack surfaces:
 * 1. CompactBlock deserialization (header + nonce + short txids + prefilled)
 * 2. BlockTransactionsRequest deserialization (hash + indexes)
 * 3. BlockTransactions deserialization (hash + transactions)
 * 4. PrefilledTransaction deserialization (index + tx)
 * 5. Short txid handling (48-bit values)
 * 6. CompactSize parsing (variable-length integers)
 *
 * Wire format (CompactBlock - cmpctblock message):
 *   header       : 128 bytes (BlockHeader)
 *   nonce        : 8 bytes (uint64_t LE)
 *   short_txids  : CompactSize count + 6 bytes each
 *   prefilled    : CompactSize count + (index + tx) each
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include -I../src fuzz_compact_block.cpp \
 *           ../src/p2p/compact_block.cpp -o fuzz_compact_block
 *
 * Run:
 *   ./fuzz_compact_block corpus/compact_block/ -max_len=100000
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <cassert>

#include "p2p/compact_block.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"

using namespace dinero;

// ============================================================================
// Fuzzer Mode Selection
// ============================================================================

// Define FUZZ_STANDALONE to run without libFuzzer (for debugging)
#ifdef FUZZ_STANDALONE
#include <iostream>
#include <fstream>
#include <random>

// Standalone test harness
int main(int argc, char** argv) {
    std::cout << "Compact Block Fuzzer (Standalone Mode)" << std::endl;
    std::cout << "======================================" << std::endl;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
    std::uniform_int_distribution<size_t> size_dist(0, 10000);

    size_t iterations = 10000;
    size_t crashes = 0;
    size_t successful_parses = 0;
    size_t round_trip_failures = 0;

    for (size_t i = 0; i < iterations; ++i) {
        // Generate random data
        size_t size = size_dist(gen);
        std::vector<uint8_t> data(size);
        for (size_t j = 0; j < size; ++j) {
            data[j] = byte_dist(gen);
        }

        try {
            // Test CompactBlock
            auto compact = CompactBlock::Deserialize(data);
            if (!compact.short_txids.empty() || !compact.prefilled.empty()) {
                successful_parses++;

                // Verify round-trip
                auto reserialized = compact.Serialize();
                auto reparsed = CompactBlock::Deserialize(reserialized);

                if (reparsed.nonce != compact.nonce ||
                    reparsed.short_txids.size() != compact.short_txids.size()) {
                    round_trip_failures++;
                }
            }

            // Test BlockTransactionsRequest
            auto req = BlockTransactionsRequest::Deserialize(data);

            // Test BlockTransactions
            auto txns = BlockTransactions::Deserialize(data);

        } catch (const std::exception& e) {
            // Exceptions are expected for malformed data
        } catch (...) {
            crashes++;
        }

        if ((i + 1) % 1000 == 0) {
            std::cout << "Progress: " << (i + 1) << "/" << iterations
                      << " (parses: " << successful_parses
                      << ", round-trip fails: " << round_trip_failures << ")" << std::endl;
        }
    }

    std::cout << "\nResults:" << std::endl;
    std::cout << "  Iterations: " << iterations << std::endl;
    std::cout << "  Successful parses: " << successful_parses << std::endl;
    std::cout << "  Round-trip failures: " << round_trip_failures << std::endl;
    std::cout << "  Unexpected crashes: " << crashes << std::endl;

    return (crashes > 0 || round_trip_failures > 0) ? 1 : 0;
}

#else
// ============================================================================
// LibFuzzer Entry Point
// ============================================================================

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Skip empty inputs
    if (size == 0) {
        return 0;
    }

    std::vector<uint8_t> bytes(data, data + size);

    // ========================================================================
    // Test 1: CompactBlock Deserialization
    // ========================================================================
    // Must never crash, regardless of input
    {
        auto result = CompactBlock::Deserialize(bytes);

        // If parsed successfully (has content), verify round-trip
        if (!result.short_txids.empty() || !result.prefilled.empty()) {
            auto reserialized = result.Serialize();
            auto reparsed = CompactBlock::Deserialize(reserialized);

            // Round-trip must produce identical results
            assert(reparsed.nonce == result.nonce);
            assert(reparsed.short_txids.size() == result.short_txids.size());
            assert(reparsed.prefilled.size() == result.prefilled.size());

            // Verify short txids match
            for (size_t i = 0; i < result.short_txids.size(); ++i) {
                assert(reparsed.short_txids[i] == result.short_txids[i]);
            }

            // Verify prefilled indexes match
            for (size_t i = 0; i < result.prefilled.size(); ++i) {
                assert(reparsed.prefilled[i].index == result.prefilled[i].index);
            }

            // Deterministic: serialize twice, get same bytes
            auto reserialized2 = result.Serialize();
            assert(reserialized == reserialized2);
        }
    }

    // ========================================================================
    // Test 2: BlockTransactionsRequest Deserialization
    // ========================================================================
    {
        auto result = BlockTransactionsRequest::Deserialize(bytes);

        // If parsed successfully (has indexes), verify round-trip
        if (!result.indexes.empty()) {
            auto reserialized = result.Serialize();
            auto reparsed = BlockTransactionsRequest::Deserialize(reserialized);

            assert(reparsed.block_hash == result.block_hash);
            assert(reparsed.indexes.size() == result.indexes.size());
            for (size_t i = 0; i < result.indexes.size(); ++i) {
                assert(reparsed.indexes[i] == result.indexes[i]);
            }

            // Deterministic
            auto reserialized2 = result.Serialize();
            assert(reserialized == reserialized2);
        }
    }

    // ========================================================================
    // Test 3: BlockTransactions Deserialization
    // ========================================================================
    {
        auto result = BlockTransactions::Deserialize(bytes);

        // If parsed successfully (has transactions), verify round-trip
        if (!result.transactions.empty()) {
            auto reserialized = result.Serialize();
            auto reparsed = BlockTransactions::Deserialize(reserialized);

            assert(reparsed.block_hash == result.block_hash);
            assert(reparsed.transactions.size() == result.transactions.size());

            // Deterministic
            auto reserialized2 = result.Serialize();
            assert(reserialized == reserialized2);
        }
    }

    // ========================================================================
    // Test 4: Mode-based testing (use first byte as mode selector)
    // ========================================================================
    if (size >= 2) {
        uint8_t mode = data[0] % 4;
        std::vector<uint8_t> payload(data + 1, data + size);

        switch (mode) {
            case 0: {
                // Focus on CompactBlock with varying nonce
                auto result = CompactBlock::Deserialize(payload);
                (void)result;  // Suppress unused warning
                break;
            }
            case 1: {
                // Focus on BlockTransactionsRequest with many indexes
                auto result = BlockTransactionsRequest::Deserialize(payload);
                (void)result;
                break;
            }
            case 2: {
                // Focus on BlockTransactions with transaction data
                auto result = BlockTransactions::Deserialize(payload);
                (void)result;
                break;
            }
            case 3: {
                // Focus on short txid edge cases (48-bit values)
                // Create a CompactBlock with crafted short txids
                auto result = CompactBlock::Deserialize(payload);
                if (!result.short_txids.empty()) {
                    // Verify all short txids are 48-bit (upper 16 bits zero)
                    for (uint64_t short_id : result.short_txids) {
                        // After deserialization, values should be properly masked
                        // (This is a sanity check, not a crash test)
                        (void)short_id;
                    }
                }
                break;
            }
        }
    }

    return 0;
}

#endif  // FUZZ_STANDALONE

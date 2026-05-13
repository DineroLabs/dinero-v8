/**
 * Phase D.3.3: Monetary Validation Fuzz Harness
 *
 * CRITICAL CONSENSUS FUZZING: This fuzzer targets monetary amount validation.
 * Bugs here can allow inflation (amounts > MAX_MONEY) or other monetary exploits.
 *
 * What we're looking for:
 * - Integer overflows in amount checks
 * - Off-by-one errors at MAX_MONEY boundary
 * - Inconsistent validation (IsValidAmount vs IsValidAmountOrZero)
 * - Overflow in subsidy calculations
 * - Wrong MAX_MONEY constant (should be 265.428M DIN, NOT 21M!)
 *
 * Phase D Ground Rules (In Effect):
 * - ❌ NO new features
 * - ❌ NO refactors
 * - ✅ ONLY: fuzz existing consensus code
 *
 * Build with libFuzzer:
 *   clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
 *           -I../include fuzz_monetary_validation.cpp -o fuzz_monetary_validation
 *
 * Run:
 *   ./fuzz_monetary_validation corpus/monetary/ -max_len=128 -runs=1000000
 *
 * SPDX-License-Identifier: MIT
 */

#include "consensus/tx_validation.h"
#include "consensus/subsidy.h"
#include "consensus/premine.h"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <limits>

using namespace dinero;
using namespace dinero::consensus;

// Fuzz modes
enum MonetaryFuzzMode : uint8_t {
    FUZZ_IS_VALID_AMOUNT = 0,          // Test IsValidAmount()
    FUZZ_IS_VALID_AMOUNT_OR_ZERO = 1,  // Test IsValidAmountOrZero()
    FUZZ_MAX_MONEY_BOUNDARY = 2,       // Test MAX_MONEY boundary
    FUZZ_SUBSIDY_CALCULATION = 3,      // Test GetBlockSubsidy()
    FUZZ_OVERFLOW = 4,                 // Test near UINT64_MAX
    FUZZ_PREMINE_AMOUNT = 5,           // Test premine amount consistency
    FUZZ_SUPPLY_INTEGRITY = 6,         // Test total supply constraints
};

/**
 * Test IsValidAmount() with all possible values
 */
void FuzzIsValidAmount(uint64_t amount) {
    bool valid = IsValidAmount(amount);

    // INVARIANT: Amount must be in range (0, MAX_MONEY]
    if (amount == 0) {
        if (valid) {
            __builtin_trap();  // BUG: Accepted 0 amount!
        }
    } else if (amount <= MAX_MONEY) {
        if (!valid) {
            __builtin_trap();  // BUG: Rejected valid amount!
        }
    } else {
        if (valid) {
            __builtin_trap();  // BUG: Accepted amount > MAX_MONEY! INFLATION!
        }
    }
}

/**
 * Test IsValidAmountOrZero() with all possible values
 */
void FuzzIsValidAmountOrZero(uint64_t amount) {
    bool valid = IsValidAmountOrZero(amount);

    // INVARIANT: Amount must be in range [0, MAX_MONEY]
    if (amount <= MAX_MONEY) {
        if (!valid) {
            __builtin_trap();  // BUG: Rejected valid amount!
        }
    } else {
        if (valid) {
            __builtin_trap();  // BUG: Accepted amount > MAX_MONEY! INFLATION!
        }
    }
}

/**
 * Test MAX_MONEY boundary systematically
 */
void FuzzMaxMoneyBoundary(uint64_t offset) {
    // CRITICAL: Verify MAX_MONEY is Dinero's supply, NOT Bitcoin's!
    constexpr uint64_t BITCOIN_MAX_MONEY = 2100000000000000ULL;  // 21M BTC
    constexpr uint64_t DINERO_MAX_MONEY = 26542800000000000ULL;  // 265.428M DIN

    if (MAX_MONEY == BITCOIN_MAX_MONEY) {
        // CRITICAL BUG: MAX_MONEY is Bitcoin's supply!
        __builtin_trap();
    }

    if (MAX_MONEY != DINERO_MAX_MONEY) {
        // CRITICAL BUG: MAX_MONEY is wrong!
        __builtin_trap();
    }

    // Test values around MAX_MONEY boundary
    uint64_t test_vals[] = {
        MAX_MONEY - 10,
        MAX_MONEY - 1,
        MAX_MONEY,
        MAX_MONEY + 1,
        MAX_MONEY + 10,
        MAX_MONEY + offset,
    };

    for (uint64_t val : test_vals) {
        FuzzIsValidAmount(val);
        FuzzIsValidAmountOrZero(val);
    }
}

/**
 * Test GetBlockSubsidy() for all heights
 */
void FuzzSubsidyCalculation(uint32_t height) {
    uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(height);

    // INVARIANT: Subsidy must never exceed MAX_MONEY
    if (subsidy > MAX_MONEY) {
        __builtin_trap();  // BUG: Subsidy exceeds MAX_MONEY!
    }

    // INVARIANT: Specific height checks
    if (height == 0) {
        // Genesis block has 0 subsidy
        if (subsidy != 0) {
            __builtin_trap();  // BUG: Genesis subsidy != 0!
        }
    } else if (height == 1) {
        // Premine block
        if (subsidy != ConsensusSubsidy::PREMINE_UNA) {
            __builtin_trap();  // BUG: Premine subsidy mismatch!
        }
    } else if (height == 2) {
        // First PoW block
        if (subsidy != ConsensusSubsidy::INITIAL_SUBSIDY) {
            __builtin_trap();  // BUG: Initial subsidy mismatch!
        }
    }

    // INVARIANT: Subsidy should decrease over time (after first halving)
    if (height >= 2 + ConsensusSubsidy::HALVING_INTERVAL * 2) {
        uint64_t prev_subsidy = ConsensusSubsidy::GetBlockSubsidy(height - ConsensusSubsidy::HALVING_INTERVAL);
        if (subsidy > prev_subsidy) {
            // Subsidy should not increase after halving
            __builtin_trap();
        }
    }

    // INVARIANT: After 33 halvings, subsidy must be 0
    if (height >= 2 + (33 * ConsensusSubsidy::HALVING_INTERVAL)) {
        if (subsidy != 0) {
            __builtin_trap();  // BUG: Subsidy not 0 after 33 halvings!
        }
    }
}

/**
 * Test overflow conditions near UINT64_MAX
 */
void FuzzOverflow(uint64_t value) {
    // Test values near UINT64_MAX
    uint64_t test_vals[] = {
        value,
        UINT64_MAX,
        UINT64_MAX - 1,
        UINT64_MAX - 100,
        UINT64_MAX / 2,
        0x8000000000000000ULL,  // INT64_MAX + 1
        0x7FFFFFFFFFFFFFFFULL,  // INT64_MAX
    };

    for (uint64_t val : test_vals) {
        // These should not crash, even with extreme values
        FuzzIsValidAmount(val);
        FuzzIsValidAmountOrZero(val);

        // All should be rejected (way over MAX_MONEY)
        if (val > MAX_MONEY) {
            if (IsValidAmount(val)) {
                __builtin_trap();  // BUG: Accepted huge amount!
            }
            if (!IsValidAmountOrZero(0) || (val > 0 && IsValidAmountOrZero(val))) {
                if (val != 0 && IsValidAmountOrZero(val)) {
                    __builtin_trap();  // BUG: Accepted huge amount!
                }
            }
        }
    }
}

/**
 * Test premine amount consistency across modules
 */
void FuzzPremineAmount() {
    // INVARIANT: PREMINE_AMOUNT_UNA must match across all modules
    if (PREMINE_AMOUNT_UNA != ConsensusSubsidy::PREMINE_UNA) {
        __builtin_trap();  // BUG: Premine amount mismatch!
    }

    // INVARIANT: Premine must be valid amount
    if (!IsValidAmount(PREMINE_AMOUNT_UNA)) {
        __builtin_trap();  // BUG: Premine amount invalid!
    }

    // INVARIANT: Premine must be less than MAX_MONEY
    if (PREMINE_AMOUNT_UNA >= MAX_MONEY) {
        __builtin_trap();  // BUG: Premine >= MAX_MONEY!
    }

    // INVARIANT: Premine should be roughly 1% of MAX_MONEY
    uint64_t one_percent = MAX_MONEY / 100;
    if (PREMINE_AMOUNT_UNA < one_percent / 2 || PREMINE_AMOUNT_UNA > one_percent * 2) {
        __builtin_trap();  // BUG: Premine not ~1% of supply!
    }
}

/**
 * Test total supply integrity
 */
void FuzzSupplyIntegrity(const uint8_t* data, size_t size) {
    // Calculate theoretical max supply
    uint64_t total_supply = ConsensusSubsidy::PREMINE_UNA;
    uint64_t subsidy = ConsensusSubsidy::INITIAL_SUBSIDY;

    // Sum all subsidies (simplified)
    for (uint32_t halving = 0; halving < 33 && subsidy > 0; halving++) {
        uint64_t blocks = ConsensusSubsidy::HALVING_INTERVAL;
        uint64_t supply_this_halving = blocks * subsidy;

        // Check for overflow
        if (total_supply > UINT64_MAX - supply_this_halving) {
            __builtin_trap();  // BUG: Supply calculation overflowed!
        }

        total_supply += supply_this_halving;
        subsidy >>= 1;
    }

    // INVARIANT: Total supply must not exceed MAX_MONEY
    if (total_supply > MAX_MONEY) {
        __builtin_trap();  // BUG: Total supply exceeds MAX_MONEY!
    }

    // INVARIANT: Total supply should be close to MAX_MONEY
    // (Otherwise MAX_MONEY is set too high/low)
    if (total_supply < MAX_MONEY / 2) {
        __builtin_trap();  // BUG: MAX_MONEY way too high!
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return 0;
    }

    MonetaryFuzzMode mode = static_cast<MonetaryFuzzMode>(data[0] % 7);
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    // Extract a uint64_t from fuzz data (if available)
    uint64_t fuzz_value = 0;
    if (payload_size >= 8) {
        for (int i = 0; i < 8; i++) {
            fuzz_value |= static_cast<uint64_t>(payload[i]) << (i * 8);
        }
    }

    // Extract a uint32_t for height tests
    uint32_t fuzz_height = 0;
    if (payload_size >= 4) {
        for (int i = 0; i < 4; i++) {
            fuzz_height |= static_cast<uint32_t>(payload[i]) << (i * 8);
        }
    }

    try {
        switch (mode) {
            case FUZZ_IS_VALID_AMOUNT:
                FuzzIsValidAmount(fuzz_value);
                break;

            case FUZZ_IS_VALID_AMOUNT_OR_ZERO:
                FuzzIsValidAmountOrZero(fuzz_value);
                break;

            case FUZZ_MAX_MONEY_BOUNDARY:
                FuzzMaxMoneyBoundary(fuzz_value);
                break;

            case FUZZ_SUBSIDY_CALCULATION:
                FuzzSubsidyCalculation(fuzz_height);
                break;

            case FUZZ_OVERFLOW:
                FuzzOverflow(fuzz_value);
                break;

            case FUZZ_PREMINE_AMOUNT:
                FuzzPremineAmount();
                break;

            case FUZZ_SUPPLY_INTEGRITY:
                FuzzSupplyIntegrity(payload, payload_size);
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

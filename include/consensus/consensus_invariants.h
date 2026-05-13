#pragma once

// ============================================================================
// CONSENSUS INVARIANTS - FORENSIC-GRADE CORRECTNESS ASSERTIONS
// ============================================================================
//
// This is where we stop believing consensus works and start proving it.
//
// USAGE:
//   - Unit tests: assertions catch bugs immediately
//   - Fuzzing: violations trigger with full state dump
//   - Debug builds: runtime verification throughout execution
//   - CI: run with DINERO_CONSENSUS_PARANOID=1
//
// When an invariant fails, we dump everything needed for forensics:
//   - Block hash, height, utreexo root
//   - UTXO count, total supply
//   - Full stack context
//
// Every future bug becomes a forensic artifact, not a mystery.
//
// ============================================================================

#include "primitives/uint256.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dinero {
namespace consensus {

// Forward declarations to avoid circular deps
class ConsensusUTXOSet;

// ============================================================================
// Consensus State Snapshot (for forensic dumps)
// ============================================================================

struct ConsensusStateSnapshot {
    uint256 block_hash;
    uint32_t height;
    uint256 utreexo_root;
    uint64_t utxo_count;
    uint64_t total_supply;      // Sum of all UTXO values
    uint64_t expected_supply;   // Max possible supply at height

    // Additional context
    const char* operation;      // What we were doing
    const char* file;
    int line;

    void dump(FILE* out = stderr) const {
        fprintf(out, "\n");
        fprintf(out, "╔══════════════════════════════════════════════════════════════════╗\n");
        fprintf(out, "║           CONSENSUS INVARIANT VIOLATION - STATE DUMP             ║\n");
        fprintf(out, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(out, "║ Location: %s:%d\n", file ? file : "unknown", line);
        fprintf(out, "║ Operation: %s\n", operation ? operation : "unknown");
        fprintf(out, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(out, "║ Block Hash:      %s\n", block_hash.GetHex().c_str());
        fprintf(out, "║ Height:          %u\n", height);
        fprintf(out, "║ Utreexo Root:    %s\n", utreexo_root.GetHex().c_str());
        fprintf(out, "║ UTXO Count:      %lu\n", static_cast<unsigned long>(utxo_count));
        fprintf(out, "║ Total Supply:    %lu una\n", static_cast<unsigned long>(total_supply));
        fprintf(out, "║ Expected Max:    %lu una\n", static_cast<unsigned long>(expected_supply));
        if (total_supply > expected_supply) {
            fprintf(out, "║ OVERFLOW:        %lu una CREATED FROM NOTHING\n",
                    static_cast<unsigned long>(total_supply - expected_supply));
        }
        fprintf(out, "╚══════════════════════════════════════════════════════════════════╝\n");
        fprintf(out, "\n");
        fflush(out);
    }
};

// ============================================================================
// Global State Capture (set by consensus operations)
// ============================================================================

// Thread-local state for forensic capture
// In single-threaded consensus, this is always current
struct ConsensusInvariantContext {
    uint256 current_block_hash;
    uint32_t current_height = 0;
    uint256 current_utreexo_root;
    const ConsensusUTXOSet* current_utxo_set = nullptr;
    const char* current_operation = nullptr;

    // Singleton access (thread-local in multi-threaded builds)
#ifdef DINERO_MULTI_THREADED
    static ConsensusInvariantContext& get() {
        thread_local ConsensusInvariantContext instance;
        return instance;
    }
#else
    static ConsensusInvariantContext& get() {
        static ConsensusInvariantContext instance;
        return instance;
    }
#endif

    void setBlock(const uint256& hash, uint32_t height) {
        current_block_hash = hash;
        current_height = height;
    }

    void setUtreexoRoot(const uint256& root) {
        current_utreexo_root = root;
    }

    void setUTXOSet(const ConsensusUTXOSet* set) {
        current_utxo_set = set;
    }

    void setOperation(const char* op) {
        current_operation = op;
    }

    void clear() {
        current_block_hash = uint256();
        current_height = 0;
        current_utreexo_root = uint256();
        current_utxo_set = nullptr;
        current_operation = nullptr;
    }
};

// ============================================================================
// Supply Calculation (for invariant checking)
// ============================================================================

// Maximum possible supply at given height
// Must match GetBlockSubsidy logic exactly (Fair Launch v3 — no premine)
[[nodiscard]]
constexpr uint64_t MaxSupplyAtHeight(uint32_t height) noexcept {
    constexpr uint64_t UNA_PER_DIN = 100'000'000ULL;
    constexpr uint64_t INITIAL_SUBSIDY = 100ULL * UNA_PER_DIN;
    constexpr uint32_t HALVING_INTERVAL = 1'314'000;
    constexpr uint64_t TAIL_EMISSION_UNA = 1ULL * UNA_PER_DIN;

    if (height == 0) return 0;

    // Sum subsidies from height 1 to height (PoW starts at height 1)
    uint64_t total = 0;
    uint32_t pow_blocks = height;  // blocks 1..height
    uint32_t blocks_remaining = pow_blocks;
    uint32_t epoch = 0;

    while (blocks_remaining > 0) {
        uint64_t subsidy = (epoch >= 64) ? 0 : (INITIAL_SUBSIDY >> epoch);
        // Tail emission floor
        if (subsidy < TAIL_EMISSION_UNA) subsidy = TAIL_EMISSION_UNA;

        uint32_t blocks_at_this_rate = HALVING_INTERVAL;
        if (blocks_at_this_rate > blocks_remaining) {
            blocks_at_this_rate = blocks_remaining;
        }

        total += subsidy * blocks_at_this_rate;
        blocks_remaining -= blocks_at_this_rate;
        epoch++;

        // Once in pure tail emission, all remaining blocks are 1 DIN
        if (epoch >= 64 || (INITIAL_SUBSIDY >> epoch) < TAIL_EMISSION_UNA) {
            uint64_t next_subsidy = (epoch >= 64) ? 0 : (INITIAL_SUBSIDY >> epoch);
            uint64_t effective = (next_subsidy > TAIL_EMISSION_UNA) ? next_subsidy : TAIL_EMISSION_UNA;
            if (effective == TAIL_EMISSION_UNA) {
                total += static_cast<uint64_t>(blocks_remaining) * TAIL_EMISSION_UNA;
                break;
            }
        }
    }

    return total;
}

// ============================================================================
// Core Assertion Implementation
// ============================================================================

// Abort handler - dumps state and terminates
[[noreturn]]
inline void consensus_abort_with_state(
    const char* assertion,
    const char* file,
    int line,
    const ConsensusStateSnapshot& snapshot) {

    fprintf(stderr, "\n");
    fprintf(stderr, "!!! CONSENSUS INVARIANT FAILED !!!\n");
    fprintf(stderr, "Assertion: %s\n", assertion);

    snapshot.dump(stderr);

    // Also write to file for post-mortem analysis
    FILE* dump_file = fopen("consensus_invariant_failure.dump", "w");
    if (dump_file) {
        fprintf(dump_file, "ASSERTION: %s\n", assertion);
        snapshot.dump(dump_file);
        fclose(dump_file);
        fprintf(stderr, "State dumped to: consensus_invariant_failure.dump\n");
    }

    fflush(stderr);
    std::abort();
}

// Build snapshot from current context
ConsensusStateSnapshot BuildCurrentSnapshot(const char* file, int line);

// ============================================================================
// Assertion Macros
// ============================================================================

// Always-on assertion (even in release builds for critical invariants)
#define CONSENSUS_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            auto snapshot = ::dinero::consensus::BuildCurrentSnapshot(__FILE__, __LINE__); \
            ::dinero::consensus::consensus_abort_with_state( \
                #condition, __FILE__, __LINE__, snapshot); \
        } \
    } while (0)

// Assertion with custom message
#define CONSENSUS_ASSERT_MSG(condition, msg) \
    do { \
        if (!(condition)) { \
            auto snapshot = ::dinero::consensus::BuildCurrentSnapshot(__FILE__, __LINE__); \
            snapshot.operation = msg; \
            ::dinero::consensus::consensus_abort_with_state( \
                #condition " (" msg ")", __FILE__, __LINE__, snapshot); \
        } \
    } while (0)

// Debug-only assertion (compiled out in release)
#ifdef NDEBUG
#define CONSENSUS_DEBUG_ASSERT(condition) ((void)0)
#define CONSENSUS_DEBUG_ASSERT_MSG(condition, msg) ((void)0)
#else
#define CONSENSUS_DEBUG_ASSERT(condition) CONSENSUS_ASSERT(condition)
#define CONSENSUS_DEBUG_ASSERT_MSG(condition, msg) CONSENSUS_ASSERT_MSG(condition, msg)
#endif

// Paranoid mode - extra expensive checks (enabled via env var or compile flag)
#if defined(DINERO_CONSENSUS_PARANOID) || defined(DINERO_FUZZ_BUILD)
#define CONSENSUS_PARANOID_ASSERT(condition) CONSENSUS_ASSERT(condition)
#define CONSENSUS_PARANOID_ASSERT_MSG(condition, msg) CONSENSUS_ASSERT_MSG(condition, msg)
#else
#define CONSENSUS_PARANOID_ASSERT(condition) \
    do { \
        if (::dinero::consensus::IsParanoidModeEnabled() && !(condition)) { \
            auto snapshot = ::dinero::consensus::BuildCurrentSnapshot(__FILE__, __LINE__); \
            ::dinero::consensus::consensus_abort_with_state( \
                #condition, __FILE__, __LINE__, snapshot); \
        } \
    } while (0)
#define CONSENSUS_PARANOID_ASSERT_MSG(condition, msg) \
    do { \
        if (::dinero::consensus::IsParanoidModeEnabled() && !(condition)) { \
            auto snapshot = ::dinero::consensus::BuildCurrentSnapshot(__FILE__, __LINE__); \
            snapshot.operation = msg; \
            ::dinero::consensus::consensus_abort_with_state( \
                #condition " (" msg ")", __FILE__, __LINE__, snapshot); \
        } \
    } while (0)
#endif

// Runtime paranoid mode check
inline bool IsParanoidModeEnabled() {
    static bool checked = false;
    static bool enabled = false;
    if (!checked) {
        const char* env = std::getenv("DINERO_CONSENSUS_PARANOID");
        enabled = (env != nullptr && env[0] != '0');
        checked = true;
    }
    return enabled;
}

// ============================================================================
// Scoped Context Helpers
// ============================================================================

// RAII helper for setting operation context
class ConsensusOperationScope {
public:
    explicit ConsensusOperationScope(const char* operation) {
        prev_operation_ = ConsensusInvariantContext::get().current_operation;
        ConsensusInvariantContext::get().setOperation(operation);
    }

    ~ConsensusOperationScope() {
        ConsensusInvariantContext::get().setOperation(prev_operation_);
    }

    ConsensusOperationScope(const ConsensusOperationScope&) = delete;
    ConsensusOperationScope& operator=(const ConsensusOperationScope&) = delete;

private:
    const char* prev_operation_;
};

#define CONSENSUS_OPERATION_SCOPE(name) \
    ::dinero::consensus::ConsensusOperationScope _consensus_op_scope_##__LINE__(name)

// ============================================================================
// Pre-defined Invariant Checks
// ============================================================================

// These can be called explicitly or used with CONSENSUS_ASSERT

// I1: No UTXO can have negative value (uint64_t ensures this, but check overflow)
#define CONSENSUS_CHECK_NO_OVERFLOW(value, addition) \
    CONSENSUS_ASSERT_MSG((value) <= UINT64_MAX - (addition), "value overflow detected")

// I2: Total supply cannot exceed maximum at height
#define CONSENSUS_CHECK_SUPPLY_BOUNDED(total_supply, height) \
    CONSENSUS_ASSERT_MSG((total_supply) <= MaxSupplyAtHeight(height), "supply exceeds maximum")

// I3: Spent output must exist
#define CONSENSUS_CHECK_UTXO_EXISTS(utxo_set, outpoint) \
    CONSENSUS_ASSERT_MSG((utxo_set).HaveCoin(outpoint), "spending non-existent UTXO")

// I4: No double-spend within block
#define CONSENSUS_CHECK_NOT_DOUBLE_SPENT(spent_set, outpoint) \
    CONSENSUS_ASSERT_MSG((spent_set).find(outpoint) == (spent_set).end(), "double-spend detected")

// I5: Coinbase maturity
#define CONSENSUS_CHECK_COINBASE_MATURE(utxo, current_height, maturity) \
    CONSENSUS_ASSERT_MSG(!(utxo).is_coinbase || (current_height) >= (utxo).height + (maturity), \
                        "immature coinbase spend")

// I6: Output value positive and bounded
#define CONSENSUS_CHECK_OUTPUT_VALUE(value) \
    CONSENSUS_ASSERT_MSG((value) > 0 && (value) <= 21'000'000ULL * 100'000'000ULL, \
                        "invalid output value")

// I7: Block height monotonically increasing
#define CONSENSUS_CHECK_HEIGHT_INCREASES(old_height, new_height) \
    CONSENSUS_ASSERT_MSG((new_height) == (old_height) + 1, "non-sequential block height")

// ============================================================================
// Batch Invariant Verification
// ============================================================================

// Verify all critical invariants on a UTXO set
// Call this after applying a block in debug/paranoid builds
bool VerifyAllInvariants(const ConsensusUTXOSet& utxo_set,
                         uint32_t height,
                         std::string& error);

// Quick sanity check (cheap, can run always)
bool VerifyQuickSanity(const ConsensusUTXOSet& utxo_set,
                       uint32_t height);

} // namespace consensus
} // namespace dinero

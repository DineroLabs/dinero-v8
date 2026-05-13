#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace din {

/**
 * @brief Production-grade coin selection with Branch-and-Bound + greedy fallback
 * 
 * Implements Bitcoin Core-compatible algorithms for optimal UTXO selection:
 * - Branch-and-Bound (BnB) for exact/near-exact matches
 * - Greedy fallback for cases where BnB fails
 * - Waste minimization and dust avoidance
 * - RBF and anti-fee-sniping support
 */

/**
 * @brief UTXO representation for coin selection
 */
struct SelectableUTXO {
    std::string txid;
    uint32_t vout;
    int64_t value;              // Value in una
    int32_t confirmations;      // Number of confirmations
    bool spendable;             // Can be spent (not locked/frozen)
    uint8_t address_type;       // 0=P2PKH, 1=P2WPKH, 2=P2SH, 3=P2TR, etc.
    
    // Computed fields (filled by coin selector)
    int64_t effective_value;    // value - input_fee
    int64_t input_fee;          // Fee to spend this UTXO
    uint64_t input_vbytes;      // Virtual bytes to spend this UTXO
    
    SelectableUTXO(const std::string& id, uint32_t out, int64_t val, int32_t conf, bool spend, uint8_t type)
        : txid(id), vout(out), value(val), confirmations(conf), spendable(spend), address_type(type)
        , effective_value(0), input_fee(0), input_vbytes(0) {}
};

/**
 * @brief Coin selection request parameters
 */
struct CoinSelectionRequest {
    int64_t target_value;           // Target amount in una
    double feerate_una_vb;          // Fee rate in una per virtual byte
    int64_t min_relay_fee_una_vb;   // Minimum relay fee rate
    uint8_t change_output_type;     // Type for change output (default P2WPKH)
    bool enable_rbf;                // Enable Replace-By-Fee
    uint32_t current_height;        // Current blockchain height
    uint32_t median_time_past;      // Median time past for anti-fee-sniping
    
    // BnB parameters
    uint32_t max_bnb_attempts;      // Max BnB iterations (default 100,000)
    uint32_t max_bnb_time_ms;       // Max BnB runtime in ms (default 20)
    
    CoinSelectionRequest()
        : target_value(0), feerate_una_vb(1.0), min_relay_fee_una_vb(1.0)
        , change_output_type(1), enable_rbf(false), current_height(0), median_time_past(0)
        , max_bnb_attempts(100000), max_bnb_time_ms(20) {}
};

/**
 * @brief Coin selection result
 */
struct CoinSelectionResult {
    std::vector<SelectableUTXO> selected_utxos;
    int64_t selected_value;         // Total value of selected UTXOs
    int64_t target_value;           // Original target
    int64_t fee_paid;               // Total fee for transaction
    int64_t change_value;           // Change amount (0 if no change)
    bool has_change;                // Whether change output is included
    int64_t waste;                  // Waste metric (lower is better)
    
    // Transaction parameters
    uint32_t lockTime;              // nLockTime for anti-fee-sniping
    std::vector<uint32_t> sequences; // nSequence for each input
    
    // Algorithm used
    enum Algorithm { BranchAndBound, Greedy } algorithm_used;
    
    CoinSelectionResult() 
        : selected_value(0), target_value(0), fee_paid(0), change_value(0)
        , has_change(false), waste(0), locktime(0), algorithm_used(Greedy) {}
};

/**
 * @brief Virtual byte size table for different address types
 */
class VirtualSizeTable {
public:
    // Input sizes (virtual bytes)
    static constexpr uint64_t INPUT_P2PKH_VBYTES = 148;      // Legacy P2PKH
    static constexpr uint64_t INPUT_P2WPKH_VBYTES = 68;      // Native SegWit
    static constexpr uint64_t INPUT_P2SH_P2WPKH_VBYTES = 91; // Wrapped SegWit
    static constexpr uint64_t INPUT_P2TR_KEYPATH_VBYTES = 57; // Taproot keypath
    
    // Output sizes (bytes, same as virtual bytes for outputs)
    static constexpr uint64_t OUTPUT_P2PKH_BYTES = 34;       // Legacy P2PKH
    static constexpr uint64_t OUTPUT_P2SH_BYTES = 32;        // P2SH
    static constexpr uint64_t OUTPUT_P2WPKH_BYTES = 31;      // Native SegWit
    static constexpr uint64_t OUTPUT_P2WSH_BYTES = 43;       // P2WSH
    static constexpr uint64_t OUTPUT_P2TR_BYTES = 43;        // Taproot
    
    /**
     * @brief Get input size for address type
     */
    static uint64_t getInputVBytes(uint8_t address_type) {
        switch (address_type) {
            case 0: return INPUT_P2PKH_VBYTES;      // P2PKH
            case 1: return INPUT_P2WPKH_VBYTES;     // P2WPKH
            case 2: return INPUT_P2SH_P2WPKH_VBYTES; // P2SH-P2WPKH
            case 3: return INPUT_P2TR_KEYPATH_VBYTES; // P2TR
            default: return INPUT_P2WPKH_VBYTES;    // Default to P2WPKH
        }
    }
    
    /**
     * @brief Get output size for address type
     */
    static uint64_t getOutputBytes(uint8_t address_type) {
        switch (address_type) {
            case 0: return OUTPUT_P2PKH_BYTES;      // P2PKH
            case 1: return OUTPUT_P2WPKH_BYTES;     // P2WPKH
            case 2: return OUTPUT_P2SH_BYTES;       // P2SH
            case 3: return OUTPUT_P2TR_BYTES;       // P2TR
            case 4: return OUTPUT_P2WSH_BYTES;      // P2WSH
            default: return OUTPUT_P2WPKH_BYTES;    // Default to P2WPKH
        }
    }
};

/**
 * @brief Production coin selection engine
 */
class CoinSelector {
public:
    /**
     * @brief Select coins using BnB + greedy fallback
     * 
     * @param utxos Available UTXOs to select from
     * @param request Selection parameters
     * @return Selection result with chosen UTXOs and metadata
     */
    static std::optional<CoinSelectionResult> selectCoins(
        std::vector<SelectableUTXO>& utxos,
        const CoinSelectionRequest& request
    );

private:
    // Core algorithms
    static std::optional<CoinSelectionResult> tryBranchAndBound(
        std::vector<SelectableUTXO>& utxos,
        const CoinSelectionRequest& request
    );
    
    static std::optional<CoinSelectionResult> tryGreedy(
        std::vector<SelectableUTXO>& utxos,
        const CoinSelectionRequest& request
    );
    
    // Helper functions
    static void preprocessUTXOs(
        std::vector<SelectableUTXO>& utxos,
        const CoinSelectionRequest& request
    );
    
    static int64_t calculateDustThreshold(
        uint8_t output_type,
        int64_t min_relay_fee_una_vb
    );
    
    static int64_t calculateWaste(
        const std::vector<SelectableUTXO>& selected,
        int64_t change_fee,
        int64_t target_excess
    );
    
    static void setTransactionParams(
        CoinSelectionResult& result,
        const CoinSelectionRequest& request
    );
    
    // BnB implementation
    static bool branchAndBoundSearch(
        const std::vector<SelectableUTXO>& utxos,
        int64_t target_range_min,
        int64_t target_range_max,
        std::vector<bool>& selection,
        size_t index,
        int64_t current_sum,
        uint32_t& attempts,
        const std::chrono::steady_clock::time_point& start_time,
        uint32_t max_time_ms
    );
};

} // namespace din

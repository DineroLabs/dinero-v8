#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <map>

namespace din::privacy {

// CoinJoin round parameters
struct CJParams {
    int64_t target_amt;           // Target amount per output (una)
    int64_t fee_rate_una_vb;      // Fee rate in una/vB
    int min_peers;                // Minimum number of peers required
    int max_peers = 10;           // Maximum number of peers
    int64_t min_input_value;      // Minimum input value
    int64_t max_input_value;     // Maximum input value
    bool require_equal_outputs = true;  // Require equal output amounts
    std::string coordinator_url;   // Coordinator endpoint URL
};

// CoinJoin round status
struct CJStatus {
    std::string round_id;         // Unique round identifier
    std::string phase;            // Current phase: "register", "commit", "psbt", "sign", "broadcast"
    int peers;                    // Number of peers in round
    int required_peers;          // Required peers for round
    bool done;                    // Round completed
    bool success;                 // Round successful
    std::string psbt_b64;         // Current PSBT (if in psbt phase)
    std::string txid;             // Final transaction ID (if successful)
    std::string error_message;    // Error message (if failed)
    int64_t total_fee;            // Total fee paid
    std::vector<std::string> input_txids;  // Input transaction IDs
    std::vector<std::string> output_addresses;  // Output addresses
};

// CoinJoin input selection
struct CJInput {
    std::string txid;             // Transaction ID
    uint32_t vout;                // Output index
    int64_t value;                // Value in una
    std::vector<uint8_t> script_pubkey;  // Script pubkey
    std::string address;          // Address (for display)
    bool is_change;               // Is this a change output
};

// CoinJoin output
struct CJOutput {
    std::string address;          // Output address
    int64_t value;               // Output value
    bool is_change;              // Is this a change output
};

// CoinJoin round state machine
enum class CJPhase {
    REGISTER,    // Registering inputs
    COMMIT,      // Committing to round
    PSBT,        // Exchanging PSBTs
    SIGN,        // Signing transaction
    BROADCAST,   // Broadcasting transaction
    COMPLETE,    // Round completed
    FAILED       // Round failed
};

// UTXO selection function
using SelectUTXOFn = std::function<std::vector<CJInput>(int64_t target_amount, int64_t fee_rate)>;

// Change address generation function
using GetChangeAddressFn = std::function<std::string()>;

// Transaction signing function
using SignTransactionFn = std::function<bool(const std::string& psbt_b64, std::string& signed_psbt)>;

// CoinJoin client interface
class ICoinJoinClient {
public:
    virtual ~ICoinJoinClient() = default;
    
    // Join a CoinJoin round
    virtual std::string join_round(const CJParams& params) = 0;
    
    // Poll round status
    virtual CJStatus poll(const std::string& round_id) = 0;
    
    // Cancel participation in round
    virtual bool cancel(const std::string& round_id) = 0;
    
    // Get active rounds
    virtual std::vector<std::string> get_active_rounds() = 0;
    
    // Get round history
    virtual std::vector<CJStatus> get_round_history() = 0;
};

// CoinJoin coordinator interface (for different backends)
class ICoinJoinCoordinator {
public:
    virtual ~ICoinJoinCoordinator() = default;
    
    // Register for a round
    virtual std::string register_round(const CJParams& params, const std::vector<CJInput>& inputs) = 0;
    
    // Commit to round
    virtual bool commit_round(const std::string& round_id, const std::string& commitment) = 0;
    
    // Get round PSBT
    virtual std::string get_round_psbt(const std::string& round_id) = 0;
    
    // Submit signed PSBT
    virtual bool submit_signed_psbt(const std::string& round_id, const std::string& signed_psbt) = 0;
    
    // Get round status
    virtual CJStatus get_round_status(const std::string& round_id) = 0;
    
    // Cancel round participation
    virtual bool cancel_round(const std::string& round_id) = 0;
};

} // namespace din::privacy

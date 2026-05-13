#pragma once
#include <string>
#include <optional>
#include <vector>
#include <map>

namespace din::privacy {

/**
 * @brief PayJoin offer from receiver
 */
struct PayjoinOffer {
    std::string endpoint;           // HTTP endpoint for PayJoin negotiation
    int64_t amount;                 // Amount in una
    std::string metadata;           // Additional metadata (memo, etc.)
    std::string bpu_uri;           // BIP78 URI for client
    
    PayjoinOffer(const std::string& ep, int64_t amt, const std::string& meta, const std::string& uri)
        : endpoint(ep), amount(amt), metadata(meta), bpu_uri(uri) {}
};

/**
 * @brief PayJoin negotiation result
 */
struct PayjoinResult {
    std::string psbt_b64;           // Base64-encoded PSBT
    bool success;                   // Whether negotiation succeeded
    std::string error_message;     // Error message if failed
    int64_t fee_paid;              // Fee paid in una
    
    PayjoinResult(const std::string& psbt, bool succ, const std::string& err = "", int64_t fee = 0)
        : psbt_b64(psbt), success(succ), error_message(err), fee_paid(fee) {}
};

/**
 * @brief PayJoin policy constraints
 */
struct PayjoinPolicy {
    int64_t min_fee_rate;          // Minimum fee rate in sat/vB
    int64_t max_fee_rate;          // Maximum fee rate in sat/vB
    bool require_rbf;              // Require RBF (Replace-By-Fee)
    bool allow_output_amount_leak; // Allow output amount leakage
    int64_t min_change_amount;     // Minimum change amount in una
    int max_inputs;                // Maximum number of inputs
    int max_outputs;               // Maximum number of outputs
    
    PayjoinPolicy() : min_fee_rate(1), max_fee_rate(100), require_rbf(true),
                     allow_output_amount_leak(false), min_change_amount(1000),
                     max_inputs(10), max_outputs(10) {}
};

/**
 * @brief PayJoin receiver interface
 * 
 * Handles PayJoin requests from senders, validates policy,
 * and merges receiver inputs into sender's PSBT.
 */
struct IPayjoinReceiver {
    virtual ~IPayjoinReceiver() = default;
    
    /**
     * @brief Prepare a PayJoin offer
     * 
     * @param amount Amount to receive in una
     * @param memo Optional memo/description
     * @param policy PayJoin policy constraints
     * @return PayJoin offer with endpoint and BIP78 URI
     */
    virtual PayjoinOffer prepare_offer(
        int64_t amount, 
        const std::string& memo,
        const PayjoinPolicy& policy = PayjoinPolicy()
    ) = 0;
    
    /**
     * @brief Handle PayJoin request from sender
     * 
     * @param psbt_b64 Base64-encoded PSBT from sender
     * @param policy Policy constraints to apply
     * @return Modified PSBT with receiver inputs merged
     */
    virtual PayjoinResult handle_request(
        const std::string& psbt_b64,
        const PayjoinPolicy& policy = PayjoinPolicy()
    ) = 0;
    
    /**
     * @brief Validate PayJoin PSBT against policy
     * 
     * @param psbt_b64 PSBT to validate
     * @param policy Policy constraints
     * @return true if PSBT passes policy validation
     */
    virtual bool validate_psbt(
        const std::string& psbt_b64,
        const PayjoinPolicy& policy
    ) = 0;
};

/**
 * @brief PayJoin client interface
 * 
 * Initiates PayJoin negotiations with receivers
 * and merges receiver inputs into sender's PSBT.
 */
struct IPayjoinClient {
    virtual ~IPayjoinClient() = default;
    
    /**
     * @brief Negotiate PayJoin with receiver
     * 
     * @param bpu_uri BIP78 URI from receiver
     * @param psbt_b64 Original PSBT from sender
     * @param policy Policy constraints to apply
     * @return Negotiated PSBT with receiver inputs merged
     */
    virtual PayjoinResult negotiate(
        const std::string& bpu_uri,
        const std::string& psbt_b64,
        const PayjoinPolicy& policy = PayjoinPolicy()
    ) = 0;
    
    /**
     * @brief Parse BIP78 URI
     * 
     * @param bpu_uri BIP78 URI string
     * @return Parsed URI components
     */
    virtual std::map<std::string, std::string> parse_bpu_uri(
        const std::string& bpu_uri
    ) = 0;
    
    /**
     * @brief Check if PayJoin is beneficial
     * 
     * @param original_psbt Original PSBT
     * @param payjoin_psbt PayJoin PSBT
     * @return true if PayJoin provides privacy benefit
     */
    virtual bool is_beneficial(
        const std::string& original_psbt,
        const std::string& payjoin_psbt
    ) = 0;
};

} // namespace din::privacy

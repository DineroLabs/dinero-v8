#pragma once

#include "lightning_types.h"
#include "gossip_types.h"
#include "time_oracle.h"  // Phase 8.5: Deterministic time source
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <memory>

namespace dinero {
namespace lightning {

// BOLT 11 Lightning Invoice
class Invoice {
public:
    // Invoice creation (Phase 8.5: Requires ::lightning::ITimeOracle for deterministic timestamps)
    static Invoice create(
        const ::lightning::ITimeOracle* time_oracle,
        const std::string& network,  // "din", "tdn" (testnet), "rdn" (regtest)
        uint64_t amount_muna,
        const std::array<uint8_t, 32>& payment_hash,
        const std::array<uint8_t, 32>& payment_secret,
        const std::string& description,
        uint32_t expiry_seconds = 3600
    );

    // Parse from BOLT 11 string
    static std::optional<Invoice> decode(const std::string& bolt11_string);

    // Encode to BOLT 11 string
    std::string encode() const;

    // Getters
    std::string get_network() const { return network_; }
    std::optional<uint64_t> get_amount_muna() const { return amount_muna_; }
    std::array<uint8_t, 32> get_payment_hash() const { return payment_hash_; }
    std::optional<std::array<uint8_t, 32>> get_payment_secret() const { return payment_secret_; }
    std::string get_description() const { return description_; }
    uint32_t get_timestamp() const { return timestamp_; }
    uint32_t get_expiry() const { return expiry_seconds_; }
    std::optional<NodeID> get_payee_node_id() const { return payee_node_id_; }
    std::vector<uint8_t> get_signature() const { return signature_; }

    // Status (Phase 8.5: Block-height-based expiry)
    bool is_expired(const ::lightning::ITimeOracle* time_oracle) const;
    uint64_t get_expiry_timestamp() const;  // Returns Unix timestamp (derived from block height)
    uint32_t get_seconds_until_expiry(const ::lightning::ITimeOracle* time_oracle) const;

    // Route hints for private channels
    struct RouteHint {
        NodeID node_id;
        ShortChannelID short_channel_id;
        uint32_t fee_base_muna;
        uint32_t fee_proportional_millionths;
        uint16_t cltv_expiry_delta;
    };

    void add_route_hint(const std::vector<RouteHint>& route);
    std::vector<std::vector<RouteHint>> get_route_hints() const { return route_hints_; }

    // Features
    void add_feature(uint16_t feature_bit);
    bool has_feature(uint16_t feature_bit) const;
    std::vector<uint8_t> get_features() const { return features_; }

    // Fallback on-chain address
    void set_fallback_address(const std::string& address);
    std::optional<std::string> get_fallback_address() const { return fallback_address_; }

    // Min final CLTV expiry
    void set_min_final_cltv_expiry(uint16_t blocks);
    uint16_t get_min_final_cltv_expiry() const { return min_final_cltv_expiry_; }

    // Metadata (for stateless invoices)
    void set_metadata(const std::vector<uint8_t>& metadata);
    std::optional<std::vector<uint8_t>> get_metadata() const { return metadata_; }

    // Validation
    bool verify_signature() const;
    bool is_valid() const;

    // QR code generation
    std::string to_qr_string() const;  // Returns URI: dinero:<invoice>
    std::vector<uint8_t> to_qr_code_png(int size = 300) const;  // Generate PNG QR code

private:
    Invoice() = default;

    // Required fields
    std::string network_;                              // "din", "tdn", "rdn"
    std::array<uint8_t, 32> payment_hash_;            // SHA256 of preimage
    uint32_t timestamp_;                               // Unix timestamp
    std::vector<uint8_t> signature_;                   // Recoverable ECDSA signature

    // Optional fields
    std::optional<uint64_t> amount_muna_;             // Amount (can be empty for flexible)
    std::string description_;                          // UTF-8 description
    std::optional<std::array<uint8_t, 32>> payment_secret_;  // Payment secret (for MPP)
    std::optional<NodeID> payee_node_id_;             // Destination node
    uint32_t expiry_seconds_ = 3600;                  // Expiry (default 1 hour)
    std::optional<std::string> fallback_address_;     // On-chain fallback
    uint16_t min_final_cltv_expiry_ = 18;            // Min final CLTV (default 18 blocks)
    std::vector<std::vector<RouteHint>> route_hints_; // Route hints for private channels
    std::vector<uint8_t> features_;                    // Feature bits
    std::optional<std::vector<uint8_t>> metadata_;    // Metadata for stateless invoices

    friend class InvoiceBuilder;
};

// Invoice builder for convenient construction
class InvoiceBuilder {
public:
    InvoiceBuilder(const ::lightning::ITimeOracle* time_oracle, const std::string& network);

    // Required
    InvoiceBuilder& amount_muna(uint64_t amount);
    InvoiceBuilder& payment_hash(const std::array<uint8_t, 32>& hash);
    InvoiceBuilder& description(const std::string& desc);

    // Optional
    InvoiceBuilder& payment_secret(const std::array<uint8_t, 32>& secret);
    InvoiceBuilder& payee_node_id(const NodeID& node_id);
    InvoiceBuilder& expiry_seconds(uint32_t seconds);
    InvoiceBuilder& fallback_address(const std::string& address);
    InvoiceBuilder& min_final_cltv_expiry(uint16_t blocks);
    InvoiceBuilder& route_hint(const std::vector<Invoice::RouteHint>& route);
    InvoiceBuilder& feature(uint16_t feature_bit);
    InvoiceBuilder& metadata(const std::vector<uint8_t>& meta);

    // Build and sign
    Invoice build_and_sign(const std::vector<uint8_t>& private_key) const;

private:
    const ::lightning::ITimeOracle* m_time_oracle;  // Phase 8.5: Deterministic time source
    Invoice invoice_;
};

// Invoice payment tracker
class InvoiceTracker {
public:
    InvoiceTracker(const ::lightning::ITimeOracle* time_oracle);
    ~InvoiceTracker();

    // Track invoice
    void track_invoice(const Invoice& invoice, const std::array<uint8_t, 32>& payment_preimage);

    // Payment status
    enum class PaymentStatus {
        PENDING,      // Invoice created, not paid
        PAYING,       // Payment in progress
        PAID,         // Payment successful
        EXPIRED,      // Invoice expired
        CANCELLED     // Invoice cancelled
    };

    struct PaymentInfo {
        Invoice invoice;
        std::array<uint8_t, 32> preimage;
        PaymentStatus status;
        std::optional<uint32_t> paid_at_timestamp;
        std::optional<uint64_t> amount_paid_muna;
    };

    // Query
    std::optional<PaymentInfo> get_payment_info(const std::array<uint8_t, 32>& payment_hash) const;
    std::vector<PaymentInfo> get_pending_invoices() const;
    std::vector<PaymentInfo> get_paid_invoices() const;

    // Update status
    void mark_as_paying(const std::array<uint8_t, 32>& payment_hash);
    void mark_as_paid(const std::array<uint8_t, 32>& payment_hash, uint64_t amount_muna);
    void mark_as_expired(const std::array<uint8_t, 32>& payment_hash);
    void cancel_invoice(const std::array<uint8_t, 32>& payment_hash);

    // Cleanup (Phase 8.5: Uses ITimeOracle for deterministic pruning)
    size_t prune_expired_invoices();
    size_t prune_old_paid_invoices(uint32_t older_than_seconds);

    // Statistics
    struct Stats {
        uint32_t total_invoices;
        uint32_t pending_invoices;
        uint32_t paid_invoices;
        uint32_t expired_invoices;
        uint64_t total_received_muna;
        double success_rate;
    };
    Stats get_stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Invoice manager - high-level API
class InvoiceManager {
public:
    InvoiceManager(const ::lightning::ITimeOracle* time_oracle, const NodeID& our_node_id);
    ~InvoiceManager();

    // Create invoice
    Result<Invoice> create_invoice(
        uint64_t amount_muna,
        const std::string& description,
        uint32_t expiry_seconds = 3600
    );

    // Create invoice with amount to be determined (for tips, donations)
    Result<Invoice> create_open_invoice(
        const std::string& description,
        uint32_t expiry_seconds = 3600
    );

    // Decode and validate invoice
    Result<Invoice> decode_invoice(const std::string& bolt11_string);

    // Pay invoice
    Result<std::array<uint8_t, 32>> pay_invoice(
        const Invoice& invoice,
        std::optional<uint64_t> amount_muna = std::nullopt  // For open invoices
    );

    // Query invoice status
    std::optional<InvoiceTracker::PaymentInfo> get_invoice_status(
        const std::array<uint8_t, 32>& payment_hash
    );

    // List invoices
    std::vector<Invoice> list_invoices(InvoiceTracker::PaymentStatus status);

    // Cancel invoice
    Result<void> cancel_invoice(const std::array<uint8_t, 32>& payment_hash);

    // Generate QR code
    std::vector<uint8_t> generate_qr_code(const Invoice& invoice, int size = 300);

    // Export invoice as text/image
    std::string export_invoice_text(const Invoice& invoice);
    std::vector<uint8_t> export_invoice_png(const Invoice& invoice, int size = 300);

    // Statistics
    InvoiceTracker::Stats get_stats() const;

    // Settings
    void set_default_expiry(uint32_t seconds);
    void set_min_final_cltv_expiry(uint16_t blocks);
    void enable_route_hints(bool enable);  // For private channels

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    // Generate payment preimage and hash
    std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>> generate_payment_pair();

    // Sign invoice
    std::vector<uint8_t> sign_invoice(const Invoice& invoice);
};

// Utility functions
namespace invoice_utils {
    // Bech32 encoding/decoding
    std::string bech32_encode(const std::string& hrp, const std::vector<uint8_t>& data);
    std::optional<std::pair<std::string, std::vector<uint8_t>>> bech32_decode(const std::string& str);

    // Convert between bit groups (for Bech32 encoding)
    std::vector<uint8_t> convert_bits(const std::vector<uint8_t>& data, int frombits, int tobits, bool pad = true);

    // Decode helpers (for invoice parsing)
    void convert_from_5bit(const std::vector<uint8_t>& data, std::array<uint8_t, 32>& output);
    void convert_from_5bit(const std::vector<uint8_t>& data, std::vector<uint8_t>& output);
    void decode_string(const std::vector<uint8_t>& data, std::string& output);

    // BOLT 11 tagged field encoding
    std::vector<uint8_t> encode_payment_hash(const std::array<uint8_t, 32>& payment_hash);
    std::vector<uint8_t> encode_description(const std::string& description);
    std::vector<uint8_t> encode_payment_secret(const std::array<uint8_t, 32>& payment_secret);
    std::vector<uint8_t> encode_expiry(uint32_t expiry_seconds);
    std::vector<uint8_t> encode_min_final_cltv(uint16_t blocks);
    std::vector<uint8_t> encode_features(const std::vector<uint8_t>& features);
    std::vector<uint8_t> encode_payee_node_id(const NodeID& node_id);
    std::vector<uint8_t> encode_fallback_address(const std::string& address);
    std::vector<uint8_t> encode_route_hint(const std::vector<Invoice::RouteHint>& hints);
    std::vector<uint8_t> encode_metadata(const std::vector<uint8_t>& metadata);

    // QR code generation
    std::vector<uint8_t> generate_qr_code_png(const std::string& data, int size);

    // Payment hash from preimage
    std::array<uint8_t, 32> hash_preimage(const std::array<uint8_t, 32>& preimage);

    // Human-readable amount formatting
    std::string format_amount(uint64_t amount_muna);  // "0.001 DIN" or "1,000 una"

    // Parse amount from string
    std::optional<uint64_t> parse_amount(const std::string& amount_str);

    // Validate invoice string format
    bool is_valid_bolt11(const std::string& invoice_str);

    // Extract payment hash from invoice string (without full decode)
    std::optional<std::array<uint8_t, 32>> extract_payment_hash(const std::string& invoice_str);
}

} // namespace lightning
} // namespace dinero

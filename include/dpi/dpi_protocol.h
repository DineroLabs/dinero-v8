#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace din::dpi {

// ============================================================================
// DPI Protocol v1 — Dinero Payment Intent
//
// Phase 1: Attested invoice-bound payment with verifiable transaction packages
// and explicit conflict/risk checks. NOT instant settlement finality.
// ============================================================================

// Protocol constants
static constexpr uint8_t  DPI_VERSION        = 0x01;
static constexpr uint8_t  DPI_NETWORK_MAIN   = 0x01;
static constexpr uint8_t  DPI_NETWORK_TEST   = 0x02;
static constexpr uint8_t  DPI_NETWORK_REG    = 0x03;
static constexpr uint16_t DPI_DEFAULT_EXPIRY  = 300;   // 5 minutes
static constexpr size_t   MAX_MEMO_LENGTH     = 255;
static constexpr size_t   MAX_ADDRESS_LENGTH  = 192;   // includes shielded bech32m
static constexpr size_t   SCHNORR_SIG_SIZE    = 64;
static constexpr size_t   XONLY_PUBKEY_SIZE   = 32;
static constexpr size_t   HASH_SIZE           = 32;
static constexpr size_t   MERCHANT_ID_SIZE    = 20;    // HASH160
static constexpr size_t   NONCE_SIZE          = 16;

// ============================================================================
// DpiInvoice — Merchant-issued payment request
// ============================================================================
//
// Phase 1 identity model: merchant signing key = wallet receive key (index 0).
// Long-term: dedicated merchant identity key separate from spend key.

struct DpiInvoice {
    uint8_t  version = DPI_VERSION;
    uint8_t  network = DPI_NETWORK_MAIN;
    uint64_t amount = 0;                               // una (1 DIN = 1e8 una)
    std::string destination_address;                    // bech32m address (human-readable)
    std::array<uint8_t, MERCHANT_ID_SIZE> merchant_id{};  // HASH160 of merchant pubkey
    std::array<uint8_t, NONCE_SIZE> nonce{};            // 16-byte CSPRNG nonce
    uint32_t timestamp = 0;                             // UNIX seconds
    uint16_t expiry = DPI_DEFAULT_EXPIRY;               // seconds from timestamp
    std::string memo;                                   // UTF-8, max 255 bytes
    std::vector<uint8_t> merchant_sig;                  // 64-byte Schnorr signature

    // Computed (not serialized) — derived from CanonicalInvoiceFields
    std::array<uint8_t, HASH_SIZE> invoice_id{};
};

// ============================================================================
// DpiPaymentPackage — Sender-constructed payment proof
// ============================================================================

struct DpiPaymentPackage {
    std::vector<uint8_t> raw_tx;                        // full serialized transaction
    std::vector<uint8_t> attestation_sig;               // 64-byte Schnorr signature
    std::array<uint8_t, XONLY_PUBKEY_SIZE> sender_pubkey{};  // x-only sender pubkey
    std::array<uint8_t, HASH_SIZE> invoice_id{};        // links to merchant's invoice
};

// ============================================================================
// DpiVerifyResult — Transparent, rule-based verification outcome
// ============================================================================

struct DpiVerifyChecks {
    bool invoice_bound = false;       // invoice_id in package matches invoice
    bool output_match = false;        // tx output scriptPubKey matches destination
    bool amount_match = false;        // tx output value >= invoice amount
    bool attestation_valid = false;   // Schnorr attestation signature valid
    bool seen_in_mempool = false;     // tx found in local mempool
    bool conflicts_found = false;     // conflicting spends on any input
    bool expired = false;             // invoice past TTL
    bool utreexo_proofs_valid = false; // Phase 2: all input UTXO proofs verified
};

struct DpiVerifyResult {
    bool valid = false;
    std::string tier = "T0";          // "T0" = unverified, "T1" = instant accept
    DpiVerifyChecks checks;
    double risk_score = 1.0;          // 0.0 = safe, 1.0 = risky (rule-based)
    std::string error;
};

// ============================================================================
// Canonical field builders — centralized hash-critical field selection
// ============================================================================

// Deterministic byte buffer of invoice fields for hashing/signing.
// Fields: version | network | amount(8 LE) | addr_len(2 LE) | address |
//         merchant_id(20) | nonce(16) | timestamp(4 LE) | expiry(2 LE)
std::vector<uint8_t> CanonicalInvoiceFields(const DpiInvoice& inv);

// Deterministic byte buffer for attestation hashing.
// Fields: invoice_id(32) | txid(32) | sender_pubkey(32)
std::vector<uint8_t> CanonicalAttestationFields(
    const std::array<uint8_t, HASH_SIZE>& invoice_id,
    const std::array<uint8_t, HASH_SIZE>& txid,
    const std::array<uint8_t, XONLY_PUBKEY_SIZE>& sender_pubkey
);

// ============================================================================
// Hash computations — BIP-340 tagged hash domain separation
// ============================================================================

// SHA256(SHA256(tag) || SHA256(tag) || data) per BIP-340
std::array<uint8_t, HASH_SIZE> DpiTaggedHash(
    const std::string& tag,
    const std::vector<uint8_t>& data
);

// invoice_id = DpiTaggedHash("DPI/invoice", CanonicalInvoiceFields(inv))
std::array<uint8_t, HASH_SIZE> ComputeInvoiceId(const DpiInvoice& inv);

// attestation_msg = DpiTaggedHash("DPI/attest-v1", CanonicalAttestationFields(...))
std::array<uint8_t, HASH_SIZE> ComputeAttestationMessage(
    const std::array<uint8_t, HASH_SIZE>& invoice_id,
    const std::array<uint8_t, HASH_SIZE>& txid,
    const std::array<uint8_t, XONLY_PUBKEY_SIZE>& sender_pubkey
);

// ============================================================================
// Serialization / Deserialization
// ============================================================================

std::vector<uint8_t> SerializeInvoice(const DpiInvoice& inv);
bool DeserializeInvoice(const std::vector<uint8_t>& data, DpiInvoice& out);

std::vector<uint8_t> SerializePackage(const DpiPaymentPackage& pkg);
bool DeserializePackage(const std::vector<uint8_t>& data, DpiPaymentPackage& out);

// ============================================================================
// Signature operations (wraps SchnorrSigner)
// ============================================================================

// Sign an invoice with the merchant's private key. Sets inv.merchant_sig and inv.invoice_id.
bool SignInvoice(DpiInvoice& inv, const std::vector<uint8_t>& merchant_privkey);

// Verify merchant signature on invoice.
// merchant_pubkey = 32-byte x-only pubkey corresponding to the signing key.
bool VerifyInvoiceSignature(
    const DpiInvoice& inv,
    const std::vector<uint8_t>& merchant_pubkey
);

// Sign attestation: proves sender intentionally bound payment to invoice.
// Returns 64-byte Schnorr signature, or empty on failure.
std::vector<uint8_t> SignAttestation(
    const std::array<uint8_t, HASH_SIZE>& invoice_id,
    const std::array<uint8_t, HASH_SIZE>& txid,
    const std::array<uint8_t, XONLY_PUBKEY_SIZE>& sender_pubkey,
    const std::vector<uint8_t>& sender_privkey
);

// Verify attestation signature.
bool VerifyAttestationSignature(
    const std::array<uint8_t, HASH_SIZE>& invoice_id,
    const std::array<uint8_t, HASH_SIZE>& txid,
    const std::array<uint8_t, XONLY_PUBKEY_SIZE>& sender_pubkey,
    const std::vector<uint8_t>& signature
);

// ============================================================================
// Utility
// ============================================================================

// Check expiry: true if invoice is expired
bool IsInvoiceExpired(const DpiInvoice& inv);

// Get DPI network byte for the currently active chain
uint8_t GetDpiNetworkByte();

// Convert bech32m address to P2TR scriptPubKey (OP_1 PUSH32 <witness_program>)
std::vector<uint8_t> AddressToScriptPubKey(const std::string& address);

// Compute risk score from verification checks (transparent, rule-based)
double ComputeRiskScore(const DpiVerifyChecks& checks);

// Determine acceptance tier from checks
std::string DetermineTier(const DpiVerifyChecks& checks, double risk_score);

// Generate cryptographically secure random bytes (RAND_bytes)
void GenerateSecureRandom(uint8_t* buf, size_t len);

} // namespace din::dpi

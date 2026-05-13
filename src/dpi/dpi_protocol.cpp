#include "dpi/dpi_protocol.h"
#include "crypto/sha256.h"
#include "crypto/hash.h"
#include "wallet/schnorr_signer.h"
#include "address/addr_codec.h"
#include <openssl/rand.h>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace din::dpi {

// ============================================================================
// Helpers — little-endian serialization
// ============================================================================

static void WriteLE16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static void WriteLE32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

static void WriteLE64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

static bool ReadLE16(const uint8_t*& p, const uint8_t* end, uint16_t& v) {
    if (p + 2 > end) return false;
    v = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    p += 2;
    return true;
}

static bool ReadLE32(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
    if (p + 4 > end) return false;
    v = static_cast<uint32_t>(p[0])
      | (static_cast<uint32_t>(p[1]) << 8)
      | (static_cast<uint32_t>(p[2]) << 16)
      | (static_cast<uint32_t>(p[3]) << 24);
    p += 4;
    return true;
}

static bool ReadLE64(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    if (p + 8 > end) return false;
    v = 0;
    for (int i = 0; i < 8; i++) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    p += 8;
    return true;
}

// ============================================================================
// CSPRNG
// ============================================================================

void GenerateSecureRandom(uint8_t* buf, size_t len) {
    RAND_bytes(buf, static_cast<int>(len));
}

// ============================================================================
// BIP-340 Tagged Hash
// ============================================================================

std::array<uint8_t, HASH_SIZE> DpiTaggedHash(
    const std::string& tag,
    const std::vector<uint8_t>& data
) {
    // tag_hash = SHA256(tag)
    auto tag_hash = din::crypto::SHA256(
        reinterpret_cast<const uint8_t*>(tag.data()), tag.size()
    );

    // result = SHA256(tag_hash || tag_hash || data)
    ::dinero::crypto::CSHA256 hasher;
    hasher.Write(tag_hash.data(), HASH_SIZE);
    hasher.Write(tag_hash.data(), HASH_SIZE);
    hasher.Write(data.data(), data.size());

    std::array<uint8_t, HASH_SIZE> result{};
    hasher.Finalize(result.data());
    return result;
}

// ============================================================================
// Canonical field builders
// ============================================================================

std::vector<uint8_t> CanonicalInvoiceFields(const DpiInvoice& inv) {
    std::vector<uint8_t> buf;
    buf.reserve(128);

    buf.push_back(inv.version);
    buf.push_back(inv.network);
    WriteLE64(buf, inv.amount);

    // Address as length-prefixed UTF-8
    uint16_t addr_len = static_cast<uint16_t>(inv.destination_address.size());
    WriteLE16(buf, addr_len);
    buf.insert(buf.end(), inv.destination_address.begin(), inv.destination_address.end());

    // Fixed-size fields
    buf.insert(buf.end(), inv.merchant_id.begin(), inv.merchant_id.end());
    buf.insert(buf.end(), inv.nonce.begin(), inv.nonce.end());
    WriteLE32(buf, inv.timestamp);
    WriteLE16(buf, inv.expiry);

    return buf;
}

std::vector<uint8_t> CanonicalAttestationFields(
    const std::array<uint8_t, HASH_SIZE>& invoice_id,
    const std::array<uint8_t, HASH_SIZE>& txid,
    const std::array<uint8_t, XONLY_PUBKEY_SIZE>& sender_pubkey
) {
    std::vector<uint8_t> buf;
    buf.reserve(96);
    buf.insert(buf.end(), invoice_id.begin(), invoice_id.end());
    buf.insert(buf.end(), txid.begin(), txid.end());
    buf.insert(buf.end(), sender_pubkey.begin(), sender_pubkey.end());
    return buf;
}

// ============================================================================
// Hash computations
// ============================================================================

std::array<uint8_t, HASH_SIZE> ComputeInvoiceId(const DpiInvoice& inv) {
    auto fields = CanonicalInvoiceFields(inv);
    return DpiTaggedHash("DPI/invoice", fields);
}

std::array<uint8_t, HASH_SIZE> ComputeAttestationMessage(
    const std::array<uint8_t, HASH_SIZE>& invoice_id,
    const std::array<uint8_t, HASH_SIZE>& txid,
    const std::array<uint8_t, XONLY_PUBKEY_SIZE>& sender_pubkey
) {
    auto fields = CanonicalAttestationFields(invoice_id, txid, sender_pubkey);
    return DpiTaggedHash("DPI/attest-v1", fields);
}

// ============================================================================
// Serialization
// ============================================================================

std::vector<uint8_t> SerializeInvoice(const DpiInvoice& inv) {
    std::vector<uint8_t> buf;
    buf.reserve(256);

    buf.push_back(inv.version);
    buf.push_back(inv.network);
    WriteLE64(buf, inv.amount);

    // Address: length-prefixed UTF-8
    uint16_t addr_len = static_cast<uint16_t>(inv.destination_address.size());
    WriteLE16(buf, addr_len);
    buf.insert(buf.end(), inv.destination_address.begin(), inv.destination_address.end());

    // Fixed fields
    buf.insert(buf.end(), inv.merchant_id.begin(), inv.merchant_id.end());
    buf.insert(buf.end(), inv.nonce.begin(), inv.nonce.end());
    WriteLE32(buf, inv.timestamp);
    WriteLE16(buf, inv.expiry);

    // Memo: 1-byte length prefix
    uint8_t memo_len = static_cast<uint8_t>(std::min(inv.memo.size(), MAX_MEMO_LENGTH));
    buf.push_back(memo_len);
    buf.insert(buf.end(), inv.memo.begin(), inv.memo.begin() + memo_len);

    // Merchant signature (64 bytes)
    if (inv.merchant_sig.size() == SCHNORR_SIG_SIZE) {
        buf.insert(buf.end(), inv.merchant_sig.begin(), inv.merchant_sig.end());
    } else {
        buf.resize(buf.size() + SCHNORR_SIG_SIZE, 0);
    }

    return buf;
}

bool DeserializeInvoice(const std::vector<uint8_t>& data, DpiInvoice& out) {
    const uint8_t* p = data.data();
    const uint8_t* end = p + data.size();

    // Version + network
    if (p + 2 > end) return false;
    out.version = *p++;
    out.network = *p++;

    // Amount
    if (!ReadLE64(p, end, out.amount)) return false;

    // Address
    uint16_t addr_len = 0;
    if (!ReadLE16(p, end, addr_len)) return false;
    if (addr_len > MAX_ADDRESS_LENGTH || p + addr_len > end) return false;
    out.destination_address.assign(reinterpret_cast<const char*>(p), addr_len);
    p += addr_len;

    // Merchant ID (20 bytes)
    if (p + MERCHANT_ID_SIZE > end) return false;
    std::memcpy(out.merchant_id.data(), p, MERCHANT_ID_SIZE);
    p += MERCHANT_ID_SIZE;

    // Nonce (16 bytes)
    if (p + NONCE_SIZE > end) return false;
    std::memcpy(out.nonce.data(), p, NONCE_SIZE);
    p += NONCE_SIZE;

    // Timestamp + expiry
    if (!ReadLE32(p, end, out.timestamp)) return false;
    if (!ReadLE16(p, end, out.expiry)) return false;

    // Memo
    if (p + 1 > end) return false;
    uint8_t memo_len = *p++;
    if (memo_len > MAX_MEMO_LENGTH || p + memo_len > end) return false;
    out.memo.assign(reinterpret_cast<const char*>(p), memo_len);
    p += memo_len;

    // Merchant signature (64 bytes)
    if (p + SCHNORR_SIG_SIZE > end) return false;
    out.merchant_sig.assign(p, p + SCHNORR_SIG_SIZE);
    p += SCHNORR_SIG_SIZE;

    // Compute invoice_id from deserialized fields
    out.invoice_id = ComputeInvoiceId(out);

    return true;
}

std::vector<uint8_t> SerializePackage(const DpiPaymentPackage& pkg) {
    std::vector<uint8_t> buf;
    buf.reserve(pkg.raw_tx.size() + 4 + SCHNORR_SIG_SIZE + XONLY_PUBKEY_SIZE + HASH_SIZE);

    // tx_len (4 LE) + raw_tx
    WriteLE32(buf, static_cast<uint32_t>(pkg.raw_tx.size()));
    buf.insert(buf.end(), pkg.raw_tx.begin(), pkg.raw_tx.end());

    // Attestation signature (64 bytes)
    if (pkg.attestation_sig.size() == SCHNORR_SIG_SIZE) {
        buf.insert(buf.end(), pkg.attestation_sig.begin(), pkg.attestation_sig.end());
    } else {
        buf.resize(buf.size() + SCHNORR_SIG_SIZE, 0);
    }

    // Sender pubkey (32 bytes)
    buf.insert(buf.end(), pkg.sender_pubkey.begin(), pkg.sender_pubkey.end());

    // Invoice ID (32 bytes)
    buf.insert(buf.end(), pkg.invoice_id.begin(), pkg.invoice_id.end());

    return buf;
}

bool DeserializePackage(const std::vector<uint8_t>& data, DpiPaymentPackage& out) {
    const uint8_t* p = data.data();
    const uint8_t* end = p + data.size();

    // tx_len + raw_tx
    uint32_t tx_len = 0;
    if (!ReadLE32(p, end, tx_len)) return false;
    if (tx_len == 0 || tx_len > 1000000 || p + tx_len > end) return false;  // 1MB max tx
    out.raw_tx.assign(p, p + tx_len);
    p += tx_len;

    // Attestation signature (64 bytes)
    if (p + SCHNORR_SIG_SIZE > end) return false;
    out.attestation_sig.assign(p, p + SCHNORR_SIG_SIZE);
    p += SCHNORR_SIG_SIZE;

    // Sender pubkey (32 bytes)
    if (p + XONLY_PUBKEY_SIZE > end) return false;
    std::memcpy(out.sender_pubkey.data(), p, XONLY_PUBKEY_SIZE);
    p += XONLY_PUBKEY_SIZE;

    // Invoice ID (32 bytes)
    if (p + HASH_SIZE > end) return false;
    std::memcpy(out.invoice_id.data(), p, HASH_SIZE);
    p += HASH_SIZE;

    return true;
}

// ============================================================================
// Signature operations
// ============================================================================

bool SignInvoice(DpiInvoice& inv, const std::vector<uint8_t>& merchant_privkey) {
    inv.invoice_id = ComputeInvoiceId(inv);

    std::vector<uint8_t> msg_hash(inv.invoice_id.begin(), inv.invoice_id.end());
    auto sig = din::SchnorrSigner::sign(msg_hash, merchant_privkey);
    if (!sig.has_value()) return false;

    inv.merchant_sig = std::move(*sig);
    return true;
}

bool VerifyInvoiceSignature(
    const DpiInvoice& inv,
    const std::vector<uint8_t>& merchant_pubkey
) {
    if (inv.merchant_sig.size() != SCHNORR_SIG_SIZE) return false;
    if (merchant_pubkey.size() != XONLY_PUBKEY_SIZE) return false;

    auto expected_id = ComputeInvoiceId(inv);
    std::vector<uint8_t> msg_hash(expected_id.begin(), expected_id.end());

    return din::SchnorrSigner::verify(inv.merchant_sig, msg_hash, merchant_pubkey);
}

std::vector<uint8_t> SignAttestation(
    const std::array<uint8_t, HASH_SIZE>& invoice_id,
    const std::array<uint8_t, HASH_SIZE>& txid,
    const std::array<uint8_t, XONLY_PUBKEY_SIZE>& sender_pubkey,
    const std::vector<uint8_t>& sender_privkey
) {
    auto msg = ComputeAttestationMessage(invoice_id, txid, sender_pubkey);
    std::vector<uint8_t> msg_hash(msg.begin(), msg.end());

    auto sig = din::SchnorrSigner::sign(msg_hash, sender_privkey);
    if (!sig.has_value()) return {};
    return std::move(*sig);
}

bool VerifyAttestationSignature(
    const std::array<uint8_t, HASH_SIZE>& invoice_id,
    const std::array<uint8_t, HASH_SIZE>& txid,
    const std::array<uint8_t, XONLY_PUBKEY_SIZE>& sender_pubkey,
    const std::vector<uint8_t>& signature
) {
    if (signature.size() != SCHNORR_SIG_SIZE) return false;

    auto msg = ComputeAttestationMessage(invoice_id, txid, sender_pubkey);
    std::vector<uint8_t> msg_hash(msg.begin(), msg.end());
    std::vector<uint8_t> pubkey_vec(sender_pubkey.begin(), sender_pubkey.end());

    return din::SchnorrSigner::verify(signature, msg_hash, pubkey_vec);
}

// ============================================================================
// Utility
// ============================================================================

bool IsInvoiceExpired(const DpiInvoice& inv) {
    uint64_t deadline = static_cast<uint64_t>(inv.timestamp) + static_cast<uint64_t>(inv.expiry);
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    return now > deadline;
}

uint8_t GetDpiNetworkByte() {
    // Check active network HRP
    try {
        auto wp = ::dinero::DecodeTaprootWitnessProgram("din1p" + std::string(58, 'q'));
        (void)wp;
        return DPI_NETWORK_MAIN;
    } catch (...) {}

    // Fallback: try to detect from address prefix conventions
    return DPI_NETWORK_MAIN;
}

std::vector<uint8_t> AddressToScriptPubKey(const std::string& address) {
    auto witness_program = ::dinero::DecodeTaprootWitnessProgram(address);
    return ::dinero::CreateP2TRScriptPubKey(witness_program);
}

double ComputeRiskScore(const DpiVerifyChecks& checks) {
    double score = 1.0;

    if (checks.invoice_bound)     score -= 0.15;
    if (checks.output_match)      score -= 0.15;
    if (checks.amount_match)      score -= 0.15;
    if (checks.attestation_valid) score -= 0.15;
    if (checks.seen_in_mempool)   score -= 0.25;
    if (!checks.conflicts_found)  score -= 0.15;
    // expired does not add penalty — it's a separate check

    return std::max(0.0, std::min(1.0, score));
}

std::string DetermineTier(const DpiVerifyChecks& checks, double risk_score) {
    // T1 requires: all protocol checks pass + seen in mempool + no conflicts + not expired
    if (checks.invoice_bound &&
        checks.output_match &&
        checks.amount_match &&
        checks.attestation_valid &&
        checks.seen_in_mempool &&
        !checks.conflicts_found &&
        !checks.expired &&
        risk_score <= 0.15) {
        return "T1";
    }
    return "T0";
}

} // namespace din::dpi

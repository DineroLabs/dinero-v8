#include "wallet/receipt_bundle.h"
#include "wallet/schnorr_signer.h"
#include "crypto/sha256.h"
#include <cstring>
#include <stdexcept>

namespace dinero {

// ============================================================================
// AttestorReceipt
// ============================================================================

std::vector<uint8_t> AttestorReceipt::Serialize() const {
    // pubkey(32) || sig(64)
    std::vector<uint8_t> out;
    out.reserve(96);
    out.insert(out.end(), attestor_pubkey.begin(), attestor_pubkey.end());
    out.insert(out.end(), signature.begin(), signature.end());
    return out;
}

AttestorReceipt AttestorReceipt::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 96) {
        throw std::runtime_error("AttestorReceipt: data too short");
    }
    AttestorReceipt r;
    r.attestor_pubkey.assign(data.begin(), data.begin() + 32);
    r.signature.assign(data.begin() + 32, data.begin() + 96);
    return r;
}

// ============================================================================
// ReceiptBundle
// ============================================================================

bool ReceiptBundle::VerifyReceipts(const std::array<uint8_t, 32>& outcome_hash) const {
    // Verify each receipt: sig over (trade_id || outcome_hash)
    std::vector<uint8_t> message(64);
    std::memcpy(message.data(), trade_id.data(), 32);
    std::memcpy(message.data() + 32, outcome_hash.data(), 32);

    // Hash the message for Schnorr verification
    uint8_t msg_hash[32];
    crypto::CSHA256().Write(message.data(), message.size()).Finalize(msg_hash);
    std::vector<uint8_t> msg_hash_vec(msg_hash, msg_hash + 32);

    for (const auto& receipt : receipts) {
        if (receipt.attestor_pubkey.size() != 32 || receipt.signature.size() != 64) {
            return false;
        }
        if (!din::SchnorrSigner::verify(receipt.signature, msg_hash_vec, receipt.attestor_pubkey)) {
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> ReceiptBundle::Serialize() const {
    // trade_id(32) || n_receipts(1) || receipt_1(96) || receipt_2(96) || ...
    std::vector<uint8_t> out;
    uint8_t n = static_cast<uint8_t>(receipts.size());
    out.reserve(32 + 1 + 96 * n);

    out.insert(out.end(), trade_id.begin(), trade_id.end());
    out.push_back(n);

    for (const auto& r : receipts) {
        auto ser = r.Serialize();
        out.insert(out.end(), ser.begin(), ser.end());
    }
    return out;
}

ReceiptBundle ReceiptBundle::Deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 33) {
        throw std::runtime_error("ReceiptBundle: data too short");
    }

    ReceiptBundle bundle;
    std::copy(data.begin(), data.begin() + 32, bundle.trade_id.begin());

    uint8_t n = data[32];
    size_t off = 33;

    if (data.size() < 33 + 96 * n) {
        throw std::runtime_error("ReceiptBundle: not enough receipt data");
    }

    bundle.receipts.resize(n);
    for (uint8_t i = 0; i < n; i++) {
        std::vector<uint8_t> chunk(data.begin() + off, data.begin() + off + 96);
        bundle.receipts[i] = AttestorReceipt::Deserialize(chunk);
        off += 96;
    }
    return bundle;
}

} // namespace dinero

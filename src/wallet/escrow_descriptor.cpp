#include "wallet/escrow_descriptor.h"
#include "crypto/tagged_hash.h"
#include <cstring>
#include <stdexcept>

namespace dinero {

using crypto::TaggedHashArray;

std::array<uint8_t, 32> EscrowDescriptor::ComputePendingTradeId(
    const std::vector<uint8_t>& nonce,
    const std::vector<uint8_t>& buyer_pubkey,
    const std::vector<uint8_t>& seller_pubkey,
    uint64_t amount) {

    // pending_trade_id = TaggedHash("dinero/trade/pending/v1",
    //     nonce || buyer(32) || seller(32) || amount(8 LE))
    std::vector<uint8_t> preimage;
    preimage.reserve(nonce.size() + 32 + 32 + 8);

    preimage.insert(preimage.end(), nonce.begin(), nonce.end());
    preimage.insert(preimage.end(), buyer_pubkey.begin(), buyer_pubkey.end());
    preimage.insert(preimage.end(), seller_pubkey.begin(), seller_pubkey.end());

    for (int i = 0; i < 8; i++) {
        preimage.push_back(static_cast<uint8_t>((amount >> (i * 8)) & 0xff));
    }

    return TaggedHashArray("dinero/trade/pending/v1", preimage.data(), preimage.size());
}

std::array<uint8_t, 32> EscrowDescriptor::ComputeFinalizedTradeId(
    const std::array<uint8_t, 32>& funding_txid,
    uint32_t funding_vout) const {

    // finalized_trade_id = TaggedHash("dinero/trade/final/v1",
    //     funding_txid(32) || funding_vout(4 LE))
    uint8_t preimage[36];
    std::memcpy(preimage, funding_txid.data(), 32);
    preimage[32] = funding_vout & 0xff;
    preimage[33] = (funding_vout >> 8) & 0xff;
    preimage[34] = (funding_vout >> 16) & 0xff;
    preimage[35] = (funding_vout >> 24) & 0xff;

    return TaggedHashArray("dinero/trade/final/v1", preimage, 36);
}

std::vector<uint8_t> EscrowDescriptor::Serialize() const {
    // buyer(32) || seller(32) || n_attestors(1) || threshold(1) ||
    // attestors(32*n) || timeout(4 LE) || amount(8 LE)
    std::vector<uint8_t> out;
    uint8_t n = static_cast<uint8_t>(attestor_pubkeys.size());
    out.reserve(32 + 32 + 1 + 1 + 32 * n + 4 + 8);

    out.insert(out.end(), buyer_pubkey.begin(), buyer_pubkey.end());
    out.insert(out.end(), seller_pubkey.begin(), seller_pubkey.end());
    out.push_back(n);
    out.push_back(attestor_threshold);

    for (const auto& pk : attestor_pubkeys) {
        out.insert(out.end(), pk.begin(), pk.end());
    }

    out.push_back(timeout_blocks & 0xff);
    out.push_back((timeout_blocks >> 8) & 0xff);
    out.push_back((timeout_blocks >> 16) & 0xff);
    out.push_back((timeout_blocks >> 24) & 0xff);

    for (int i = 0; i < 8; i++) {
        out.push_back(static_cast<uint8_t>((escrow_amount >> (i * 8)) & 0xff));
    }

    return out;
}

EscrowDescriptor EscrowDescriptor::Deserialize(const std::vector<uint8_t>& data) {
    // Minimum: buyer(32) + seller(32) + n(1) + threshold(1) + timeout(4) + amount(8) = 78
    if (data.size() < 78) {
        throw std::runtime_error("EscrowDescriptor: data too short");
    }

    EscrowDescriptor d;
    size_t off = 0;

    d.buyer_pubkey.assign(data.begin() + off, data.begin() + off + 32);
    off += 32;
    d.seller_pubkey.assign(data.begin() + off, data.begin() + off + 32);
    off += 32;

    uint8_t n = data[off++];
    d.attestor_threshold = data[off++];

    if (data.size() < 78 + 32 * n) {
        throw std::runtime_error("EscrowDescriptor: not enough attestor data");
    }

    d.attestor_pubkeys.resize(n);
    for (uint8_t i = 0; i < n; i++) {
        d.attestor_pubkeys[i].assign(data.begin() + off, data.begin() + off + 32);
        off += 32;
    }

    d.timeout_blocks = static_cast<uint32_t>(data[off])
        | (static_cast<uint32_t>(data[off + 1]) << 8)
        | (static_cast<uint32_t>(data[off + 2]) << 16)
        | (static_cast<uint32_t>(data[off + 3]) << 24);
    off += 4;

    d.escrow_amount = 0;
    for (int i = 0; i < 8; i++) {
        d.escrow_amount |= static_cast<uint64_t>(data[off + i]) << (i * 8);
    }

    return d;
}

} // namespace dinero

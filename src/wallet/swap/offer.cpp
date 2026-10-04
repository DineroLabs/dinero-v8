#include "wallet/swap/offer.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <stdexcept>
#include <vector>

namespace dinero::swap {
namespace {

constexpr uint8_t kAcceptVersion = 1;
constexpr size_t kOfferPayloadSize = 1 + 1 + 8 + 8 + 32 + 32 + 33 + 5 * 4;  // 135
constexpr size_t kAcceptPayloadSize = 1 + 32 + 32 + 33;                    // 98
constexpr size_t kChecksumSize = 4;
// Bob must have time to lock BTC and Alice to claim it before her cut-off
// (T_btc - 6 h, design §6.1) after the offer is accepted at the latest.
constexpr uint32_t kMinExpiryBeforeBtcDeadline = 12 * 60 * 60;
// Design §6.1 abort rule: Bob does not commit when the DIN deadline is close.
constexpr uint32_t kMinDinDeadlineAhead = 36 * 60 * 60;

[[noreturn]] void Refuse(const std::string& why) {
    throw std::invalid_argument("swap message refused: " + why);
}

Bytes32 Sha256(const std::vector<uint8_t>& data) {
    Bytes32 out{};
    crypto::CSHA256().Write(data.data(), data.size()).Finalize(out.data());
    return out;
}

void RequireXOnly(const Bytes32& key, const char* what) {
    secp256k1_xonly_pubkey parsed;
    if (secp256k1_xonly_pubkey_parse(crypto::GetSecp256k1ContextVerify(), &parsed, key.data()) != 1) {
        Refuse(std::string(what) + " is not a valid x-only key");
    }
}

void RequireCompressed(const std::array<uint8_t, 33>& key, const char* what) {
    secp256k1_pubkey parsed;
    if ((key[0] != 0x02 && key[0] != 0x03) ||
        secp256k1_ec_pubkey_parse(crypto::GetSecp256k1ContextVerify(), &parsed, key.data(),
                                  key.size()) != 1) {
        Refuse(std::string(what) + " is not a valid compressed key");
    }
}

void ValidateOffer(const SwapOffer& o) {
    if (o.network != SwapNetwork::Mainnet && o.network != SwapNetwork::Testnet &&
        o.network != SwapNetwork::Regtest) {
        Refuse("unknown network");
    }
    if (o.din_amount_una == 0 || o.btc_amount_sat == 0) Refuse("amounts must be positive");
    RequireXOnly(o.din_refund_pubkey, "DIN refund key");
    RequireCompressed(o.btc_claim_pubkey, "BTC claim key");
    for (uint32_t t : {o.t_din_unix, o.t_btc_unix, o.expires_unix}) {
        if (t < kTimestampLockThreshold) Refuse("deadlines must be Unix timestamps, not heights");
    }
    if (uint64_t(o.t_din_unix) < uint64_t(o.t_btc_unix) + kMinDeadlineGapSeconds) {
        Refuse("DIN deadline must be at least 24 h after the BTC deadline");
    }
    if (uint64_t(o.t_btc_unix) < uint64_t(o.expires_unix) + kMinExpiryBeforeBtcDeadline) {
        Refuse("offer must expire at least 12 h before the BTC deadline");
    }
    if (o.n_din_confirmations < kMinDinConfirmations) Refuse("too few DIN confirmations");
    if (o.n_btc_confirmations < kMinBtcConfirmations) Refuse("too few BTC confirmations");
}

void ValidateAccept(const SwapAccept& a) {
    RequireXOnly(a.din_claim_pubkey, "DIN claim key");
    RequireCompressed(a.btc_refund_pubkey, "BTC refund key");
}

void PutLE(std::vector<uint8_t>& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

template <size_t N>
void PutBytes(std::vector<uint8_t>& out, const std::array<uint8_t, N>& a) {
    out.insert(out.end(), a.begin(), a.end());
}

struct Reader {
    const std::vector<uint8_t>& in;
    size_t pos = 0;
    uint64_t LE(int bytes) {
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= uint64_t(in.at(pos++)) << (8 * i);
        return v;
    }
    template <size_t N>
    void Bytes(std::array<uint8_t, N>& a) {
        for (auto& b : a) b = in.at(pos++);
    }
};

std::vector<uint8_t> OfferPayload(const SwapOffer& o) {
    std::vector<uint8_t> p;
    p.push_back(kOfferVersion);
    p.push_back(static_cast<uint8_t>(o.network));
    PutLE(p, o.din_amount_una, 8);
    PutLE(p, o.btc_amount_sat, 8);
    PutBytes(p, o.payment_hash);
    PutBytes(p, o.din_refund_pubkey);
    PutBytes(p, o.btc_claim_pubkey);
    PutLE(p, o.t_din_unix, 4);
    PutLE(p, o.t_btc_unix, 4);
    PutLE(p, o.n_din_confirmations, 4);
    PutLE(p, o.n_btc_confirmations, 4);
    PutLE(p, o.expires_unix, 4);
    return p;
}

std::string Seal(const char* prefix, const std::vector<uint8_t>& payload) {
    const auto sum = Sha256(payload);
    static const char* digits = "0123456789abcdef";
    std::string text = prefix;
    auto put = [&](uint8_t b) { text += digits[b >> 4]; text += digits[b & 15]; };
    for (auto b : payload) put(b);
    for (size_t i = 0; i < kChecksumSize; ++i) put(sum[i]);
    return text;
}

std::vector<uint8_t> Open(const std::string& text, const char* prefix, size_t payload_size) {
    const std::string p(prefix);
    if (text.compare(0, p.size(), p) != 0) Refuse("wrong message type");
    const std::string hex = text.substr(p.size());
    if (hex.size() != 2 * (payload_size + kChecksumSize)) Refuse("wrong length");
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) Refuse("not lowercase hex");
        bytes.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    std::vector<uint8_t> payload(bytes.begin(), bytes.end() - kChecksumSize);
    const auto sum = Sha256(payload);
    if (!std::equal(sum.begin(), sum.begin() + kChecksumSize, bytes.end() - kChecksumSize)) {
        Refuse("checksum mismatch");
    }
    return payload;
}

void RequireMatched(const SwapOffer& offer, const SwapAccept& accept) {
    ValidateOffer(offer);
    ValidateAccept(accept);
    if (accept.offer_id != OfferId(offer)) Refuse("accept is for a different offer");
}

}  // namespace

std::string EncodeOffer(const SwapOffer& offer) {
    ValidateOffer(offer);
    return Seal(kOfferPrefix, OfferPayload(offer));
}

SwapOffer DecodeOffer(const std::string& text) {
    const auto payload = Open(text, kOfferPrefix, kOfferPayloadSize);
    Reader r{payload};
    if (r.LE(1) != kOfferVersion) Refuse("unsupported offer version");
    SwapOffer o;
    const auto network = r.LE(1);
    if (network > static_cast<uint8_t>(SwapNetwork::Regtest)) Refuse("unknown network");
    o.network = static_cast<SwapNetwork>(network);
    o.din_amount_una = r.LE(8);
    o.btc_amount_sat = r.LE(8);
    r.Bytes(o.payment_hash);
    r.Bytes(o.din_refund_pubkey);
    r.Bytes(o.btc_claim_pubkey);
    o.t_din_unix = static_cast<uint32_t>(r.LE(4));
    o.t_btc_unix = static_cast<uint32_t>(r.LE(4));
    o.n_din_confirmations = static_cast<uint32_t>(r.LE(4));
    o.n_btc_confirmations = static_cast<uint32_t>(r.LE(4));
    o.expires_unix = static_cast<uint32_t>(r.LE(4));
    ValidateOffer(o);
    return o;
}

std::string EncodeAccept(const SwapAccept& accept) {
    ValidateAccept(accept);
    std::vector<uint8_t> p;
    p.push_back(kAcceptVersion);
    PutBytes(p, accept.offer_id);
    PutBytes(p, accept.din_claim_pubkey);
    PutBytes(p, accept.btc_refund_pubkey);
    return Seal(kAcceptPrefix, p);
}

SwapAccept DecodeAccept(const std::string& text) {
    const auto payload = Open(text, kAcceptPrefix, kAcceptPayloadSize);
    Reader r{payload};
    if (r.LE(1) != kAcceptVersion) Refuse("unsupported accept version");
    SwapAccept a;
    r.Bytes(a.offer_id);
    r.Bytes(a.din_claim_pubkey);
    r.Bytes(a.btc_refund_pubkey);
    ValidateAccept(a);
    return a;
}

Bytes32 OfferId(const SwapOffer& offer) { return Sha256(OfferPayload(offer)); }

DinHtlcTerms MakeDinTerms(const SwapOffer& offer, const SwapAccept& accept) {
    RequireMatched(offer, accept);
    if (accept.din_claim_pubkey == offer.din_refund_pubkey) {
        Refuse("DIN claim and refund keys must belong to different parties");
    }
    DinHtlcTerms t;
    t.payment_hash = offer.payment_hash;
    t.claim_pubkey = accept.din_claim_pubkey;   // Bob
    t.refund_pubkey = offer.din_refund_pubkey;  // Alice
    t.refund_locktime_unix = offer.t_din_unix;
    return t;
}

BtcHtlcTerms MakeBtcTerms(const SwapOffer& offer, const SwapAccept& accept) {
    RequireMatched(offer, accept);
    if (accept.btc_refund_pubkey == offer.btc_claim_pubkey) {
        Refuse("BTC claim and refund keys must belong to different parties");
    }
    BtcHtlcTerms t;
    t.payment_hash = offer.payment_hash;
    t.claim_pubkey = offer.btc_claim_pubkey;     // Alice
    t.refund_pubkey = accept.btc_refund_pubkey;  // Bob
    t.refund_locktime_unix = offer.t_btc_unix;
    return t;
}

void RequireAcceptableNow(const SwapOffer& offer, uint32_t now_unix) {
    if (now_unix > offer.expires_unix) Refuse("offer has expired");
    if (uint64_t(offer.t_din_unix) < uint64_t(now_unix) + kMinDinDeadlineAhead) {
        Refuse("DIN deadline is less than 36 h away");
    }
}

}  // namespace dinero::swap

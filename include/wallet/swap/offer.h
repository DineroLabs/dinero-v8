#pragma once
// Copy-paste messages that set up a DIN <-> BTC swap (milestone 1 of
// docs/design/din-btc-atomic-swaps-v1-plan.md).
//
// Alice (sells DIN, holds the secret) publishes an OFFER; Bob (sells BTC)
// answers with an ACCEPT bound to that exact offer. Neither message carries a
// secret or a private key. Decoding is strict: a wrong prefix, checksum,
// version, network, length, key, amount, deadline or confirmation count is
// refused before anything is built, so no money moves on a malformed swap.
//
// Wire form: "<prefix><hex(payload || first 4 bytes of SHA256(payload))>".

#include "wallet/swap/htlc.h"

#include <array>
#include <cstdint>
#include <string>

namespace dinero::swap {

enum class SwapNetwork : uint8_t { Mainnet = 0, Testnet = 1, Regtest = 2 };

inline constexpr uint8_t kOfferVersion = 1;
inline constexpr char kOfferPrefix[] = "dinswap1o";
inline constexpr char kAcceptPrefix[] = "dinswap1a";

// Design §6: Bob needs >= 24 h between the BTC and DIN refund deadlines, and
// at least 30 DIN confirmations before he locks BTC.
inline constexpr uint32_t kMinDeadlineGapSeconds = 24 * 60 * 60;
inline constexpr uint32_t kMinDinConfirmations = 30;
inline constexpr uint32_t kMinBtcConfirmations = 1;

struct SwapOffer {
    SwapNetwork network{SwapNetwork::Mainnet};
    uint64_t din_amount_una{};
    uint64_t btc_amount_sat{};
    Bytes32 payment_hash{};                    // SHA256(secret); Alice keeps the secret
    Bytes32 din_refund_pubkey{};               // Alice, x-only (refunds DIN)
    std::array<uint8_t, 33> btc_claim_pubkey{};  // Alice, compressed (claims BTC)
    uint32_t t_din_unix{};                     // DIN refund opens (Alice)
    uint32_t t_btc_unix{};                     // BTC refund opens (Bob)
    uint32_t n_din_confirmations{};            // Bob waits this deep before locking BTC
    uint32_t n_btc_confirmations{};            // Alice waits this deep before claiming
    uint32_t expires_unix{};                   // offer cannot be accepted after this
};

struct SwapAccept {
    Bytes32 offer_id{};                         // OfferId() of the accepted offer
    Bytes32 din_claim_pubkey{};                 // Bob, x-only (claims DIN)
    std::array<uint8_t, 33> btc_refund_pubkey{};  // Bob, compressed (refunds BTC)
};

// Throw std::invalid_argument on any rule violation (see file comment).
std::string EncodeOffer(const SwapOffer& offer);
SwapOffer DecodeOffer(const std::string& text);
std::string EncodeAccept(const SwapAccept& accept);
SwapAccept DecodeAccept(const std::string& text);

// SHA256 of the offer's canonical payload: what an accept commits to.
Bytes32 OfferId(const SwapOffer& offer);

// HTLC terms for both chains from a matched offer + accept. Throws if the
// accept is for a different offer. DIN: Bob claims, Alice refunds.
// BTC: Alice claims, Bob refunds.
DinHtlcTerms MakeDinTerms(const SwapOffer& offer, const SwapAccept& accept);
BtcHtlcTerms MakeBtcTerms(const SwapOffer& offer, const SwapAccept& accept);

// Taker-side timing check at acceptance time (design §6.1 abort rule): refuses
// an expired offer or one whose DIN deadline is less than 36 h away.
void RequireAcceptableNow(const SwapOffer& offer, uint32_t now_unix);

}  // namespace dinero::swap

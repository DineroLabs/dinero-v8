#pragma once
// Sending from the built-in Bitcoin wallet (docs/design/multi-coin-wallet.md,
// section 5): planning a payment and signing it. Pure: which coins exist is
// the caller's knowledge, and nothing here talks to a network.
//
// A payment is planned once, without any key, and that plan is what the user
// is shown. Signing fills in the witnesses and then checks the signed bytes
// against the plan, output by output: what is sent is what was shown.
//
// Every input signals replacement (BIP125), so a stuck payment can later be
// re-sent with a higher fee. Swap locks do not use this code: they must not
// be replaceable.

#include "wallet/swap/btc_tx.h"
#include "wallet/swap/btc_wallet_keys.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dinero::swap {

inline constexpr uint64_t kBtcWalletDustSat = 546;   // no output below this is created
inline constexpr size_t kBtcWalletMaxInputs = 50;    // in one payment
inline constexpr uint32_t kBtcReplaceableSequence = 0xfffffffd;

struct BtcWalletCoin {
    Bytes32 txid{};  // wire byte order
    uint32_t vout{};
    uint64_t value_sat{};
    uint32_t branch{}, index{};  // whose key: account/branch/index
};

struct BtcSendRequest {
    std::vector<uint8_t> to_script;
    uint64_t amount_sat{};          // ignored with send_all
    bool send_all{false};           // everything spendable, less the fee, no change
    double feerate_sat_per_vb{};
    uint64_t max_fee_sat{};         // the plan fails rather than pay more
    std::vector<uint8_t> change_script;  // a change address of this wallet
};

struct BtcSendPlan {
    BtcTx tx;                        // unsigned
    std::vector<BtcWalletCoin> used; // tx.vin[i] spends used[i]
    uint64_t amount_sat{};           // what arrives at to_script (output 0)
    uint64_t fee_sat{};
    uint64_t change_sat{};           // 0: no change output
    uint64_t dust_to_fee_sat{};      // part of fee_sat: change too small to be an output
    uint64_t vsize{};                // an upper bound of the signed size
};

// Coins are taken largest first, only as many as needed; a coin that costs
// more to spend than it holds at this fee rate is left alone. Throws
// std::invalid_argument for a request that cannot be a payment, and
// std::runtime_error("not enough bitcoin: …") with the shortfall.
BtcSendPlan PlanBtcSend(const std::vector<BtcWalletCoin>& coins, const BtcSendRequest& request);

// The same payment with a higher fee, taken from its change: the same coins,
// the same destination and amount, so the network replaces the old one
// (BIP125). `sent` is the payment as it was broadcast, `coins` its inputs in
// order. The new fee is the larger of the new rate times the size and the old
// fee plus one sat per virtual byte (what nodes ask of a replacement). Throws
// std::runtime_error when the payment has no change, or its change cannot
// carry the higher fee, and std::invalid_argument when the fee would be above
// max_fee_sat or `sent` is not a payment of this shape.
BtcSendPlan PlanBtcFeeBump(const BtcTx& sent, const std::vector<BtcWalletCoin>& coins, double feerate_sat_per_vb,
                           uint64_t max_fee_sat);

struct BtcSignedSend {
    std::vector<uint8_t> raw;  // with witnesses
    Bytes32 txid{};            // wire byte order
};

// `key_of(branch, index)` returns that address's private key. Throws
// std::runtime_error if a key does not own its coin, or if the signed
// transaction differs from the plan in any input, output or the fee.
BtcSignedSend SignBtcSend(const BtcSendPlan& plan, const BtcWalletAccount& account,
                          const std::function<Bytes32(uint32_t branch, uint32_t index)>& key_of);

}  // namespace dinero::swap

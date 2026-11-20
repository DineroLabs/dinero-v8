#pragma once
// Paying BTC into a swap without a Bitcoin wallet
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3.3).
//
// A buyer of DIN pays BTC to a deposit address whose key the node derives from
// its wallet seed, from any wallet or exchange and in as many payments as they
// like. The node then builds the swap's lock transaction itself from those
// coins. Everything here is pure transaction building: which coins exist and
// how deep they are is the caller's knowledge (proven, see btc_light_watcher).
//
// The lock's id is fixed before it is sent: every input is segwit and the node
// is the only signer, so nothing a third party does can change it. That is
// what the watchtower package and the pinned watcher rely on.

#include "wallet/swap/btc_tx.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero::swap {

// Outputs below this are not created: the change goes to the fee instead.
inline constexpr uint64_t kBtcDustSat = 546;
// Deposit coins below this are never spent: anyone who knows the address
// could otherwise make the lock arbitrarily large (and its fee too small).
inline constexpr uint64_t kBtcMinDepositCoinSat = 2000;
// Inputs in one lock or sweep.
inline constexpr size_t kBtcMaxDepositInputs = 20;

// The pay-to-witness-public-key-hash script of a compressed public key, and
// its address ("bc1q…", "tb1q…", "bcrt1q…").
std::vector<uint8_t> BtcP2wpkhScript(const std::array<uint8_t, 33>& pubkey);
std::string BtcP2wpkhAddress(const std::array<uint8_t, 33>& pubkey, const std::string& hrp);

struct BtcDepositCoin {
    std::array<uint8_t, 32> txid{};  // wire byte order
    uint32_t vout{};
    uint64_t value_sat{};
};

struct BtcDepositSpend {
    BtcTx tx;                       // signed
    std::vector<uint8_t> raw;       // its serialization, with witness
    Bytes32 txid{};                 // wire byte order
    uint64_t fee_sat{};
    uint64_t change_sat{};          // 0: no change output
    uint64_t vsize{};               // virtual bytes the fee was computed for (an upper bound of the real size)
    std::vector<BtcDepositCoin> used;
};

// The lock: output 0 pays `htlc_script_pubkey` exactly `amount_sat`; output 1,
// if any, pays the rest to `change_script`. Coins are taken largest first,
// only as many as needed. The fee is `feerate_sat_per_vb` times the
// transaction's size, and never above `max_fee_sat`.
//
// Throws std::runtime_error("deposit is short by N sat …") when the usable
// coins do not cover the amount and the fee, and std::invalid_argument for
// parameters that cannot make a transaction (including a fee rate that would
// need more than max_fee_sat).
BtcDepositSpend BuildBtcLockFromDeposit(const std::vector<BtcDepositCoin>& coins, const Bytes32& deposit_secret_key,
                                        const std::vector<uint8_t>& htlc_script_pubkey, uint64_t amount_sat,
                                        const std::vector<uint8_t>& change_script, double feerate_sat_per_vb,
                                        uint64_t max_fee_sat);

// Sends every usable coin (up to kBtcMaxDepositInputs, largest first) to
// `to_script`, less the fee: the deposit going back to its owner. Throws
// std::runtime_error when nothing usable is there or the fee would leave dust.
BtcDepositSpend BuildBtcDepositSweep(const std::vector<BtcDepositCoin>& coins, const Bytes32& deposit_secret_key,
                                     const std::vector<uint8_t>& to_script, double feerate_sat_per_vb,
                                     uint64_t max_fee_sat);

}  // namespace dinero::swap

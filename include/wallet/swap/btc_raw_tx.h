#pragma once
// Reads any Bitcoin transaction, for swaps without Bitcoin Core
// (docs/design/din-btc-swaps-without-bitcoin-core.md).
//
// btc_tx.h builds and parses the swap's own transactions only (no signature
// scripts). A light node is handed other people's transactions by servers and
// peers: a counterparty's lock paid from any wallet, the spend that reveals the
// secret, a block's coinbase. This parser accepts every serialization Bitcoin
// does (legacy, and BIP144 with witness) and nothing else: lengths must be
// minimally encoded, counts must fit the bytes that are there, no byte may be
// left over. It never throws and never reads outside its input.
//
// The id is computed here, from the bytes, never taken from the source.

#include "wallet/swap/htlc.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace dinero::swap {

// A transaction cannot be larger than a block.
inline constexpr size_t kBtcMaxRawTxSize = 4'000'000;

struct BtcRawTx {
    struct In {
        Bytes32 prev_txid{};  // wire byte order
        uint32_t prev_vout{};
        std::vector<uint8_t> script_sig;
        uint32_t sequence{};
        std::vector<std::vector<uint8_t>> witness;
    };
    struct Out {
        uint64_t value_sat{};
        std::vector<uint8_t> script_pubkey;
    };
    int32_t version{};
    std::vector<In> vin;
    std::vector<Out> vout;
    uint32_t locktime{};

    Bytes32 txid{};          // wire byte order: SHA256d of the serialization without witness
    size_t stripped_size{};  // length of that serialization
    bool has_witness{};

    // One input spending the null outpoint: only a block's first transaction is this.
    bool IsCoinbase() const {
        return vin.size() == 1 && vin[0].prev_txid == Bytes32{} && vin[0].prev_vout == 0xffffffff;
    }
};

std::optional<BtcRawTx> ParseBtcRawTx(const std::vector<uint8_t>& raw);

}  // namespace dinero::swap

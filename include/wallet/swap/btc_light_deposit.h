#pragma once
// Watches a deposit address without Bitcoin Core
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3.3).
//
// Which coins a buyer's deposit address holds, from servers that are not
// believed: a payment counts only if its transaction is proven by Merkle proof
// into the node's own header chain, and its depth is counted on that chain.
//
// "Spent" is taken from any server that can show the spending transaction (it
// must really spend that output). A server that wrongly says "unspent" only
// makes the node build a transaction the network then refuses; one that
// wrongly says "spent" cannot, since it has to produce the spender.
//
// No I/O of its own. The caller holds the header chain steady meanwhile
// (BtcLightBackend::Lock).

#include "wallet/swap/btc_deposit.h"
#include "wallet/swap/btc_esplora.h"
#include "wallet/swap/btc_header_chain.h"

#include <cstdint>
#include <map>
#include <vector>

namespace dinero::swap {

inline constexpr uint32_t kBtcDepositConfirmations = 2;  // a payment this deep may fund a lock

struct BtcDepositView {
    bool ok{false};                    // false: no server answered; nothing below is known
    std::vector<BtcDepositCoin> coins;       // unspent, at least 1 confirmation
    std::vector<BtcDepositCoin> deep_coins;  // unspent, at least kBtcDepositConfirmations
    uint64_t seen_sat{0};              // sum of `coins`
    uint64_t confirmed_sat{0};         // sum of `deep_coins`
    uint32_t newest_confirmations{0};  // of the newest payment (0: none)
    bool ever_paid{false};             // a proven payment to the script exists, spent or not (Observe only)
};

class BtcDepositWatcher {
public:
    // The chain and the servers must outlive the watcher.
    BtcDepositWatcher(const BtcHeaderChain& chain, std::vector<EsploraServer*> servers,
                      std::vector<uint8_t> script_pubkey);

    BtcDepositView Observe();

    // For sending coins BACK only (cancel, recovery): every unspent output a
    // server reports as confirmed, proven or not. The raw transaction is still
    // hashed here and must pay our script. A payment in a block below the
    // header chain's checkpoint cannot be proven, yet it is the user's money;
    // and a coin a server makes up costs nothing, since the sweep that tries
    // to spend it is refused by the network. Never use this to decide that a
    // deposit can fund a swap. `deep_coins` and `confirmed_sat` are left empty.
    BtcDepositView ObserveForRecovery();

private:
    struct Payment {
        Bytes32 block_hash{};
        std::vector<std::pair<uint32_t, uint64_t>> outputs;  // vout, value: the outputs paying our script
    };
    bool Spent(const Bytes32& txid, uint32_t vout);

    const BtcHeaderChain& chain_;
    std::vector<EsploraServer*> servers_;
    std::vector<uint8_t> script_pubkey_;
    std::map<Bytes32, Payment> proven_;  // payments already proven; re-checked against the chain only
};

}  // namespace dinero::swap

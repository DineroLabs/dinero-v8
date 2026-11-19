#include "wallet/swap/btc_light_deposit.h"

#include <algorithm>
#include <stdexcept>

namespace dinero::swap {

BtcDepositWatcher::BtcDepositWatcher(const BtcHeaderChain& chain, std::vector<EsploraServer*> servers,
                                     std::vector<uint8_t> script_pubkey)
    : chain_(chain), servers_(std::move(servers)), script_pubkey_(std::move(script_pubkey)) {
    if (servers_.empty()) throw std::invalid_argument("deposit watcher: no servers");
    if (script_pubkey_.empty()) throw std::invalid_argument("deposit watcher: no script");
}

// Whether some server can show a transaction that spends this output.
bool BtcDepositWatcher::Spent(const Bytes32& txid, uint32_t vout) {
    for (EsploraServer* reporter : servers_) {
        const auto out = reporter->Outspend(txid, vout);
        if (!out || !out->spent) continue;
        for (EsploraServer* server : servers_) {
            const auto raw = server->RawTx(out->txid);
            const auto tx = raw ? ParseBtcRawTx(*raw) : std::nullopt;
            if (!tx || tx->txid != out->txid) continue;
            for (const auto& in : tx->vin) {
                if (in.prev_txid == txid && in.prev_vout == vout) return true;
            }
        }
    }
    return false;
}

BtcDepositView BtcDepositWatcher::Observe() {
    BtcDepositView view;
    // Payments proven earlier stand while their block is on our chain.
    for (auto it = proven_.begin(); it != proven_.end();) {
        it = chain_.HeightOf(it->second.block_hash) ? std::next(it) : proven_.erase(it);
    }
    std::vector<Bytes32> candidates;
    for (EsploraServer* server : servers_) {
        const auto history = server->ScriptHistory(script_pubkey_);
        if (!history) continue;
        view.ok = true;
        for (const EsploraScriptTx& entry : *history) {
            if (!entry.status.confirmed || proven_.count(entry.txid)) continue;
            if (std::find(candidates.begin(), candidates.end(), entry.txid) == candidates.end()) candidates.push_back(entry.txid);
        }
    }
    if (!view.ok) return view;
    if (candidates.size() > 50) candidates.resize(50);  // an address used by one swap; the rest next time
    for (const Bytes32& txid : candidates) {
        for (EsploraServer* server : servers_) {
            std::string why;
            const auto proven = FetchProvenTx(*server, chain_, txid, why);
            if (!proven) continue;
            Payment payment;
            payment.block_hash = proven->block_hash;
            for (size_t i = 0; i < proven->tx.vout.size(); ++i) {
                if (proven->tx.vout[i].script_pubkey == script_pubkey_) {
                    payment.outputs.emplace_back(static_cast<uint32_t>(i), proven->tx.vout[i].value_sat);
                }
            }
            proven_[txid] = std::move(payment);  // also when it pays us nothing: it is not asked about again
            break;
        }
    }
    for (const auto& [txid, payment] : proven_) {
        const uint32_t confirmations = chain_.Confirmations(payment.block_hash);
        for (const auto& [vout, value] : payment.outputs) {
            if (Spent(txid, vout)) continue;
            BtcDepositCoin coin;
            coin.txid = txid;
            coin.vout = vout;
            coin.value_sat = value;
            view.coins.push_back(coin);
            view.seen_sat += value;
            if (confirmations >= kBtcDepositConfirmations) {
                view.deep_coins.push_back(coin);
                view.confirmed_sat += value;
            }
            if (view.newest_confirmations == 0 || confirmations < view.newest_confirmations) {
                view.newest_confirmations = confirmations;
            }
        }
    }
    return view;
}

}  // namespace dinero::swap

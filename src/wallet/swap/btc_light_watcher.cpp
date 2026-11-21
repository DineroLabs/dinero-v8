#include "wallet/swap/btc_light_watcher.h"

#include <algorithm>
#include <stdexcept>

namespace dinero::swap {

BtcLightWatcher::BtcLightWatcher(const BtcHeaderChain& chain, std::vector<EsploraServer*> servers,
                                 BtcWatchTarget target, BtcLightWatchConfig config)
    : chain_(chain), servers_(std::move(servers)), target_(std::move(target)), config_(config),
      script_pubkey_(BtcP2wshScriptPubKey(BuildBtcHtlcWitnessScript(target_.terms))) {
    if (servers_.empty()) throw std::invalid_argument("light Bitcoin watcher: no servers");
    for (const EsploraServer* s : servers_) {
        if (!s) throw std::invalid_argument("light Bitcoin watcher: null server");
    }
    // A header chain of another Bitcoin network would show free coins as a "lock".
    if (target_.expected_chain != chain_.params().name) {
        throw std::invalid_argument("light Bitcoin watcher: header chain is " + chain_.params().name + ", the swap is on " +
                                    target_.expected_chain);
    }
    if (!target_.expected_funding_txid.empty()) {
        expected_funding_txid_ = BtcHashFromHex(target_.expected_funding_txid);
        if (!expected_funding_txid_) throw std::invalid_argument("light Bitcoin watcher: bad funding txid");
    }
}

std::optional<std::vector<uint8_t>> BtcLightWatcher::RawFromAnyServer(const Bytes32& txid) {
    for (EsploraServer* server : servers_) {
        auto raw = server->RawTx(txid);
        if (!raw) continue;
        const auto tx = ParseBtcRawTx(*raw);
        if (tx && tx->txid == txid) return raw;  // the bytes are the transaction asked for, whoever sent them
    }
    return std::nullopt;
}

// Looks for the lock: a proven transaction paying the HTLC script the swap's
// amount. Returns whether one is known after the round.
bool BtcLightWatcher::FindFunding(size_t& answered) {
    std::vector<Bytes32> candidates;
    for (EsploraServer* server : servers_) {
        const auto history = server->ScriptHistory(script_pubkey_);
        if (!history) continue;  // unreachable or malformed: this server did not answer
        ++answered;
        for (const EsploraScriptTx& entry : *history) {
            if (!entry.status.confirmed) continue;  // an unconfirmed lock is no lock yet
            if (expected_funding_txid_ && entry.txid != *expected_funding_txid_) continue;
            if (not_the_lock_.count(entry.txid)) continue;
            if (std::find(candidates.begin(), candidates.end(), entry.txid) == candidates.end()) candidates.push_back(entry.txid);
        }
    }
    if (candidates.size() > config_.max_candidates) candidates.resize(config_.max_candidates);

    std::optional<Proven> best;
    uint32_t best_height = 0;
    for (const Bytes32& txid : candidates) {
        std::optional<BtcProvenTx> proven;
        for (EsploraServer* server : servers_) {
            std::string why;
            proven = FetchProvenTx(*server, chain_, txid, why);
            if (proven) break;
        }
        if (!proven) continue;  // nobody could prove it (yet): not remembered, tried again next round
        // Anyone can pay this script: only the exact swap output is the lock.
        std::optional<uint32_t> vout;
        for (size_t i = 0; i < proven->tx.vout.size(); ++i) {
            const auto& out = proven->tx.vout[i];
            if (out.script_pubkey != script_pubkey_) continue;
            if (target_.expected_amount_sat && out.value_sat != target_.expected_amount_sat) continue;
            vout = static_cast<uint32_t>(i);
            break;
        }
        if (!vout) {
            not_the_lock_.insert(txid);  // proven, and proven not to be ours: never asked about again
            continue;
        }
        if (proven->height < target_.scan_from_height) continue;  // older than the swap: not its lock
        // The earliest confirmed one is the lock, as a block scan from the start would find it.
        if (!best || proven->height < best_height) {
            best = Proven{txid, proven->block_hash, *vout, proven->tx.vout[*vout].value_sat};
            best_height = proven->height;
        }
    }
    if (best) funding_ = best;
    return funding_.has_value();
}

// Looks for a spend of the lock. Fills the spend fields of `htlc`.
void BtcLightWatcher::FindSpend(HtlcObservation& htlc, size_t& answered) {
    // A confirmed spend proven earlier stands while its block is on our chain.
    if (confirmed_spend_ && !chain_.HeightOf(confirmed_spend_->block_hash)) {
        confirmed_spend_.reset();
        confirmed_spend_preimage_.reset();
    }
    if (!confirmed_spend_) {
        std::vector<Bytes32> reported;
        for (EsploraServer* server : servers_) {
            const auto out = server->Outspend(funding_->txid, funding_->vout);
            if (!out) continue;
            ++answered;
            if (out->spent && std::find(reported.begin(), reported.end(), out->txid) == reported.end()) {
                reported.push_back(out->txid);
            }
        }
        for (const Bytes32& txid : reported) {
            const auto raw = RawFromAnyServer(txid);
            const auto tx = raw ? ParseBtcRawTx(*raw) : std::nullopt;
            if (!tx) continue;
            // It must really spend the lock's output.
            const BtcRawTx::In* input = nullptr;
            for (const auto& in : tx->vin) {
                if (in.prev_txid == funding_->txid && in.prev_vout == funding_->vout) input = &in;
            }
            if (!input) continue;
            const auto preimage = ExtractPreimageFromBtcClaim(input->witness, target_.terms.payment_hash);

            // Confirmed? Then it must prove into our chain.
            std::optional<BtcProvenTx> proven;
            for (EsploraServer* server : servers_) {
                std::string why;
                proven = FetchProvenTx(*server, chain_, txid, why);
                if (proven) break;
            }
            if (proven) {
                confirmed_spend_ = Proven{txid, proven->block_hash, 0, 0};
                confirmed_spend_preimage_ = preimage;
                break;
            }
            // Unconfirmed (or not provable yet): it counts only if it carries the
            // secret, which nobody can make up.
            if (preimage) {
                htlc.spent = true;
                htlc.spent_by_claim = true;
                htlc.revealed_preimage = preimage;
                htlc.spend_confirmations = 0;
            }
        }
    }
    if (confirmed_spend_) {
        htlc.spent = true;
        htlc.spent_by_claim = confirmed_spend_preimage_.has_value();
        htlc.revealed_preimage = confirmed_spend_preimage_;
        htlc.spend_confirmations = chain_.Confirmations(confirmed_spend_->block_hash);
    }
}

BtcWatchReport BtcLightWatcher::Observe(uint32_t now_unix) {
    BtcWatchReport report;
    problem_.clear();

    // Our own view of Bitcoin must be current, or nothing below means anything.
    const uint32_t tip_time = chain_.Tip().time;
    if (now_unix > tip_time && now_unix - tip_time > config_.max_tip_age_seconds) {
        problem_ = "Bitcoin headers are behind (newest is " + std::to_string((now_unix - tip_time) / 60) + " minutes old)";
        return report;
    }
    const auto mtp = chain_.MedianTimePast();
    if (!mtp) {
        problem_ = "Bitcoin headers are still syncing";
        return report;
    }
    report.mtp_unix = *mtp;

    // A lock proven earlier stands while its block is on our chain (a reorg drops it).
    if (funding_ && !chain_.HeightOf(funding_->block_hash)) {
        funding_.reset();
        confirmed_spend_.reset();
        confirmed_spend_preimage_.reset();
    }
    size_t answered = 0;
    if (!funding_) {
        FindFunding(answered);
    }
    if (funding_) {
        BtcFunding f;
        f.txid = funding_->txid;
        f.vout = funding_->vout;
        f.value_sat = funding_->value_sat;
        report.funding = f;
        report.htlc.output_seen = true;
        report.htlc.output_value = funding_->value_sat;
        report.htlc.output_confirmations = chain_.Confirmations(funding_->block_hash);
        size_t spend_answered = 0;
        FindSpend(report.htlc, spend_answered);
        // The round's count is of servers that answered the question that matters now.
        if (!confirmed_spend_) answered = spend_answered;
        else answered = std::max(answered, config_.min_servers);  // a proven spend needs nobody's word
    }
    if (answered < config_.min_servers) {
        problem_ = answered == 0 ? "cannot reach any Bitcoin server"
                                 : "only " + std::to_string(answered) + " of the " + std::to_string(config_.min_servers) +
                                       " Bitcoin servers needed answered";
        return BtcWatchReport{};  // not ok: the engine does nothing on a partial view
    }
    report.ok = true;
    return report;
}

}  // namespace dinero::swap

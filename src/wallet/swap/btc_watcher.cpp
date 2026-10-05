#include "wallet/swap/btc_watcher.h"

#include <algorithm>
#include <cmath>

namespace dinero::swap {
namespace {

std::string ToHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

std::optional<std::vector<uint8_t>> FromHex(const std::string& h) {
    if (h.size() % 2) return std::nullopt;
    std::vector<uint8_t> out;
    for (size_t i = 0; i < h.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(h[i]), lo = nib(h[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return out;
}

// Bitcoin Core prints txids reversed relative to wire order.
std::string DisplayTxid(const std::array<uint8_t, 32>& wire) {
    std::array<uint8_t, 32> rev{};
    std::reverse_copy(wire.begin(), wire.end(), rev.begin());
    return ToHex(rev.data(), rev.size());
}

std::optional<std::array<uint8_t, 32>> WireTxid(const std::string& display) {
    const auto bytes = FromHex(display);
    if (!bytes || bytes->size() != 32) return std::nullopt;
    std::array<uint8_t, 32> wire{};
    std::reverse_copy(bytes->begin(), bytes->end(), wire.begin());
    return wire;
}

// Fill the spend fields of `obs` from a spending input's witness (hex items).
bool ClassifySpend(const Json::Value& txinwitness, const Bytes32& payment_hash, HtlcObservation& obs) {
    std::vector<std::vector<uint8_t>> witness;
    for (const auto& item : txinwitness) {
        const auto bytes = FromHex(item.asString());
        if (!bytes) return false;
        witness.push_back(*bytes);
    }
    obs.spent = true;
    obs.revealed_preimage = ExtractPreimageFromBtcClaim(witness, payment_hash);
    obs.spent_by_claim = obs.revealed_preimage.has_value();  // a claim always carries the secret
    return true;
}

}  // namespace

BtcWatcher::BtcWatcher(BtcRpc rpc, BtcWatchTarget target)
    : rpc_(std::move(rpc)), target_(std::move(target)),
      script_pubkey_(BtcP2wshScriptPubKey(BuildBtcHtlcWitnessScript(target_.terms))),
      next_height_(target_.scan_from_height) {}

BtcWatchReport BtcWatcher::Observe() {
    BtcWatchReport report;
    const auto info = rpc_("getblockchaininfo", Json::Value(Json::arrayValue));
    if (!info || !(*info)["blocks"].isNumeric()) return report;
    // A node on another Bitcoin network would show free coins as a "lock".
    if (!target_.expected_chain.empty() && (*info)["chain"].asString() != target_.expected_chain) return report;
    const uint32_t tip = (*info)["blocks"].asUInt();
    report.mtp_unix = (*info)["mediantime"].asUInt();

    auto hash_at = [&](uint32_t height) -> std::optional<std::string> {
        Json::Value p(Json::arrayValue);
        p.append(height);
        const auto h = rpc_("getblockhash", p);
        if (!h || !h->isString()) return std::nullopt;
        return h->asString();
    };

    // Reorg anywhere in the scanned range (not only the funding/spend blocks):
    // a replaced block may now hold the funding or the claim. Rewind to the
    // fork point (deeper than the remembered window: to the start). A failed
    // RPC is "not observed", never a reorg.
    if (next_height_ > target_.scan_from_height) {
        const uint32_t last = next_height_ - 1;
        std::optional<std::string> at_last;
        if (last <= tip && !(at_last = hash_at(last))) return report;
        if (last > tip || recent_.empty() || *at_last != recent_.rbegin()->second) {
            uint32_t resume = target_.scan_from_height;
            for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
                if (it->first > tip) continue;
                const auto h = hash_at(it->first);
                if (!h) return report;
                if (*h == it->second) {
                    resume = it->first + 1;
                    break;
                }
            }
            recent_.erase(recent_.lower_bound(resume), recent_.end());
            next_height_ = resume;
        }
    }
    // Reorg checks: forget anything whose block left the main chain.
    auto left_chain = [&](uint32_t height, const std::string& block_hash) -> std::optional<bool> {
        if (height > tip) return true;
        const auto h = hash_at(height);
        if (!h) return std::nullopt;
        return *h != block_hash;
    };
    if (confirmed_spend_) {
        const auto gone = left_chain(spend_height_, spend_block_hash_);
        if (!gone) return report;
        if (*gone) {
            confirmed_spend_.reset();
            next_height_ = std::min(next_height_, spend_height_);
        }
    }
    if (funding_) {
        const auto gone = left_chain(funding_height_, funding_block_hash_);
        if (!gone) return report;
        if (*gone) {
            funding_.reset();
            confirmed_spend_.reset();
            next_height_ = std::min(next_height_, funding_height_);
        }
    }

    const std::string spk_hex = ToHex(script_pubkey_.data(), script_pubkey_.size());
    for (uint32_t height = next_height_; height <= tip; ++height) {
        const auto hash = hash_at(height);
        if (!hash) return report;
        Json::Value p(Json::arrayValue);
        p.append(*hash);
        p.append(2);
        const auto block = rpc_("getblock", p);
        if (!block) return report;
        for (const auto& tx : (*block)["tx"]) {
            if (!funding_) {
                for (const auto& out : tx["vout"]) {
                    if (out["scriptPubKey"]["hex"].asString() != spk_hex) continue;
                    // Anyone can pay this script: only the exact swap output (and,
                    // once known, Bob's own funding transaction) is the lock.
                    const uint64_t sat = static_cast<uint64_t>(std::llround(out["value"].asDouble() * 1e8));
                    if (target_.expected_amount_sat && sat != target_.expected_amount_sat) continue;
                    if (!target_.expected_funding_txid.empty() && tx["txid"].asString() != target_.expected_funding_txid) {
                        continue;
                    }
                    const auto wire = WireTxid(tx["txid"].asString());
                    if (!wire) return report;
                    BtcFunding f;
                    f.txid = *wire;
                    f.vout = out["n"].asUInt();
                    f.value_sat = static_cast<uint64_t>(std::llround(out["value"].asDouble() * 1e8));
                    funding_ = f;
                    funding_height_ = height;
                    funding_block_hash_ = *hash;
                    break;
                }
            }
            if (funding_ && !confirmed_spend_) {
                const std::string funding_txid = DisplayTxid(funding_->txid);
                for (const auto& in : tx["vin"]) {
                    if (in["txid"].asString() != funding_txid || in["vout"].asUInt() != funding_->vout) continue;
                    HtlcObservation spend;
                    if (!ClassifySpend(in["txinwitness"], target_.terms.payment_hash, spend)) return report;
                    confirmed_spend_ = spend;
                    spend_height_ = height;
                    spend_block_hash_ = *hash;
                }
            }
        }
        next_height_ = height + 1;
        recent_[height] = *hash;
        while (recent_.size() > 288) recent_.erase(recent_.begin());  // the fork-point window
    }

    if (funding_) {
        report.funding = funding_;
        report.htlc.output_seen = true;
        report.htlc.output_value = funding_->value_sat;
        report.htlc.output_confirmations = tip - funding_height_ + 1;
        if (confirmed_spend_) {
            report.htlc.spent = true;
            report.htlc.spent_by_claim = confirmed_spend_->spent_by_claim;
            report.htlc.revealed_preimage = confirmed_spend_->revealed_preimage;
            report.htlc.spend_confirmations = tip - spend_height_ + 1;
        } else {
            // Unconfirmed spend: the secret is visible as soon as Alice's claim is in the mempool.
            Json::Value outpoint(Json::objectValue);
            outpoint["txid"] = DisplayTxid(funding_->txid);
            outpoint["vout"] = funding_->vout;
            Json::Value list(Json::arrayValue);
            list.append(outpoint);
            Json::Value p(Json::arrayValue);
            p.append(list);
            const auto spending = rpc_("gettxspendingprevout", p);
            if (!spending || !spending->isArray() || spending->empty()) return report;
            const auto& entry = (*spending)[0];
            if (entry.isMember("spendingtxid")) {
                Json::Value q(Json::arrayValue);
                q.append(entry["spendingtxid"].asString());
                q.append(true);
                const auto tx = rpc_("getrawtransaction", q);
                if (!tx) return report;
                for (const auto& in : (*tx)["vin"]) {
                    if (in["txid"].asString() == outpoint["txid"].asString() &&
                        in["vout"].asUInt() == funding_->vout) {
                        if (!ClassifySpend(in["txinwitness"], target_.terms.payment_hash, report.htlc)) return report;
                        report.htlc.spend_confirmations = 0;
                    }
                }
            }
        }
    }
    report.ok = true;
    return report;
}

}  // namespace dinero::swap

#include "wallet/swap/zec_watcher.h"

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

// Zebra prints txids reversed relative to wire order, as Bitcoin Core does.
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

std::optional<uint32_t> BranchId(const Json::Value& hex) {
    if (!hex.isString() || hex.asString().size() != 8) return std::nullopt;
    const auto bytes = FromHex(hex.asString());
    if (!bytes) return std::nullopt;
    return uint32_t((*bytes)[0]) << 24 | uint32_t((*bytes)[1]) << 16 | uint32_t((*bytes)[2]) << 8 | (*bytes)[3];
}

Json::Value Params(std::initializer_list<Json::Value> items) {
    Json::Value p(Json::arrayValue);
    for (const auto& i : items) p.append(i);
    return p;
}

// The spend fields of `obs` from a spending input's scriptSig.
bool ClassifySpend(const Json::Value& script_sig_hex, const Bytes32& payment_hash, HtlcObservation& obs) {
    const auto bytes = FromHex(script_sig_hex.asString());
    if (!script_sig_hex.isString() || !bytes) return false;
    obs.spent = true;
    obs.revealed_preimage = ExtractPreimageFromZecClaim(*bytes, payment_hash);
    obs.spent_by_claim = obs.revealed_preimage.has_value();  // a claim always carries the secret
    return true;
}

// The input of `tx` (verbose JSON) that spends `funding`, if any.
const Json::Value* SpendingInput(const Json::Value& tx, const ZecFunding& funding) {
    const std::string txid = DisplayTxid(funding.txid);
    for (const auto& in : tx["vin"]) {
        if (in.isMember("txid") && in["txid"].asString() == txid && in["vout"].asUInt() == funding.vout) return &in;
    }
    return nullptr;
}

}  // namespace

std::optional<ZecUpgrade> ZecUpgradeBefore(const ZecChainInfo& chain, uint32_t now_unix, uint32_t until_unix) {
    const double seconds = until_unix > now_unix ? double(until_unix - now_unix) : 0.0;
    const uint64_t projected = uint64_t(chain.tip_height) + uint64_t(std::ceil(seconds / kZecProjectedSecondsPerBlock));
    std::optional<ZecUpgrade> first;
    for (const auto& u : chain.upgrades) {
        if (u.activation_height <= chain.tip_height || u.activation_height > projected) continue;
        if (!first || u.activation_height < first->activation_height) first = u;
    }
    return first;
}

std::optional<ZecChainInfo> ObserveZecChain(const ZecRpc& rpc, const std::string& expected_chain,
                                            std::map<std::string, uint32_t>* time_cache) {
    const auto info = rpc("getblockchaininfo", Json::Value(Json::arrayValue));
    if (!info || !(*info)["blocks"].isNumeric()) return std::nullopt;
    // A node on another Zcash network would show free coins as a "lock".
    if (!expected_chain.empty() && (*info)["chain"].asString() != expected_chain) return std::nullopt;
    ZecChainInfo chain;
    chain.tip_height = (*info)["blocks"].asUInt();
    const auto next = BranchId((*info)["consensus"]["nextblock"]);
    if (!next) return std::nullopt;
    chain.next_block_branch_id = *next;
    const auto& upgrades = (*info)["upgrades"];
    for (const auto& key : upgrades.getMemberNames()) {
        const auto branch = BranchId(Json::Value(key));
        const auto& u = upgrades[key];
        if (!branch || !u["activationheight"].isNumeric()) return std::nullopt;
        chain.upgrades.push_back({u["name"].asString(), u["activationheight"].asUInt(), *branch});
    }

    // Median time past: the median of the last 11 block times (fewer near genesis).
    std::map<std::string, uint32_t> local;
    auto& cache = time_cache ? *time_cache : local;
    std::vector<uint32_t> times;
    for (uint32_t h = chain.tip_height, n = 0; n < 11; ++n, --h) {
        const auto hash = rpc("getblockhash", Params({h}));
        if (!hash || !hash->isString()) return std::nullopt;
        auto it = cache.find(hash->asString());
        if (it == cache.end()) {
            const auto header = rpc("getblockheader", Params({*hash, true}));
            if (!header || !(*header)["time"].isNumeric()) return std::nullopt;
            it = cache.emplace(hash->asString(), (*header)["time"].asUInt()).first;
        }
        if (n == 0) chain.tip_time_unix = it->second;
        times.push_back(it->second);
        if (h == 0) break;
    }
    std::sort(times.begin(), times.end());
    chain.mtp_unix = times[times.size() / 2];
    if (cache.size() > 4096) cache.clear();  // a cache only
    return chain;
}

ZecWatcher::ZecWatcher(ZecRpc rpc, ZecWatchTarget target)
    : rpc_(std::move(rpc)), target_(std::move(target)), next_height_(target_.scan_from_height) {
    const auto spk = ZecP2shScriptPubKey(BuildZecHtlcRedeemScript(target_.terms));
    spk_hex_ = ToHex(spk.data(), spk.size());
}

std::optional<std::string> ZecWatcher::HashAt(uint32_t height) {
    const auto h = rpc_("getblockhash", Params({height}));
    if (!h || !h->isString()) return std::nullopt;
    return h->asString();
}

bool ZecWatcher::FindMempoolSpend(HtlcObservation& htlc) {
    const auto pool = rpc_("getrawmempool", Json::Value(Json::arrayValue));
    if (!pool || !pool->isArray()) return false;
    std::set<std::string> now;
    for (const auto& id : *pool) {
        const std::string txid = id.asString();
        now.insert(txid);
        if (mempool_seen_.count(txid)) continue;
        const auto tx = rpc_("getrawtransaction", Params({txid, 1}));
        if (!tx) {
            // Mined or evicted since getrawmempool: the next tick sees it in a block or not at all.
            continue;
        }
        mempool_seen_.insert(txid);
        if (const auto* in = SpendingInput(*tx, *funding_)) {
            if (!ClassifySpend((*in)["scriptSig"]["hex"], target_.terms.payment_hash, htlc)) return false;
            htlc.spend_confirmations = 0;
            mempool_seen_.erase(txid);  // re-read every tick while it stays unconfirmed
            break;
        }
    }
    std::set<std::string> kept;
    std::set_intersection(mempool_seen_.begin(), mempool_seen_.end(), now.begin(), now.end(),
                          std::inserter(kept, kept.begin()));
    mempool_seen_.swap(kept);
    return true;
}

ZecWatchReport ZecWatcher::Observe() {
    ZecWatchReport report;
    auto chain = ObserveZecChain(rpc_, target_.expected_chain, &block_time_);
    if (!chain) return report;
    report.chain = *chain;
    const uint32_t tip = report.chain.tip_height;

    // Reorg anywhere in the scanned range: rewind to the fork point. A failed
    // RPC is "not observed", never a reorg.
    if (next_height_ > target_.scan_from_height) {
        const uint32_t last = next_height_ - 1;
        std::optional<std::string> at_last;
        if (last <= tip && !(at_last = HashAt(last))) return report;
        if (last > tip || recent_.empty() || *at_last != recent_.rbegin()->second) {
            uint32_t resume = target_.scan_from_height;
            for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
                if (it->first > tip) continue;
                const auto h = HashAt(it->first);
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
    auto left_chain = [&](uint32_t height, const std::string& block_hash) -> std::optional<bool> {
        if (height > tip) return true;
        const auto h = HashAt(height);
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

    for (uint32_t height = next_height_; height <= tip; ++height) {
        const auto hash = HashAt(height);
        if (!hash) return report;
        const auto block = rpc_("getblock", Params({*hash, 2}));
        if (!block || !(*block)["tx"].isArray()) return report;
        for (const auto& tx : (*block)["tx"]) {
            if (!funding_) {
                for (const auto& out : tx["vout"]) {
                    if (out["scriptPubKey"]["hex"].asString() != spk_hex_) continue;
                    // Anyone can pay this script: only the exact swap output (and,
                    // once known, Bob's own funding transaction) is the lock.
                    if (!out["valueZat"].isIntegral() || out["valueZat"].asInt64() < 0) return report;
                    const uint64_t zat = out["valueZat"].asUInt64();
                    if (target_.expected_amount_zat && zat != target_.expected_amount_zat) continue;
                    if (!target_.expected_funding_txid.empty() && tx["txid"].asString() != target_.expected_funding_txid) {
                        continue;
                    }
                    const auto wire = WireTxid(tx["txid"].asString());
                    if (!wire) return report;
                    funding_ = ZecFunding{*wire, out["n"].asUInt(), zat};
                    funding_height_ = height;
                    funding_block_hash_ = *hash;
                    break;
                }
            }
            if (funding_ && !confirmed_spend_) {
                if (const auto* in = SpendingInput(tx, *funding_)) {
                    HtlcObservation spend;
                    if (!ClassifySpend((*in)["scriptSig"]["hex"], target_.terms.payment_hash, spend)) return report;
                    confirmed_spend_ = spend;
                    spend_height_ = height;
                    spend_block_hash_ = *hash;
                }
            }
        }
        next_height_ = height + 1;
        recent_[height] = *hash;
        while (recent_.size() > 1152) recent_.erase(recent_.begin());  // the fork-point window, ~1 day
    }

    if (funding_) {
        report.funding = funding_;
        report.htlc.output_seen = true;
        report.htlc.output_value = funding_->value_zat;
        report.htlc.output_confirmations = tip - funding_height_ + 1;
        if (confirmed_spend_) {
            report.htlc.spent = true;
            report.htlc.spent_by_claim = confirmed_spend_->spent_by_claim;
            report.htlc.revealed_preimage = confirmed_spend_->revealed_preimage;
            report.htlc.spend_confirmations = tip - spend_height_ + 1;
        } else if (!FindMempoolSpend(report.htlc)) {
            return report;
        }
    }
    report.ok = true;
    return report;
}

uint64_t ZecDepositReport::confirmed_zat(uint32_t min_confirmations) const {
    uint64_t total = 0;
    for (const auto& c : coins) {
        if (c.confirmations >= min_confirmations) total += c.coin.value_zat;
    }
    return total;
}

std::vector<ZecCoin> ZecDepositReport::spendable(uint32_t min_confirmations) const {
    std::vector<ZecCoin> out;
    for (const auto& c : coins) {
        if (c.confirmations >= min_confirmations) out.push_back(c.coin);
    }
    return out;
}

ZecDepositReport ObserveZecDeposit(const ZecRpc& rpc, const std::string& address, const std::string& expected_chain) {
    ZecDepositReport report;
    const auto info = rpc("getblockchaininfo", Json::Value(Json::arrayValue));
    if (!info || !(*info)["blocks"].isNumeric()) return report;
    if (!expected_chain.empty() && (*info)["chain"].asString() != expected_chain) return report;

    Json::Value request(Json::objectValue);
    request["addresses"].append(address);
    request["chainInfo"] = true;  // the height the UTXO set was read at
    const auto res = rpc("getaddressutxos", Params({request}));
    if (!res || !(*res)["utxos"].isArray() || !(*res)["height"].isNumeric()) return report;
    report.tip_height = (*res)["height"].asUInt();
    for (const auto& u : (*res)["utxos"]) {
        const auto wire = WireTxid(u["txid"].asString());
        if (!wire || !u["satoshis"].isIntegral() || !u["height"].isNumeric() || u["address"].asString() != address) {
            return report;
        }
        const uint32_t height = u["height"].asUInt();
        if (height > report.tip_height) return report;
        report.coins.push_back({{*wire, u["outputIndex"].asUInt(), u["satoshis"].asUInt64()},
                                report.tip_height - height + 1});
    }
    report.ok = true;
    return report;
}

}  // namespace dinero::swap

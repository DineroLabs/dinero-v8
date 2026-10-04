#include "wallet/swap/din_watcher.h"

#include "bech32/bech32.hpp"
#include "primitives/hash_domains.h"
#include "primitives/transaction.h"

#include <algorithm>
#include <vector>

namespace dinero::swap {
namespace {

constexpr size_t kBlockHeaderSize = 128;

std::optional<std::vector<uint8_t>> FromHex(const std::string& h) {
    if (h.size() % 2) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(h.size() / 2);
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

uint64_t ReadCompact(const std::vector<uint8_t>& b, size_t& pos, bool& ok) {
    auto le = [&](int n) {
        uint64_t v = 0;
        if (pos + n > b.size()) { ok = false; return v; }
        for (int i = 0; i < n; ++i) v |= uint64_t(b[pos + i]) << (8 * i);
        pos += n;
        return v;
    };
    const uint64_t first = le(1);
    if (!ok || first < 0xfd) return first;
    return le(first == 0xfd ? 2 : first == 0xfe ? 4 : 8);
}

// Transactions of a raw block: 128-byte header, compact count, transactions,
// then the optional Utreexo section.
std::optional<std::vector<Transaction>> ParseBlockTransactions(const std::vector<uint8_t>& block) {
    if (block.size() < kBlockHeaderSize + 1) return std::nullopt;
    size_t pos = kBlockHeaderSize;
    bool ok = true;
    const uint64_t count = ReadCompact(block, pos, ok);
    if (!ok || count == 0 || count > 100'000) return std::nullopt;
    std::vector<Transaction> txs;
    for (uint64_t i = 0; i < count; ++i) {
        Transaction tx;
        size_t consumed = 0;
        const std::vector<uint8_t> rest(block.begin() + pos, block.end());
        if (!TransactionSerializer::Deserialize(tx, rest, consumed) || consumed == 0) return std::nullopt;
        pos += consumed;
        txs.push_back(std::move(tx));
    }
    // Trailing Utreexo section (Block::Serialize): 0x00 = none, 0x01 = data follows.
    if (pos == block.size()) return txs;
    if (block[pos] == 0x00 && pos + 1 == block.size()) return txs;
    if (block[pos] == 0x01) return txs;
    return std::nullopt;
}

}  // namespace

std::string DinHtlcAddress(const DinHtlcOutput& htlc, const std::string& hrp) {
    return bech32::Encode(hrp, 1, std::vector<uint8_t>(htlc.output_key.begin(), htlc.output_key.end()),
                          bech32::Encoding::BECH32M);
}

DinWatcher::DinWatcher(DinRpc rpc, DinHtlcTerms terms, std::string hrp)
    : rpc_(std::move(rpc)), terms_(terms), htlc_(BuildDinHtlc(terms)),
      address_(DinHtlcAddress(htlc_, hrp)) {}

DinWatchReport DinWatcher::Observe() {
    DinWatchReport report;
    const auto info = rpc_("getblockchaininfo", Json::Value(Json::arrayValue));
    if (!info || !(*info)["blocks"].isNumeric()) return report;
    const uint32_t tip = (*info)["blocks"].asUInt();
    report.mtp_unix = (*info)["mediantime"].asUInt();

    Json::Value p(Json::arrayValue);
    p.append(address_);
    const auto history = rpc_("getaddresshistory", p);
    if (!history || !(*history)["transactions"].isArray()) return report;

    // Confirmed entries, oldest first.
    struct Entry { std::string txid; uint32_t height; };
    std::vector<Entry> entries;
    for (const auto& t : (*history)["transactions"]) {
        if (!t["height"].isNumeric() || t["height"].asInt() <= 0) continue;
        entries.push_back({t["txid"].asString(), t["height"].asUInt()});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.height < b.height; });

    auto block_txs = [&](uint32_t height) -> std::optional<std::vector<Transaction>> {
        Json::Value hp(Json::arrayValue);
        hp.append(height);
        const auto hash = rpc_("getblockhash", hp);
        if (!hash || !hash->isString()) return std::nullopt;
        auto it = block_cache_.find(hash->asString());
        if (it == block_cache_.end()) {
            Json::Value bp(Json::arrayValue);
            bp.append(hash->asString());
            bp.append(0);
            const auto raw = rpc_("getblock", bp);
            if (!raw || !raw->isString()) return std::nullopt;
            const auto bytes = FromHex(raw->asString());
            if (!bytes) return std::nullopt;
            it = block_cache_.emplace(hash->asString(), *bytes).first;
        }
        return ParseBlockTransactions(it->second);
    };

    auto hash_at = [&](uint32_t height) -> std::optional<std::string> {
        Json::Value hp(Json::arrayValue);
        hp.append(height);
        const auto hash = rpc_("getblockhash", hp);
        if (!hash || !hash->isString()) return std::nullopt;
        return hash->asString();
    };
    auto classify = [&](const TxInput& in, uint32_t height, const std::string& block_hash) {
        const auto& w = in.witness;
        FoundSpend f;
        f.height = height;
        f.block_hash = block_hash;
        f.by_claim = w.size() == 4 && w[2] == htlc_.claim_script;
        if (f.by_claim && w[1].size() == 32) {
            Bytes32 s{};
            std::copy(w[1].begin(), w[1].end(), s.begin());
            f.preimage = s;
        }
        return f;
    };

    uint32_t funding_height = 0;
    for (const auto& e : entries) {
        const auto txs = block_txs(e.height);
        if (!txs) return report;
        const auto it = std::find_if(txs->begin(), txs->end(), [&](const Transaction& tx) {
            return TxId::Compute(tx).AsUint256().GetHex() == e.txid;
        });
        if (it == txs->end()) return report;  // history and block disagree: do not act
        const Transaction& tx = *it;

        if (!report.funding) {
            for (uint32_t n = 0; n < tx.vout.size(); ++n) {
                if (tx.vout[n].scriptPubKey != htlc_.script_pubkey) continue;
                FundingOutput f;
                f.txid = TxId::Compute(tx);
                f.vout = n;
                f.value = tx.vout[n].value;
                f.script_pubkey = htlc_.script_pubkey;
                report.funding = f;
                funding_height = e.height;
                break;
            }
        }
    }

    // A cached spend survives only while its block is still on the main chain.
    if (spend_ && (!report.funding || spend_->height > tip || hash_at(spend_->height) != spend_->block_hash)) {
        spend_.reset();
    }
    if (report.funding && !spend_) {
        Json::Value op(Json::arrayValue);
        op.append(report.funding->txid.AsUint256().GetHex());
        op.append(report.funding->vout);
        const auto utxo = rpc_("gettxout", op);
        if (!utxo) return report;
        if (utxo->isNull()) {  // spent: find the spending transaction in a block
            for (uint32_t h = funding_height; h <= tip && !spend_; ++h) {
                const auto hash = hash_at(h);
                const auto txs = block_txs(h);
                if (!hash || !txs) return report;
                for (const auto& tx : *txs) {
                    for (const auto& in : tx.vin) {
                        if (in.prevout.txid == report.funding->txid && in.prevout.vout == report.funding->vout) {
                            spend_ = classify(in, h, *hash);
                        }
                    }
                }
            }
        }
    }
    if (spend_) {
        report.htlc.spent = true;
        report.htlc.spent_by_claim = spend_->by_claim;
        report.htlc.revealed_preimage = spend_->preimage;
        report.htlc.spend_confirmations = tip - spend_->height + 1;
    }

    if (report.funding) {
        report.htlc.output_seen = true;
        report.htlc.output_value = report.funding->value.GetUna();
        report.htlc.output_confirmations = tip - funding_height + 1;
    }
    report.ok = true;
    return report;
}

}  // namespace dinero::swap

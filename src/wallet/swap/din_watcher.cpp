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

DinWatcher::DinWatcher(DinRpc rpc, DinWatchTarget target)
    : rpc_(std::move(rpc)), target_(std::move(target)), htlc_(BuildDinHtlc(target_.terms)) {
    // Never the genesis block: it cannot hold an HTLC, and regtest's canonical
    // genesis is in a format the swap block parser does not read.
    target_.scan_from_height = std::max<uint32_t>(target_.scan_from_height, 1);
    next_height_ = target_.scan_from_height;
}

DinWatcher::DinWatcher(DinRpc rpc, DinHtlcTerms terms, std::string /*hrp*/)
    : DinWatcher(std::move(rpc), DinWatchTarget{terms, 0, 0, ""}) {}

// Blocks are scanned once each, from the swap's start height: no address
// index is needed (an AssumeUTXO node has none, and getaddresshistory only
// returns the newest entries, which spam could fill). A reorg anywhere in the
// scanned range rescans from the start.
DinWatchReport DinWatcher::Observe() {
    DinWatchReport report;
    const auto info = rpc_("getblockchaininfo", Json::Value(Json::arrayValue));
    if (!info || !(*info)["blocks"].isNumeric()) return report;
    const uint32_t tip = (*info)["blocks"].asUInt();
    report.mtp_unix = (*info)["mediantime"].asUInt();

    auto hash_at = [&](uint32_t height) -> std::optional<std::string> {
        Json::Value hp(Json::arrayValue);
        hp.append(height);
        const auto hash = rpc_("getblockhash", hp);
        if (!hash || !hash->isString()) return std::nullopt;
        return hash->asString();
    };
    auto block_txs = [&](const std::string& hash) -> std::optional<std::vector<Transaction>> {
        Json::Value bp(Json::arrayValue);
        bp.append(hash);
        bp.append(0);
        const auto raw = rpc_("getblock", bp);
        if (!raw || !raw->isString()) return std::nullopt;
        const auto bytes = FromHex(raw->asString());
        if (!bytes) return std::nullopt;
        return ParseBlockTransactions(*bytes);
    };

    if (next_height_ > target_.scan_from_height) {
        const uint32_t last = next_height_ - 1;
        if (last > tip || hash_at(last) != scanned_hash_) {  // reorg: start over
            funding_.reset();
            spend_.reset();
            next_height_ = target_.scan_from_height;
        }
    }

    for (uint32_t h = next_height_; h <= tip; ++h) {
        const auto hash = hash_at(h);
        if (!hash) return report;
        const auto txs = block_txs(*hash);
        if (!txs) return report;
        for (const auto& tx : *txs) {
            if (!funding_) {
                const TxId txid = TxId::Compute(tx);
                if (target_.expected_funding_txid.empty() ||
                    txid.AsUint256().GetHex() == target_.expected_funding_txid) {
                    for (uint32_t n = 0; n < tx.vout.size(); ++n) {
                        const auto& out = tx.vout[n];
                        if (out.scriptPubKey != htlc_.script_pubkey) continue;
                        // Anyone can pay this script: only the exact swap output is the lock.
                        if (target_.expected_amount_una && out.value.GetUna() != target_.expected_amount_una) continue;
                        FundingOutput f;
                        f.txid = txid;
                        f.vout = n;
                        f.value = out.value;
                        f.script_pubkey = htlc_.script_pubkey;
                        funding_ = f;
                        funding_height_ = h;
                        break;
                    }
                    if (funding_) continue;  // a spend comes in a later transaction
                }
            }
            if (funding_ && !spend_) {
                for (const auto& in : tx.vin) {
                    if (!(in.prevout.txid == funding_->txid && in.prevout.vout == funding_->vout)) continue;
                    const auto& w = in.witness;
                    FoundSpend sp;
                    sp.height = h;
                    if (!tx.vout.empty()) {
                        sp.output.txid = TxId::Compute(tx);
                        sp.output.vout = 0;
                        sp.output.value = tx.vout[0].value;
                        sp.output.script_pubkey = tx.vout[0].scriptPubKey;
                    }
                    sp.by_claim = w.size() == 4 && w[2] == htlc_.claim_script;  // the leaf is committed
                    if (sp.by_claim && w[1].size() == 32) {
                        Bytes32 secret{};
                        std::copy(w[1].begin(), w[1].end(), secret.begin());
                        sp.preimage = secret;
                    }
                    spend_ = sp;
                }
            }
        }
        next_height_ = h + 1;
        scanned_hash_ = *hash;
    }

    if (funding_) {
        report.funding = funding_;
        report.htlc.output_seen = true;
        report.htlc.output_value = funding_->value.GetUna();
        report.htlc.output_confirmations = tip - funding_height_ + 1;
    }
    if (spend_) {
        report.htlc.spent = true;
        report.htlc.spent_by_claim = spend_->by_claim;
        report.htlc.revealed_preimage = spend_->preimage;
        report.htlc.spend_confirmations = tip - spend_->height + 1;
        if (spend_->by_claim && !spend_->output.script_pubkey.empty()) report.claim_output = spend_->output;
    }
    report.ok = true;
    return report;
}

}  // namespace dinero::swap

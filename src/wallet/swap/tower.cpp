#include "wallet/swap/tower.h"

#include "crypto/sha256.h"
#include "wallet/swap/swap_crypto.h"

#include <cstdio>
#include <fcntl.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>

namespace dinero::swap {
namespace {

using namespace detail;

constexpr char kPackageFormat[] = "dinswap1t1";

[[noreturn]] void Bad(const std::string& why) {
    throw std::invalid_argument("tower package refused: " + why);
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); } else { cur += c; }
    }
    out.push_back(cur);
    return out;
}

uint64_t ParseU64(const std::string& s) {
    if (s.empty() || s.size() > 20 || s.find_first_not_of("0123456789") != std::string::npos) Bad("bad number");
    return std::stoull(s);
}

uint64_t BtcVsize(const BtcTx& tx) {
    const uint64_t base = SerializeBtcTx(tx, false).size();
    const uint64_t total = SerializeBtcTx(tx, true).size();
    return (base * 3 + total + 3) / 4;
}

FundingOutput DinFundingOf(const TowerPackage& p, const DinHtlcOutput& htlc) {
    FundingOutput f;
    f.txid = p.din_claims.front().tx.vin.at(0).prevout.txid;
    f.vout = p.din_claims.front().tx.vin.at(0).prevout.vout;
    f.value = AmountUna::Una(p.offer.din_amount_una);
    f.script_pubkey = htlc.script_pubkey;
    return f;
}

BtcFunding BtcFundingOf(const TowerPackage& p) {
    BtcFunding f;
    f.txid = p.btc_refunds.front().tx.vin.at(0).prev_txid;
    f.vout = p.btc_refunds.front().tx.vin.at(0).prev_vout;
    f.value_sat = p.offer.btc_amount_sat;
    return f;
}

}  // namespace

// ---- Package text form ------------------------------------------------------

std::string EncodeTowerPackage(const TowerPackage& p) {
    std::ostringstream out;
    out << "format=" << kPackageFormat << "\n"
        << "offer=" << EncodeOffer(p.offer) << "\n"
        << "accept=" << EncodeAccept(p.accept) << "\n"
        << "btc_scan_from_height=" << p.btc_scan_from_height << "\n"
        << "din_scan_from_height=" << p.din_scan_from_height << "\n"
        << "din_sweep_pubkey="
        << (p.din_sweep_pubkey == Bytes32{} ? std::string()
                                            : ToHex(std::vector<uint8_t>(p.din_sweep_pubkey.begin(), p.din_sweep_pubkey.end())))
        << "\n";
    for (const auto& r : p.din_claims) {
        out << "din_claim=" << ToHex(r.tx.Serialize(TxSerializationMode::WithWitness)) << ":"
            << ToHex(std::vector<uint8_t>(r.signature.begin(), r.signature.end())) << ":" << r.fee_una << "\n";
    }
    for (size_t i = 0; i < p.din_claims.size(); ++i) {
        for (const auto& c : p.din_claims[i].children) {
            out << "din_child=" << i << ":" << ToHex(c.tx.Serialize(TxSerializationMode::WithWitness)) << ":" << c.fee_una
                << "\n";
        }
    }
    for (const auto& r : p.btc_refunds) out << "btc_refund=" << ToHex(SerializeBtcTx(r.tx)) << ":" << r.fee_sat << "\n";
    return out.str();
}

TowerPackage DecodeTowerPackage(const std::string& text) {
    TowerPackage p;
    std::map<std::string, std::string> single;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) Bad("malformed line");
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "din_claim") {
            const auto parts = Split(value, ':');
            if (parts.size() != 3) Bad("malformed din_claim");
            DinClaimRung r;
            size_t used = 0;
            const auto raw = FromHex(parts[0]);
            if (!TransactionSerializer::Deserialize(r.tx, raw, used) || used != raw.size()) Bad("bad din_claim tx");
            const auto sig = FromHex(parts[1]);
            if (sig.size() != 64) Bad("bad din_claim signature");
            std::copy(sig.begin(), sig.end(), r.signature.begin());
            r.fee_una = ParseU64(parts[2]);
            p.din_claims.push_back(std::move(r));
        } else if (key == "din_child") {
            const auto parts = Split(value, ':');
            if (parts.size() != 3) Bad("malformed din_child");
            const uint64_t i = ParseU64(parts[0]);
            if (i >= p.din_claims.size()) Bad("din_child for an unknown rung");
            DinChildRung c;
            size_t used = 0;
            const auto raw = FromHex(parts[1]);
            if (!TransactionSerializer::Deserialize(c.tx, raw, used) || used != raw.size()) Bad("bad din_child tx");
            c.fee_una = ParseU64(parts[2]);
            p.din_claims[i].children.push_back(std::move(c));
        } else if (key == "btc_refund") {
            const auto parts = Split(value, ':');
            if (parts.size() != 2) Bad("malformed btc_refund");
            BtcRefundRung r;
            r.tx = ParseBtcTx(FromHex(parts[0]));
            r.fee_sat = ParseU64(parts[1]);
            p.btc_refunds.push_back(std::move(r));
        } else if (!single.emplace(key, value).second) {
            Bad("duplicate " + key);
        }
    }
    if (single["format"] != kPackageFormat) Bad("unknown format");
    for (const char* k : {"offer", "accept", "btc_scan_from_height", "din_scan_from_height"}) {
        if (!single.count(k)) Bad(std::string("missing ") + k);
    }
    p.offer = DecodeOffer(single["offer"]);
    p.accept = DecodeAccept(single["accept"]);
    const uint64_t h = ParseU64(single["btc_scan_from_height"]);
    if (h > UINT32_MAX) Bad("bad scan height");
    p.btc_scan_from_height = static_cast<uint32_t>(h);
    const uint64_t dh = ParseU64(single["din_scan_from_height"]);
    if (dh > UINT32_MAX) Bad("bad DIN scan height");
    p.din_scan_from_height = static_cast<uint32_t>(dh);
    if (!single["din_sweep_pubkey"].empty()) {
        const auto k = FromHex(single["din_sweep_pubkey"]);
        if (k.size() != 32) Bad("bad din_sweep_pubkey");
        std::copy(k.begin(), k.end(), p.din_sweep_pubkey.begin());
    }
    VerifyTowerPackage(p);
    return p;
}

// ---- Building (Bob) and verifying (tower) -------------------------------------

TowerPackage BuildTowerPackage(const SwapSession& s, const SwapKeys& keys, const FundingOutput& din_funding,
                               const BtcFunding& btc_funding, const FeeLadderPolicy& din_policy,
                               const BtcFeeLadderPolicy& btc_policy) {
    const auto& rec = s.record;
    if (rec.role != Role::BtcSeller) Fail("only Bob arms a watchtower");
    const auto din_terms = MakeDinTerms(rec.offer, rec.accept);
    const auto btc_terms = MakeBtcTerms(rec.offer, rec.accept);
    if (XOnlyOf(keys.din_secret_key) != din_terms.claim_pubkey) Fail("DIN key does not match the swap");
    if (CompressedOf(keys.btc_secret_key) != btc_terms.refund_pubkey) Fail("BTC key does not match the swap");

    TowerPackage p;
    p.offer = rec.offer;
    p.accept = rec.accept;
    p.btc_scan_from_height = s.btc_scan_from_height;
    p.din_scan_from_height = s.din_scan_from_height;

    const auto htlc = BuildDinHtlc(din_terms);
    // CPFP: the claim rungs pay Bob's sweep output; each gets a few pre-signed
    // children (fees x1, x4, x16, x64 of the rung's) that pay his wallet.
    const bool cpfp = s.din_sweep_pubkey != Bytes32{};
    if (cpfp && XOnlyOf(keys.din_sweep_secret_key) != s.din_sweep_pubkey) Fail("sweep key does not match the swap");
    const auto sweep = cpfp ? BuildDinSweepOutput(s.din_sweep_pubkey) : DinSweepOutput{};
    p.din_sweep_pubkey = s.din_sweep_pubkey;
    for (auto& rung : BuildDinClaimLadder(htlc, din_funding, cpfp ? sweep.script_pubkey : s.din_payout_script,
                                          din_policy)) {
        DinClaimRung r;
        r.signature = SchnorrSign(keys.din_secret_key, rung.sighash);
        r.tx = std::move(rung.tx);
        r.fee_una = rung.fee_una;
        if (cpfp) {
            FundingOutput parent;
            parent.txid = TxId::Compute(r.tx);  // ignores the witness the secret goes into
            parent.vout = 0;
            parent.value = r.tx.vout[0].value;
            parent.script_pubkey = sweep.script_pubkey;
            uint64_t fee = r.fee_una;
            for (int level = 0; level < 4; ++level, fee *= 4) {
                if (fee > din_policy.max_fee_una || fee >= parent.value.GetUna() ||
                    parent.value.GetUna() - fee < din_policy.min_payout_una) {
                    break;
                }
                DinChildRung c;
                c.tx = BuildDinSweepTx(sweep, parent, Payout{s.din_payout_script, AmountUna::Una(fee)});
                SetDinSweepWitness(c.tx, sweep,
                                   SchnorrSign(keys.din_sweep_secret_key, DinSweepSighash(c.tx, parent, sweep)));
                c.fee_una = fee;
                r.children.push_back(std::move(c));
            }
            if (r.children.empty()) Fail("no CPFP child fits the fee policy");
        }
        p.din_claims.push_back(std::move(r));
    }

    if (btc_policy.start_feerate_sat_per_vb == 0 || btc_policy.max_rungs == 0 || btc_policy.max_rungs > 16) {
        Fail("bad BTC fee policy");
    }
    // The signed size does not depend on the fee; measure it once (+1 vB for a
    // DER signature one byte longer than the one measured).
    BtcTx sized = BuildBtcRefundTx(btc_terms, btc_funding, s.btc_payout_script, 1);
    SetBtcRefundWitness(sized, btc_terms, EcdsaSignAll(keys.btc_secret_key, BtcHtlcSighash(sized, btc_terms, btc_funding)));
    const uint64_t vsize = BtcVsize(sized) + 1;
    uint64_t feerate = btc_policy.start_feerate_sat_per_vb;
    for (uint32_t i = 0; i < btc_policy.max_rungs; ++i, feerate *= 2) {
        if (feerate > btc_policy.max_fee_sat / vsize) break;
        const uint64_t fee = feerate * vsize;
        if (fee >= btc_funding.value_sat || btc_funding.value_sat - fee < btc_policy.min_payout_sat) break;
        BtcRefundRung r;
        r.tx = BuildBtcRefundTx(btc_terms, btc_funding, s.btc_payout_script, fee);
        SetBtcRefundWitness(r.tx, btc_terms, EcdsaSignAll(keys.btc_secret_key, BtcHtlcSighash(r.tx, btc_terms, btc_funding)));
        r.fee_sat = fee;
        p.btc_refunds.push_back(std::move(r));
    }
    if (p.btc_refunds.empty()) Fail("no BTC refund rung fits the fee policy");
    VerifyTowerPackage(p);
    return p;
}

void VerifyTowerPackage(const TowerPackage& p) {
    const auto din_terms = MakeDinTerms(p.offer, p.accept);  // throws on a mismatched accept
    const auto btc_terms = MakeBtcTerms(p.offer, p.accept);
    const auto htlc = BuildDinHtlc(din_terms);
    if (p.din_claims.empty()) Bad("no DIN claim rungs");
    if (p.btc_refunds.empty()) Bad("no BTC refund rungs");

    const FundingOutput din_funding = DinFundingOf(p, htlc);
    const bool cpfp = p.din_sweep_pubkey != Bytes32{};
    const auto sweep = cpfp ? BuildDinSweepOutput(p.din_sweep_pubkey) : DinSweepOutput{};
    std::optional<std::vector<uint8_t>> child_payout;
    const auto& din_payout = p.din_claims.front().tx.vout.empty() ? std::vector<uint8_t>{}
                                                                   : p.din_claims.front().tx.vout[0].scriptPubKey;
    uint64_t last_fee = 0;
    for (size_t i = 0; i < p.din_claims.size(); ++i) {
        const auto& r = p.din_claims[i];
        const std::string at = "DIN claim rung " + std::to_string(i) + ": ";
        if (r.tx.vin.size() != 1 || r.tx.vout.size() != 1) Bad(at + "must be one input, one output");
        if (!(r.tx.vin[0].prevout == TxOutPoint(din_funding.txid, din_funding.vout))) Bad(at + "spends another output");
        if (r.tx.vout[0].scriptPubKey != din_payout) Bad(at + "pays a different destination");
        if (r.fee_una >= p.offer.din_amount_una || r.tx.vout[0].value.GetUna() + r.fee_una != p.offer.din_amount_una) {
            Bad(at + "amount does not match the swap");
        }
        if (i > 0 && r.fee_una <= last_fee) Bad(at + "fees must rise");
        last_fee = r.fee_una;
        if (!SchnorrVerify(din_terms.claim_pubkey, DinClaimSighash(r.tx, din_funding, htlc), r.signature)) {
            Bad(at + "signature does not verify");
        }
        if (!cpfp) {
            if (!r.children.empty()) Bad(at + "children without a sweep key");
            continue;
        }
        if (r.tx.vout[0].scriptPubKey != sweep.script_pubkey) Bad(at + "does not pay the sweep output");
        if (r.children.empty()) Bad(at + "no CPFP child");
        FundingOutput parent;
        parent.txid = TxId::Compute(r.tx);
        parent.vout = 0;
        parent.value = r.tx.vout[0].value;
        parent.script_pubkey = sweep.script_pubkey;
        uint64_t last_child_fee = 0;
        for (size_t k = 0; k < r.children.size(); ++k) {
            const auto& c = r.children[k];
            const std::string cat = at + "child " + std::to_string(k) + ": ";
            if (c.tx.vin.size() != 1 || c.tx.vout.size() != 1) Bad(cat + "must be one input, one output");
            if (!(c.tx.vin[0].prevout == TxOutPoint(parent.txid, 0))) Bad(cat + "does not spend its rung");
            if (child_payout && c.tx.vout[0].scriptPubKey != *child_payout) Bad(cat + "pays a different destination");
            child_payout = c.tx.vout[0].scriptPubKey;
            if (c.fee_una >= parent.value.GetUna() || c.tx.vout[0].value.GetUna() + c.fee_una != parent.value.GetUna()) {
                Bad(cat + "amount does not match");
            }
            if (k > 0 && c.fee_una <= last_child_fee) Bad(cat + "fees must rise");
            last_child_fee = c.fee_una;
            const auto& w = c.tx.vin[0].witness;
            if (w.size() != 3 || w[0].size() != 64 || w[1] != sweep.leaf_script || w[2] != sweep.control_block) {
                Bad(cat + "not a sweep witness");
            }
            std::array<uint8_t, 64> sig{};
            std::copy(w[0].begin(), w[0].end(), sig.begin());
            if (!SchnorrVerify(p.din_sweep_pubkey, DinSweepSighash(c.tx, parent, sweep), sig)) {
                Bad(cat + "signature does not verify");
            }
        }
    }

    const BtcFunding btc_funding = BtcFundingOf(p);
    const auto witness_script = BuildBtcHtlcWitnessScript(btc_terms);
    const auto& btc_payout = p.btc_refunds.front().tx.vout.empty() ? std::vector<uint8_t>{}
                                                                    : p.btc_refunds.front().tx.vout[0].script_pubkey;
    last_fee = 0;
    for (size_t i = 0; i < p.btc_refunds.size(); ++i) {
        const auto& r = p.btc_refunds[i];
        const std::string at = "BTC refund rung " + std::to_string(i) + ": ";
        if (r.tx.vin.size() != 1 || r.tx.vout.size() != 1) Bad(at + "must be one input, one output");
        if (r.tx.vin[0].prev_txid != btc_funding.txid || r.tx.vin[0].prev_vout != btc_funding.vout) {
            Bad(at + "spends another output");
        }
        if (r.tx.vout[0].script_pubkey != btc_payout) Bad(at + "pays a different destination");
        if (r.fee_sat >= p.offer.btc_amount_sat || r.tx.vout[0].value_sat + r.fee_sat != p.offer.btc_amount_sat) {
            Bad(at + "amount does not match the swap");
        }
        if (r.tx.locktime != p.offer.t_btc_unix) Bad(at + "locktime is not T_btc");
        if (i > 0 && r.fee_sat <= last_fee) Bad(at + "fees must rise");
        last_fee = r.fee_sat;
        const auto& w = r.tx.vin[0].witness;
        if (w.size() != 3 || !w[1].empty() || w[2] != witness_script) Bad(at + "not a refund-path witness");
        if (!EcdsaVerifyAll(btc_terms.refund_pubkey, BtcHtlcSighash(r.tx, btc_terms, btc_funding), w[0])) {
            Bad(at + "signature does not verify");
        }
    }
}

std::string WriteTowerInbox(const std::string& inbox_dir, const std::string& package_text) {
    const auto p = DecodeTowerPackage(package_text);  // never hand the tower something it would refuse
    const auto id = OfferId(p.offer);
    const std::string name = ToHex(std::vector<uint8_t>(id.begin(), id.begin() + 8));
    const std::string path = inbox_dir + "/" + name + ".pkg";
    WriteFileAtomically(path, package_text);  // temp "<name>.pkg.tmp" is not a .pkg; the rename is synced too
    return path;
}

SwapWatchSpec TowerWatchSpec(const TowerPackage& p, const std::string& btc_chain) {
    SwapWatchSpec w;
    w.offer = p.offer;
    w.accept = p.accept;
    w.din_scan_from_height = p.din_scan_from_height;
    w.btc_scan_from_height = p.btc_scan_from_height;
    w.din_funding_txid = p.din_claims.front().tx.vin.at(0).prevout.txid.AsUint256().GetHex();
    const auto& wire = p.btc_refunds.front().tx.vin.at(0).prev_txid;
    w.btc_funding_txid = ToHex(std::vector<uint8_t>(wire.rbegin(), wire.rend()));
    w.btc_chain = btc_chain;
    return w;
}

std::string TowerPackageHash(const std::string& package_text) {
    Bytes32 h{};
    crypto::CSHA256().Write(reinterpret_cast<const uint8_t*>(package_text.data()), package_text.size()).Finalize(h.data());
    return ToHex(std::vector<uint8_t>(h.begin(), h.end()));
}

void MarkTowerArmed(const std::string& inbox_dir, const std::string& swap_id, const std::string& package_hash,
                    uint32_t now) {
    WriteFileAtomically(inbox_dir + "/" + swap_id + ".armed", package_hash + " " + std::to_string(now) + "\n");
}

bool TowerAckFresh(const std::string& inbox_dir, const std::string& swap_id, const std::string& package_hash,
                   uint32_t now, uint32_t max_age) {
    std::ifstream in(inbox_dir + "/" + swap_id + ".armed");
    std::string hash;
    uint64_t t = 0;
    if (!(in >> hash >> t) || hash != package_hash) return false;
    return t <= uint64_t(now) + 60 && uint64_t(now) <= t + max_age;
}

// ---- The tower --------------------------------------------------------------

Watchtower::Watchtower(TowerPackage package, TowerConfig config, SwapChainIo& io)
    : package_(std::move(package)), config_(config), io_(io) {
    VerifyTowerPackage(package_);
}

size_t Watchtower::NextRung(Escalation& e, size_t rungs, uint32_t now) const {
    if (!e.started) {
        e = Escalation{0, now, true};
    } else if (now >= e.since + config_.escalate_after_seconds && e.rung + 1 < rungs) {
        ++e.rung;
        e.since = now;
    }
    return e.rung;
}

size_t IndexByUrgency(size_t count, uint32_t t_din, uint32_t din_mtp, const TowerConfig& config) {
    if (count == 0) return 0;
    const size_t top = count - 1;
    const int64_t left = int64_t(t_din) - int64_t(din_mtp);
    const int64_t relaxed = config.din_relaxed_before_seconds, urgent = config.din_urgent_before_seconds;
    if (left >= relaxed) return 0;
    if (left <= urgent || relaxed <= urgent) return top;
    return static_cast<size_t>((relaxed - left) * int64_t(top) / (relaxed - urgent));
}

size_t Watchtower::DinRungByUrgency(uint32_t din_mtp) const {
    return IndexByUrgency(package_.din_claims.size(), package_.offer.t_din_unix, din_mtp, config_);
}

TowerReport Watchtower::Tick(uint32_t now) {
    TowerReport report;
    const DinWatchReport din = io_.ObserveDin();
    const BtcWatchReport btc = io_.ObserveBtc();
    const auto& offer = package_.offer;
    const uint32_t settle = std::max<uint32_t>(1, config_.settle_confirmations);
    const auto& front = package_.din_claims.front().tx.vin[0].prevout;
    const bool din_lock_here = din.ok && din.funding && TxOutPoint(din.funding->txid, din.funding->vout) == front;
    // Healthy: both chains observed and the package's DIN lock on chain. The
    // daemon refreshes Bob's ack only while this holds.
    report.healthy = din.ok && btc.ok && din_lock_here;

    // Learn the secret from Alice's Bitcoin claim, once; then keep it, so a
    // later Bitcoin outage cannot stop the DIN duty.
    if (btc.ok && btc.htlc.spent && btc.htlc.spent_by_claim && btc.htlc.revealed_preimage) {
        Bytes32 h{};
        crypto::CSHA256().Write(btc.htlc.revealed_preimage->data(), 32).Finalize(h.data());
        if (h != offer.payment_hash) {
            report.events.push_back("ALERT: BTC claim with a secret that does not match; ignoring it");
        } else if (!secret_) {
            secret_ = btc.htlc.revealed_preimage;
            report.learned_secret = secret_;
        }
    }

    if (!din.ok) {
        if (!btc.ok) {
            report.events.push_back("not observed: neither node is reachable");
            return report;
        }
        // Bob's BTC refund needs only Bitcoin: a Dinero outage must not cost it.
        report.events.push_back("not observed: the Dinero node is unreachable; BTC refund duty only");
        if (!secret_) BtcRefundDuty(btc, now, report);
        return report;
    }
    if (!btc.ok && !secret_) {
        report.events.push_back("not observed: the Bitcoin node is unreachable or inconsistent");
        return report;
    }
    report.observed = btc.ok;

    // Bob's outcome in a block: done once buried (and swept), until then only
    // watched (a reorg that drops it makes the duty below active again).
    const bool din_claimed = din.htlc.spent && din.htlc.spent_by_claim && din.htlc.spend_confirmations >= 1;
    const bool btc_refunded =
        btc.ok && btc.htlc.spent && !btc.htlc.spent_by_claim && btc.htlc.spend_confirmations >= 1;
    if (din_claimed || btc_refunded) {
        const uint32_t depth = din_claimed ? din.htlc.spend_confirmations : btc.htlc.spend_confirmations;
        const std::string what = din_claimed ? "DIN claim" : "BTC refund";
        // Dinero outcomes need at least the depth Bob required before he locked BTC.
        const uint32_t needed = din_claimed ? std::max(settle, offer.n_din_confirmations) : settle;
        bool swept = true;
        if (din_claimed && package_.din_sweep_pubkey != Bytes32{} && din.claim_output) {
            // Sweep the mined rung's output to Bob's wallet (its cheapest child).
            // Settled only once that output is spent, whoever mined the claim.
            const auto unspent = io_.DinOutputUnspent(din.claim_output->txid, din.claim_output->vout);
            swept = unspent.has_value() && !*unspent;
            if (unspent && *unspent) {
                bool have_child = false;
                for (const auto& rung : package_.din_claims) {
                    if (TxId::Compute(rung.tx) != din.claim_output->txid || rung.children.empty()) continue;
                    have_child = true;
                    try {
                        report.events.push_back(
                            "sweeping the DIN to Bob's wallet: " +
                            io_.BroadcastDin(rung.children.front().tx.Serialize(TxSerializationMode::WithWitness)));
                    } catch (const std::exception& e) {
                        report.events.push_back(std::string("sweep not accepted: ") + e.what());
                    }
                }
                if (!have_child) {
                    report.events.push_back("ALERT: the mined DIN claim is not a package rung; the tower cannot "
                                            "sweep it — Bob's node must (start it with the wallet unlocked)");
                }
            }
        }
        if (depth >= needed && swept) {
            report.finished = true;
            report.events.push_back(what + " " + std::to_string(depth) + " deep: settled for Bob");
        }
        return report;
    }

    if (secret_) {
        DinClaimDuty(din, now, report);
        return report;
    }
    if (btc.ok) BtcRefundDuty(btc, now, report);
    return report;
}

// Alice revealed the secret: claim the DIN with it (Dinero alone suffices).
void Watchtower::DinClaimDuty(const DinWatchReport& din, uint32_t now, TowerReport& report) {
    const auto& offer = package_.offer;
    const uint32_t settle = std::max<uint32_t>(1, config_.settle_confirmations);
    if (din.htlc.spent && !din.htlc.spent_by_claim) {
        // Final only once buried: if a reorg drops the refund, claim again.
        report.events.push_back("ALERT: Alice refunded the DIN after claiming the BTC");
        report.finished = din.htlc.spend_confirmations >= std::max(settle, offer.n_din_confirmations);
        return;
    }
    const auto& front = package_.din_claims.front().tx.vin[0].prevout;
    if (!din.funding || !(TxOutPoint(din.funding->txid, din.funding->vout) == front)) {
        report.events.push_back("ALERT: the DIN lock in the package is not on chain");
        return;
    }
    if (!duty_since_) duty_since_ = now;
    // No replace-by-fee on Dinero: a lower rung already in mempools stays, and
    // a higher one is refused harmlessly until the lower one confirms or drops.
    const size_t i = DinRungByUrgency(din.mtp_unix);
    // Bump: still unmined a while into the duty. Which rung sits in mempools
    // is not known (an earlier choice, or one from before a restart), so offer
    // a child of every rung: only the real parent's child can be accepted.
    const int64_t left = int64_t(offer.t_din_unix) - int64_t(din.mtp_unix);
    const bool bump_due = left <= int64_t(config_.din_bump_window_seconds) ||
                          now >= *duty_since_ + config_.din_bump_stuck_seconds;
    if (!din.htlc.spent && now >= *duty_since_ + config_.din_bump_after_seconds && bump_due) {
        size_t offered = 0;
        for (const auto& rung : package_.din_claims) {
            if (rung.children.empty()) continue;
            try {
                // The only bump there will be: the largest child.
                io_.BroadcastDin(rung.children.back().tx.Serialize(TxSerializationMode::WithWitness));
                ++offered;
            } catch (const std::exception&) {
                // parent not in the mempool (or child already there): expected for all but one
            }
        }
        if (offered) report.events.push_back("DIN claim unmined; CPFP children offered (" + std::to_string(offered) + " accepted)");
    }
    const auto terms = MakeDinTerms(package_.offer, package_.accept);
    Transaction tx = package_.din_claims[i].tx;
    SetDinClaimWitness(tx, terms, BuildDinHtlc(terms), package_.din_claims[i].signature,
                       std::vector<uint8_t>(secret_->begin(), secret_->end()));
    try {
        report.events.push_back("DIN claim rung " + std::to_string(i) + " broadcast: " +
                                io_.BroadcastDin(tx.Serialize(TxSerializationMode::WithWitness)));
    } catch (const std::exception& e) {
        report.events.push_back("DIN claim rung " + std::to_string(i) + " not accepted: " + e.what());
    }
}

// No secret: refund the BTC once T_btc < Bitcoin's median time past.
void Watchtower::BtcRefundDuty(const BtcWatchReport& btc, uint32_t now, TowerReport& report) {
    const auto& offer = package_.offer;
    if (btc.htlc.spent_by_claim || (btc.htlc.spent && btc.htlc.spend_confirmations >= 1)) return;
    if (btc.mtp_unix <= offer.t_btc_unix || !btc.htlc.output_seen) return;
    const size_t i = NextRung(btc_refund_, package_.btc_refunds.size(), now);
    try {
        report.events.push_back("BTC refund rung " + std::to_string(i) + " broadcast: " +
                                io_.BroadcastBtc(SerializeBtcTx(package_.btc_refunds[i].tx)));
    } catch (const std::exception& e) {
        report.events.push_back("BTC refund rung " + std::to_string(i) + " not accepted: " + e.what());
    }
}

void Watchtower::SetKnownSecret(const Bytes32& secret) {
    Bytes32 h{};
    crypto::CSHA256().Write(secret.data(), secret.size()).Finalize(h.data());
    if (h != package_.offer.payment_hash) Bad("the kept secret does not match this swap");
    secret_ = secret;
}

}  // namespace dinero::swap

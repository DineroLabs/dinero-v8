#include "wallet/swap/tower.h"

#include "crypto/sha256.h"
#include "wallet/swap/swap_crypto.h"

#include <cstdio>
#include <fcntl.h>
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
        << "btc_scan_from_height=" << p.btc_scan_from_height << "\n";
    for (const auto& r : p.din_claims) {
        out << "din_claim=" << ToHex(r.tx.Serialize(TxSerializationMode::WithWitness)) << ":"
            << ToHex(std::vector<uint8_t>(r.signature.begin(), r.signature.end())) << ":" << r.fee_una << "\n";
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
    for (const char* k : {"offer", "accept", "btc_scan_from_height"}) {
        if (!single.count(k)) Bad(std::string("missing ") + k);
    }
    p.offer = DecodeOffer(single["offer"]);
    p.accept = DecodeAccept(single["accept"]);
    const uint64_t h = ParseU64(single["btc_scan_from_height"]);
    if (h > UINT32_MAX) Bad("bad scan height");
    p.btc_scan_from_height = static_cast<uint32_t>(h);
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

    const auto htlc = BuildDinHtlc(din_terms);
    for (auto& rung : BuildDinClaimLadder(htlc, din_funding, s.din_payout_script, din_policy)) {
        DinClaimRung r;
        r.signature = SchnorrSign(keys.din_secret_key, rung.sighash);
        r.tx = std::move(rung.tx);
        r.fee_una = rung.fee_una;
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
    const std::string path = inbox_dir + "/" + name + ".pkg", tmp = inbox_dir + "/." + name + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("tower inbox: cannot open " + tmp);
    size_t off = 0;
    while (off < package_text.size()) {
        const ssize_t n = ::write(fd, package_text.data() + off, package_text.size() - off);
        if (n <= 0) { ::close(fd); throw std::runtime_error("tower inbox: write failed"); }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) { ::close(fd); throw std::runtime_error("tower inbox: fsync failed"); }
    ::close(fd);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) throw std::runtime_error("tower inbox: rename failed");
    return path;
}

// ---- The tower --------------------------------------------------------------

Watchtower::Watchtower(TowerPackage package, TowerConfig config, SwapChainIo& io)
    : package_(std::move(package)), config_(config), io_(io) {
    VerifyTowerPackage(package_);
}

size_t Watchtower::NextRung(Escalation& e, size_t rungs, uint32_t now, bool urgent) const {
    if (!e.started) {
        e = Escalation{0, now, true};
    } else if (now >= e.since + config_.escalate_after_seconds && e.rung + 1 < rungs) {
        ++e.rung;
        e.since = now;
    }
    if (urgent) e.rung = rungs - 1;
    return e.rung;
}

TowerReport Watchtower::Tick(uint32_t now) {
    TowerReport report;
    const DinWatchReport din = io_.ObserveDin();
    const BtcWatchReport btc = io_.ObserveBtc();
    if (!din.ok || !btc.ok) {
        report.events.push_back("not observed: a node is unreachable or inconsistent");
        return report;
    }
    report.observed = true;
    const auto& offer = package_.offer;

    // Settled outcomes.
    if (din.htlc.spent && din.htlc.spent_by_claim && din.htlc.spend_confirmations >= 1) {
        report.finished = true;
        report.events.push_back("DIN claim confirmed: Bob has his DIN");
        return report;
    }
    if (btc.htlc.spent && !btc.htlc.spent_by_claim && btc.htlc.spend_confirmations >= 1) {
        report.finished = true;
        report.events.push_back("BTC refund confirmed: Bob has his BTC back");
        return report;
    }

    // Alice revealed the secret: claim the DIN with it.
    std::optional<Bytes32> secret;
    if (btc.htlc.spent && btc.htlc.spent_by_claim && btc.htlc.revealed_preimage) {
        Bytes32 h{};
        crypto::CSHA256().Write(btc.htlc.revealed_preimage->data(), 32).Finalize(h.data());
        if (h == offer.payment_hash) {
            secret = btc.htlc.revealed_preimage;
        } else {
            report.events.push_back("ALERT: BTC claim with a secret that does not match; ignoring it");
        }
    }
    if (secret) {
        if (din.htlc.spent && !din.htlc.spent_by_claim) {
            report.finished = true;
            report.events.push_back("ALERT: Alice refunded the DIN after claiming the BTC: Bob lost the DIN");
            return report;
        }
        const auto& front = package_.din_claims.front().tx.vin[0].prevout;
        if (!din.funding || !(TxOutPoint(din.funding->txid, din.funding->vout) == front)) {
            report.events.push_back("ALERT: the DIN lock in the package is not on chain");
            return report;
        }
        const bool urgent = uint64_t(din.mtp_unix) + config_.din_urgent_before_seconds >= offer.t_din_unix;
        const size_t i = NextRung(din_claim_, package_.din_claims.size(), now, urgent);
        const auto terms = MakeDinTerms(package_.offer, package_.accept);
        Transaction tx = package_.din_claims[i].tx;
        SetDinClaimWitness(tx, terms, BuildDinHtlc(terms), package_.din_claims[i].signature,
                           std::vector<uint8_t>(secret->begin(), secret->end()));
        try {
            report.events.push_back("DIN claim rung " + std::to_string(i) + " broadcast: " +
                                    io_.BroadcastDin(tx.Serialize(TxSerializationMode::WithWitness)));
        } catch (const std::exception& e) {
            report.events.push_back("DIN claim rung " + std::to_string(i) + " not accepted: " + e.what());
        }
        return report;
    }

    // No secret: refund the BTC once Bitcoin's median time reaches T_btc.
    if (!(btc.htlc.spent && btc.htlc.spent_by_claim) && btc.mtp_unix >= offer.t_btc_unix && btc.htlc.output_seen) {
        const size_t i = NextRung(btc_refund_, package_.btc_refunds.size(), now, false);
        try {
            report.events.push_back("BTC refund rung " + std::to_string(i) + " broadcast: " +
                                    io_.BroadcastBtc(SerializeBtcTx(package_.btc_refunds[i].tx)));
        } catch (const std::exception& e) {
            report.events.push_back("BTC refund rung " + std::to_string(i) + " not accepted: " + e.what());
        }
    }
    return report;
}

}  // namespace dinero::swap

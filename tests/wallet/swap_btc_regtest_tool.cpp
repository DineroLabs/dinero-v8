// Helper for the Bitcoin Core regtest check of the swap's BTC side
// (tests/wallet/swap_btc_regtest.sh). Fixed test keys; prints hex only.
//
//   swap_btc_regtest_tool script
//       -> "<witness_script_hex> <p2wsh_script_pubkey_hex>"
//   swap_btc_regtest_tool claim|refund <funding_txid> <vout> <value_sat> <payout_spk_hex> <fee_sat>
//       -> fully signed transaction hex (funding_txid in the usual display order)
#include "wallet/swap/btc_tx.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"

#include <secp256k1.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace dinero;
using namespace dinero::swap;

namespace {

constexpr uint32_t kLock = 1'800'000'000;
const std::vector<uint8_t> kPreimage(32, 0x5a);

std::array<uint8_t, 32> Secret(uint8_t s) { std::array<uint8_t, 32> a{}; a.back() = s; return a; }

std::array<uint8_t, 33> Pubkey(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_pubkey pk;
    const auto s = Secret(scalar);
    secp256k1_ec_pubkey_create(secp, &pk, s.data());
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(secp, out.data(), &len, &pk, SECP256K1_EC_COMPRESSED);
    return out;
}

std::vector<uint8_t> SignAll(uint8_t scalar, const Bytes32& msg) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    const auto s = Secret(scalar);
    secp256k1_ecdsa_signature sig;
    secp256k1_ecdsa_sign(secp, &sig, msg.data(), s.data(), nullptr, nullptr);  // low-S
    std::vector<uint8_t> der(72);
    size_t len = der.size();
    secp256k1_ecdsa_signature_serialize_der(secp, der.data(), &len, &sig);
    der.resize(len);
    der.push_back(0x01);
    return der;
}

std::vector<uint8_t> FromHex(const std::string& h) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(std::stoi(h.substr(i, 2), nullptr, 16));
    return out;
}

std::string ToHex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}

BtcHtlcTerms Terms() {
    BtcHtlcTerms t;
    crypto::CSHA256().Write(kPreimage.data(), kPreimage.size()).Finalize(t.payment_hash.data());
    t.claim_pubkey = Pubkey(4);   // Alice
    t.refund_pubkey = Pubkey(6);  // Bob
    t.refund_locktime_unix = kLock;
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string cmd = argc > 1 ? argv[1] : "";
        const auto terms = Terms();
        if (cmd == "script") {
            const auto ws = BuildBtcHtlcWitnessScript(terms);
            std::cout << ToHex(ws) << " " << ToHex(BtcP2wshScriptPubKey(ws)) << "\n";
            return 0;
        }
        if ((cmd == "claim" || cmd == "refund") && argc == 7) {
            BtcFunding f;
            const auto display = FromHex(argv[2]);
            if (display.size() != 32) throw std::invalid_argument("txid must be 32 bytes");
            for (size_t i = 0; i < 32; ++i) f.txid[i] = display[31 - i];  // display -> wire order
            f.vout = static_cast<uint32_t>(std::stoul(argv[3]));
            f.value_sat = std::stoull(argv[4]);
            const auto payout = FromHex(argv[5]);
            const uint64_t fee = std::stoull(argv[6]);
            BtcTx tx = cmd == "claim" ? BuildBtcClaimTx(f, payout, fee)
                                      : BuildBtcRefundTx(terms, f, payout, fee);
            const auto msg = BtcHtlcSighash(tx, terms, f);
            if (cmd == "claim") {
                SetBtcClaimWitness(tx, terms, SignAll(4, msg), kPreimage);
            } else {
                SetBtcRefundWitness(tx, terms, SignAll(6, msg));
            }
            std::cout << ToHex(SerializeBtcTx(tx)) << "\n";
            return 0;
        }
        std::cerr << "usage: script | claim|refund <txid> <vout> <value_sat> <payout_spk> <fee_sat>\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

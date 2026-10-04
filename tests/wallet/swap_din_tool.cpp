// Helper for the Dinero regtest check of the DIN side (tests/wallet/swap_din_regtest.sh).
// Fixed test keys: Bob (claim) scalar 2, Alice (refund) scalar 3, secret 32 x 0x5a.
//   swap_din_tool address <hrp> <lock>                 -> "<address> <script_pubkey_hex>"
//   swap_din_tool claim|refund <lock> <funding_txid> <vout> <value_una> <payout_spk_hex> <fee_una>
//                                                      -> signed transaction hex
//   swap_din_tool watch <rpcport> <user> <pass> <hrp> <lock>
//                                                      -> one observation per stdin line
#include "wallet/swap/din_watcher.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "primitives/transaction.h"
#include "rpc_client.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <iostream>
#include <string>

using namespace dinero;
using namespace dinero::swap;

namespace {

const std::vector<uint8_t> kSecret(32, 0x5a);

secp256k1_keypair Keypair(uint8_t scalar) {
    std::array<uint8_t, 32> s{};
    s.back() = scalar;
    secp256k1_keypair kp;
    secp256k1_keypair_create(crypto::GetSecp256k1ContextSignVerify(), &kp, s.data());
    return kp;
}

Bytes32 XOnly(uint8_t scalar) {
    auto kp = Keypair(scalar);
    secp256k1_xonly_pubkey x;
    secp256k1_keypair_xonly_pub(crypto::GetSecp256k1ContextSignVerify(), &x, nullptr, &kp);
    Bytes32 out{};
    secp256k1_xonly_pubkey_serialize(crypto::GetSecp256k1ContextSignVerify(), out.data(), &x);
    return out;
}

std::array<uint8_t, 64> Sign(uint8_t scalar, const Bytes32& msg) {
    auto kp = Keypair(scalar);
    std::array<uint8_t, 64> sig{};
    const std::array<uint8_t, 32> aux{};
    secp256k1_schnorrsig_sign32(crypto::GetSecp256k1ContextSignVerify(), sig.data(), msg.data(), &kp, aux.data());
    return sig;
}

std::string ToHex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}

std::vector<uint8_t> FromHex(const std::string& h) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(std::stoi(h.substr(i, 2), nullptr, 16));
    return out;
}

DinHtlcTerms Terms(uint32_t lock) {
    DinHtlcTerms t;
    crypto::CSHA256().Write(kSecret.data(), kSecret.size()).Finalize(t.payment_hash.data());
    t.claim_pubkey = XOnly(2);
    t.refund_pubkey = XOnly(3);
    t.refund_locktime_unix = lock;
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string cmd = argc > 1 ? argv[1] : "";
        if (cmd == "address" && argc == 4) {
            const auto htlc = BuildDinHtlc(Terms(static_cast<uint32_t>(std::stoul(argv[3]))));
            std::cout << DinHtlcAddress(htlc, argv[2]) << " " << ToHex(htlc.script_pubkey) << "\n";
            return 0;
        }
        if ((cmd == "claim" || cmd == "refund") && argc == 8) {
            const auto terms = Terms(static_cast<uint32_t>(std::stoul(argv[2])));
            const auto htlc = BuildDinHtlc(terms);
            FundingOutput f;
            f.txid = TxId(uint256::FromHexUnsafe(argv[3]));
            f.vout = static_cast<uint32_t>(std::stoul(argv[4]));
            f.value = AmountUna::Una(std::stoull(argv[5]));
            f.script_pubkey = htlc.script_pubkey;
            const Payout payout{FromHex(argv[6]), AmountUna::Una(std::stoull(argv[7]))};
            Transaction tx;
            if (cmd == "claim") {
                tx = BuildDinClaimTx(htlc, f, payout);
                SetDinClaimWitness(tx, terms, htlc, Sign(2, DinClaimSighash(tx, f, htlc)), kSecret);
            } else {
                tx = BuildDinRefundTx(terms, htlc, f, payout);
                SetDinRefundWitness(tx, htlc, Sign(3, DinRefundSighash(tx, f, htlc)));
            }
            std::cout << ToHex(tx.Serialize(TxSerializationMode::WithWitness)) << "\n";
            return 0;
        }
        if (cmd == "watch" && argc == 7) {
            dinero::rpc::RpcClient client("127.0.0.1", static_cast<uint16_t>(std::stoi(argv[2])), argv[3], argv[4]);
            DinWatcher watcher(
                [&](const std::string& m, const Json::Value& p) {
                    auto r = client.call(m, p);
                    if (!r || !r->isMember("result") || (r->isMember("error") && !(*r)["error"].isNull())) {
                        return std::optional<Json::Value>{};
                    }
                    return std::optional<Json::Value>{(*r)["result"]};
                },
                Terms(static_cast<uint32_t>(std::stoul(argv[6]))), argv[5]);
            for (std::string line; std::getline(std::cin, line);) {
                const auto r = watcher.Observe();
                std::cout << "ok=" << r.ok << " seen=" << r.htlc.output_seen
                          << " confs=" << r.htlc.output_confirmations << " value=" << r.htlc.output_value
                          << " spent=" << r.htlc.spent << " claim=" << r.htlc.spent_by_claim
                          << " spendconfs=" << r.htlc.spend_confirmations
                          << " secret=" << r.htlc.revealed_preimage.has_value() << " fund="
                          << (r.funding ? r.funding->txid.AsUint256().GetHex() + ":" + std::to_string(r.funding->vout)
                                        : std::string("-"))
                          << std::endl;
            }
            return 0;
        }
        std::cerr << "usage: see file header\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

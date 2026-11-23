// Helper for the Zebra regtest check of the ZEC swap leg
// (tests/wallet/swap_zec_regtest.sh). Zebra has no wallet, so this tool holds
// the test keys, builds and signs every transaction, and talks to zebrad's RPC.
//
// Fixed keys (scalars): miner 7, deposit 8, Alice's claim key 4, Bob's refund key 6.
// Fixed secret: 32 x 0x5a. Amounts in zatoshi. Commands:
//   addr <scalar>                                        tm address of a key
//   htlcaddr <lock>                                      t2 address of the HTLC
//   chain <port>                                         tip=<h> mtp=<unix> branch=<hex>
//   pay <port> <from_scalar> <to_addr> <zat>             spend that key's mature coins; prints the txid
//   deposit <port> <min_confs>                           confirmed=<zat> coins=<n>
//   fund <port> <zat> <lock> <change_addr>               deposit -> HTLC; prints "<txid> <vout>"
//   sweep <port> <to_addr>                               deposit -> to_addr (swap.cancel)
//   spend <port> <kind> <txid> <vout> <zat> <lock> <expiry>
//        kind: claim | badclaim | refund | early_refund  prints the signed hex
//   send <port> <hex>                                    accepted <txid> | rejected <reason>
//   watch <port> <scan_from> <lock> <amount> [interactive]  one HTLC observation per stdin line
//   guard <port> <seconds_after_mtp>                     upgrade=<name>@<height> | upgrade=none
#include "wallet/swap/zec_tx.h"
#include "wallet/swap/zec_watcher.h"

#include "crypto/sha256.h"
#include "rpc_client.h"
#include "wallet/swap/swap_crypto.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace dinero;
using namespace dinero::swap;

namespace {

constexpr uint64_t kSpendFee = 10'000;
const std::vector<uint8_t> kSecret(32, 0x5a);

Bytes32 Key(uint8_t scalar) {
    Bytes32 s{};
    s.back() = scalar;
    return s;
}

std::string Addr(uint8_t scalar) {
    return EncodeZecTAddress(ZecP2pkhAddressOf(detail::CompressedOf(Key(scalar)), ZecNetwork::Regtest));
}

ZecHtlcTerms Terms(uint32_t lock) {
    ZecHtlcTerms t;
    crypto::CSHA256().Write(kSecret.data(), kSecret.size()).Finalize(t.payment_hash.data());
    t.claim_pubkey = detail::CompressedOf(Key(4));
    t.refund_pubkey = detail::CompressedOf(Key(6));
    t.refund_locktime_unix = lock;
    return t;
}

ZecRpc MakeRpc(uint16_t port) {
    auto client = std::make_shared<dinero::rpc::RpcClient>("127.0.0.1", port, "", "");
    return [client](const std::string& m, const Json::Value& p) -> std::optional<Json::Value> {
        auto r = client->call(m, p);
        if (!r || !r->isMember("result") || (r->isMember("error") && !(*r)["error"].isNull())) {
            if (std::getenv("SWAP_RPC_DEBUG")) std::cerr << m << ": " << client->get_last_error() << "\n";
            return std::nullopt;
        }
        return (*r)["result"];
    };
}

std::array<uint8_t, 32> Wire(const std::string& display) {
    const auto b = detail::FromHex(display);
    if (b.size() != 32) throw std::invalid_argument("bad txid");
    std::array<uint8_t, 32> w{};
    std::reverse_copy(b.begin(), b.end(), w.begin());
    return w;
}

std::string Display(const Bytes32& wire) {
    return detail::ToHex(std::vector<uint8_t>(wire.rbegin(), wire.rend()));
}

ZecChainInfo Chain(const ZecRpc& rpc) {
    const auto c = ObserveZecChain(rpc, "test");
    if (!c) throw std::runtime_error("cannot read the regtest chain");
    return *c;
}

int Send(uint16_t port, const std::string& hex) {
    dinero::rpc::RpcClient client("127.0.0.1", port, "", "");
    Json::Value p(Json::arrayValue);
    p.append(hex);
    auto r = client.call("sendrawtransaction", p);
    if (r && r->isMember("result") && (*r)["result"].isString()) {
        std::cout << "accepted " << (*r)["result"].asString() << std::endl;
        return 0;
    }
    std::string why = r && r->isMember("error") ? (*r)["error"]["message"].asString() : client.get_last_error();
    for (auto& ch : why) if (ch == '\n') ch = ' ';
    std::cout << "rejected " << why << std::endl;
    return 0;
}

// Signs and returns the funding/payment transaction spending `coins` of `scalar`.
std::string PayFrom(const ZecRpc& rpc, uint8_t scalar, const std::vector<ZecCoin>& coins,
                    const std::vector<uint8_t>& to_spk, uint64_t amount, uint32_t multiplier) {
    const auto chain = Chain(rpc);
    const auto change = ZecScriptPubKey(ZecP2pkhAddressOf(detail::CompressedOf(Key(scalar)), ZecNetwork::Regtest));
    auto plan = BuildZecFundingTx(coins, to_spk, amount, change, multiplier, chain.next_block_branch_id,
                                  chain.tip_height + 40);
    SignZecP2pkhInputs(plan.tx, coins, Key(scalar));
    return detail::ToHex(SerializeZecTx(plan.tx));
}

}  // namespace

int main(int argc, char** argv) try {
    const std::vector<std::string> a(argv + 1, argv + argc);
    if (a.empty()) return 2;
    const std::string& cmd = a[0];
    if (cmd == "addr") {
        std::cout << Addr(static_cast<uint8_t>(std::stoi(a.at(1)))) << std::endl;
        return 0;
    }
    if (cmd == "htlcaddr") {
        const auto spk = ZecP2shScriptPubKey(BuildZecHtlcRedeemScript(Terms(std::stoul(a.at(1)))));
        ZecTAddress addr{ZecNetwork::Regtest, ZecAddressKind::P2sh, {}};
        std::copy(spk.begin() + 2, spk.begin() + 22, addr.hash.begin());
        std::cout << EncodeZecTAddress(addr) << std::endl;
        return 0;
    }
    const auto port = static_cast<uint16_t>(std::stoi(a.at(1)));
    const auto rpc = MakeRpc(port);

    if (cmd == "chain") {
        const auto c = Chain(rpc);
        char branch[9];
        std::snprintf(branch, sizeof branch, "%08x", c.next_block_branch_id);
        std::cout << "tip=" << c.tip_height << " mtp=" << c.mtp_unix << " branch=" << branch << std::endl;
    } else if (cmd == "pay") {
        const auto scalar = static_cast<uint8_t>(std::stoi(a.at(2)));
        const uint64_t amount = std::stoull(a.at(4));
        // Mature coinbase only (100 confirmations), smallest first until it covers amount + fee.
        auto report = ObserveZecDeposit(rpc, Addr(scalar), "test");
        if (!report.ok) throw std::runtime_error("cannot list coins");
        std::vector<ZecCoin> coins;
        uint64_t total = 0;
        for (const auto& c : report.coins) {
            if (c.confirmations < 101 || total >= amount + 2 * kSpendFee) continue;
            coins.push_back(c.coin);
            total += c.coin.value_zat;
        }
        const auto to = ZecScriptPubKey(DecodeZecTAddress(a.at(3)));
        return Send(port, PayFrom(rpc, scalar, coins, to, amount, 1));
    } else if (cmd == "deposit") {
        const auto report = ObserveZecDeposit(rpc, Addr(8), "test");
        if (!report.ok) throw std::runtime_error("cannot list the deposit");
        const auto min = static_cast<uint32_t>(std::stoul(a.at(2)));
        std::cout << "confirmed=" << report.confirmed_zat(min) << " coins=" << report.spendable(min).size() << std::endl;
    } else if (cmd == "fund") {
        const auto report = ObserveZecDeposit(rpc, Addr(8), "test");
        const auto coins = report.spendable(1);
        const auto htlc = ZecP2shScriptPubKey(BuildZecHtlcRedeemScript(Terms(std::stoul(a.at(3)))));
        const auto chain = Chain(rpc);
        auto plan = BuildZecFundingTx(coins, htlc, std::stoull(a.at(2)), ZecScriptPubKey(DecodeZecTAddress(a.at(4))), 2,
                                      chain.next_block_branch_id, chain.tip_height + 40);
        SignZecP2pkhInputs(plan.tx, coins, Key(8));
        dinero::rpc::RpcClient client("127.0.0.1", port, "", "");
        Json::Value p(Json::arrayValue);
        p.append(detail::ToHex(SerializeZecTx(plan.tx)));
        auto r = client.call("sendrawtransaction", p);
        if (!r || !(*r)["result"].isString()) throw std::runtime_error("funding rejected: " + r->toStyledString());
        std::cout << (*r)["result"].asString() << " 0" << std::endl;  // the HTLC is output 0
    } else if (cmd == "sweep") {
        const auto coins = ObserveZecDeposit(rpc, Addr(8), "test").spendable(1);
        const auto chain = Chain(rpc);
        auto tx = BuildZecSweepTx(coins, ZecScriptPubKey(DecodeZecTAddress(a.at(2))), chain.next_block_branch_id,
                                  chain.tip_height + 40);
        SignZecP2pkhInputs(tx, coins, Key(8));
        return Send(port, detail::ToHex(SerializeZecTx(tx)));
    } else if (cmd == "spend") {
        const std::string& kind = a.at(2);
        const ZecFunding funding{Wire(a.at(3)), static_cast<uint32_t>(std::stoul(a.at(4))), std::stoull(a.at(5))};
        const auto terms = Terms(std::stoul(a.at(6)));
        const auto expiry = static_cast<uint32_t>(std::stoul(a.at(7)));
        const auto chain = Chain(rpc);
        const auto payout = ZecScriptPubKey(ZecP2pkhAddressOf(detail::CompressedOf(Key(7)), ZecNetwork::Regtest));
        ZecTx tx;
        if (kind == "claim" || kind == "badclaim") {
            tx = BuildZecClaimTx(funding, payout, kSpendFee, chain.next_block_branch_id, expiry);
            const auto sig = detail::EcdsaSignAll(Key(4), ZecHtlcSighash(tx, terms, funding));
            if (kind == "claim") {
                SetZecClaimScriptSig(tx, terms, sig, kSecret);
            } else {  // a wrong 32-byte secret, otherwise a well-formed claim
                std::vector<uint8_t> s{static_cast<uint8_t>(sig.size())};
                s.insert(s.end(), sig.begin(), sig.end());
                s.push_back(0x20);
                s.insert(s.end(), 32, 0x5b);
                s.push_back(0x51);
                const auto redeem = BuildZecHtlcRedeemScript(terms);
                s.push_back(0x4c);
                s.push_back(static_cast<uint8_t>(redeem.size()));
                s.insert(s.end(), redeem.begin(), redeem.end());
                tx.vin[0].script_sig = s;
            }
        } else if (kind == "refund" || kind == "early_refund" || kind == "refund_nofee") {
            // refund_nofee: for a hand-built block, whose coinbase must claim exactly the fees in it
            tx = BuildZecRefundTx(terms, funding, payout, kind == "refund_nofee" ? 0 : kSpendFee,
                                  chain.next_block_branch_id, expiry);
            if (kind == "early_refund") tx.locktime = terms.refund_locktime_unix - 1;  // fails CLTV
            SetZecRefundScriptSig(tx, terms, detail::EcdsaSignAll(Key(6), ZecHtlcSighash(tx, terms, funding)));
        } else {
            return 2;
        }
        std::cout << detail::ToHex(SerializeZecTx(tx)) << std::endl;
    } else if (cmd == "send") {
        return Send(port, a.at(2));
    } else if (cmd == "watch") {
        ZecWatchTarget target;
        target.terms = Terms(std::stoul(a.at(3)));
        target.scan_from_height = static_cast<uint32_t>(std::stoul(a.at(2)));
        target.expected_amount_zat = std::stoull(a.at(4));
        target.expected_chain = "test";
        ZecWatcher watcher(rpc, target);
        auto print = [](const ZecWatchReport& r) {
            std::cout << "ok=" << r.ok << " seen=" << r.htlc.output_seen << " confs=" << r.htlc.output_confirmations
                      << " value=" << r.htlc.output_value << " spent=" << r.htlc.spent
                      << " claim=" << r.htlc.spent_by_claim << " spendconfs=" << r.htlc.spend_confirmations
                      << " secret=" << r.htlc.revealed_preimage.has_value()
                      << " fund=" << (r.funding ? Display(r.funding->txid) : "-") << std::endl;
        };
        if (a.size() > 5 && a[5] == "interactive") {
            for (std::string line; std::getline(std::cin, line);) print(watcher.Observe());
            return 0;
        }
        print(watcher.Observe());
    } else if (cmd == "guard") {
        const auto c = Chain(rpc);
        const auto u = ZecUpgradeBefore(c, c.mtp_unix, c.mtp_unix + static_cast<uint32_t>(std::stoul(a.at(2))));
        std::cout << "upgrade=" << (u ? u->name + "@" + std::to_string(u->activation_height) : "none") << std::endl;
    } else {
        return 2;
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "swap_zec_regtest_tool: " << e.what() << std::endl;
    return 1;
}

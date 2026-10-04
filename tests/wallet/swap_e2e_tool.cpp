// Two-chain end-to-end swap on regtest (tests/wallet/swap_e2e_regtest.sh):
// Alice (sells DIN) and Bob (sells BTC) each run their own SwapRunner, with
// their own store file, against one dinerod and one bitcoind. Between ticks
// both chains mine a block. Each runner is destroyed and rebuilt from its store
// once mid-swap. Outcomes are checked on chain (payout balances), not just by
// the final states.
//   swap_e2e_tool happy|offline|stale-clocks <workdir> <din_rpcport> <btc_rpcport>
//                 <btc_user> <btc_pass> <din_miner_address> <btc_miner_address>
// happy:        honest clocks; both reach Done; Alice's early DIN refund is
//               refused by the node as non-final (lock enforcement is live).
// offline:      honest clocks; Alice funds and vanishes; Bob locks, bitcoind's
//               mock time opens his refund and he takes his BTC back; Alice
//               returns, does NOT claim (no secret leak) and her DIN stays
//               locked until T_din (dinerod has no mock time; the node refuses
//               the early refund as non-final).
// stale-clocks: both parties' clocks run 100 h behind the chains, so by chain
//               time Alice's DIN refund is already open. Bob must abort WITHOUT
//               locking BTC (else Alice could refund DIN and also take his
//               BTC); Alice then refunds her DIN on chain.
#include "wallet/swap/runner.h"
#include "wallet/swap/tower.h"

#include "bech32/bech32.hpp"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "rpc_client.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace dinero;
using namespace dinero::swap;

namespace {

constexpr uint32_t kHour = 3600;
constexpr uint64_t kDinAmount = 10 * 100'000'000ULL;  // 10 DIN
constexpr uint64_t kBtcAmount = 1'000'000;            // 0.01 BTC

int g_failures = 0;
void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  OK   " : "  FAIL ") << what << std::endl;
    if (!ok) ++g_failures;
}

Bytes32 Scalar(uint8_t s) { Bytes32 a{}; a.back() = s; return a; }

Bytes32 XOnly(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_keypair kp;
    const auto s = Scalar(scalar);
    secp256k1_keypair_create(secp, &kp, s.data());
    secp256k1_xonly_pubkey x;
    secp256k1_keypair_xonly_pub(secp, &x, nullptr, &kp);
    Bytes32 out{};
    secp256k1_xonly_pubkey_serialize(secp, out.data(), &x);
    return out;
}

std::array<uint8_t, 33> Compressed(uint8_t scalar) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_pubkey pk;
    const auto s = Scalar(scalar);
    secp256k1_ec_pubkey_create(secp, &pk, s.data());
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(secp, out.data(), &len, &pk, SECP256K1_EC_COMPRESSED);
    return out;
}

std::vector<uint8_t> P2tr(uint8_t scalar) {
    const auto x = XOnly(scalar);
    std::vector<uint8_t> spk{0x51, 0x20};
    spk.insert(spk.end(), x.begin(), x.end());
    return spk;
}

std::string Hex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}

// RpcClient returns the whole JSON-RPC envelope; watchers want the result.
// Errors of state-changing calls are printed: they are the interesting ones.
std::function<std::optional<Json::Value>(const std::string&, const Json::Value&)> MakeRpc(
    std::shared_ptr<rpc::RpcClient> client, std::string chain) {
    return [client, chain](const std::string& m, const Json::Value& p) -> std::optional<Json::Value> {
        auto r = client->call(m, p);
        const bool write = m == "sendrawtransaction" || m == "sendtoaddress" || m == "wallet.sendtoaddress";
        if (!r || !r->isMember("result") || (r->isMember("error") && !(*r)["error"].isNull())) {
            if (write || std::getenv("SWAP_E2E_DEBUG")) {
                std::cout << "    [" << chain << " " << m << " error] "
                          << (r && r->isMember("error") ? (*r)["error"].toStyledString() : client->get_last_error())
                          << std::flush;
            }
            return std::nullopt;
        }
        const Json::Value& res = (*r)["result"];
        if (write && res.isObject() && res.isMember("error") && !res["error"].isNull()) {
            std::cout << "    [" << chain << " " << m << " in-band error] " << res["error"].toStyledString() << std::flush;
        }
        return res;
    };
}

struct Party {
    std::string name;
    SwapKeys keys;
    int64_t clock_offset{0};
    std::string store_path;
    PlaintextFileSwapStore store;
    std::unique_ptr<RpcSwapChainIo> io;
    std::unique_ptr<SwapRunner> runner;
    std::vector<ActionKind> history;
    bool restarted{false};

    Party(std::string n, SwapKeys k, std::string path) : name(std::move(n)), keys(k), store_path(path), store(path) {}
    uint32_t Now() const { return static_cast<uint32_t>(int64_t(std::time(nullptr)) + clock_offset); }
    bool Terminal() const {
        const auto st = runner->session().record.state;
        return st == SwapState::Done || st == SwapState::Refunded || st == SwapState::Aborted || st == SwapState::Lost;
    }
    SwapState State() const { return runner->session().record.state; }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 9 && argc != 10) {
        std::cerr << "usage: see file header\n";
        return 2;
    }
    const std::string scenario = argv[1], dir = argv[2];
    const bool happy = scenario == "happy";
    const bool stale = scenario == "stale-clocks";
    const bool tower_claim = scenario == "tower-claim", tower_refund = scenario == "tower-refund";
    const bool use_tower = tower_claim || tower_refund;
    const bool race_late_reveal = scenario == "race-late-reveal";
    const bool race_overtaken = scenario == "race-refund-overtaken";
    const bool race_reorg = scenario == "race-reorg";
    const bool din_race = scenario == "din-race";
    const bool known = happy || stale || use_tower || scenario == "offline" || race_late_reveal || race_overtaken ||
                       race_reorg || din_race;
    if (!known) return 2;
    const std::string inbox = argc == 10 ? argv[9] : "";
    if (use_tower && inbox.empty()) return 2;
    // Separate payout keys per scenario, so a balance can only come from this run.
    const uint8_t kb = happy ? 7 : stale ? 17 : scenario == "offline" ? 27 : tower_claim ? 37 : tower_refund ? 47
                     : race_late_reveal ? 57 : race_overtaken ? 67 : race_reorg ? 77 : 87;
    const uint8_t kAliceBtcClaim = kb, kBobDinClaim = kb + 1, kAliceDinRefund = kb + 2, kBobBtcRefund = kb + 3;
    auto din_client = std::make_shared<rpc::RpcClient>("127.0.0.1", uint16_t(std::stoi(argv[3])), "test", "test");
    auto btc_client = std::make_shared<rpc::RpcClient>("127.0.0.1", uint16_t(std::stoi(argv[4])), argv[5], argv[6]);
    const DinRpc din = MakeRpc(din_client, "DIN");
    const BtcRpc btc = MakeRpc(btc_client, "BTC");
    const std::string din_miner = argv[7], btc_miner = argv[8];

    // dinerod rate-limits RPC per IP (50 req/s): pace rounds like a polling
    // wallet would, and retry a refused mining call.
    auto mine = [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        auto gen = [&](rpc::RpcClient& client, const std::string& addr, const char* chain) {
            Json::Value p(Json::arrayValue);
            p.append(1);
            p.append(addr);
            for (int attempt = 0; attempt < 10; ++attempt) {
                const auto r = client.call("generatetoaddress", p);
                if (r && r->isMember("result") && !(*r)["result"].isNull()) return;
                std::cout << "    [" << chain << " mining retry] " << client.get_last_error() << "\n";
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            throw std::runtime_error(std::string(chain) + " mining failed");
        };
        gen(*din_client, din_miner, "DIN");
        gen(*btc_client, btc_miner, "BTC");
    };

    try {
        // Lock enforcement must be live on this regtest node, or the run proves nothing.
        const auto info = din("getblockchaininfo", Json::Value(Json::arrayValue));
        Check(info && (*info)["contextual_locks_active"].asBool(), "dinerod enforces transaction locks");

        // ---- Offer and accept (fixed test keys; secret differs per scenario) ----
        const uint32_t real_now = static_cast<uint32_t>(std::time(nullptr));
        // stale-clocks: the parties' clocks say "100 h ago", so for the chains both
        // refund locks are already in the past.
        const int64_t offset = stale ? -100 * int64_t(kHour) : 0;
        // Honest scenarios start from max(wall clock, Bitcoin MTP): earlier scenarios
        // may have mocked bitcoind's clock forward, and chain time never goes back.
        const auto bci = btc("getblockchaininfo", Json::Value(Json::arrayValue));
        if (!bci) throw std::runtime_error("bitcoind unreachable");
        const uint32_t btc_mtp_now = (*bci)["mediantime"].asUInt();
        const uint32_t base = stale ? static_cast<uint32_t>(int64_t(real_now) + offset)
                                    : std::max(real_now, btc_mtp_now + 60);
        Bytes32 secret{};
        secret.fill(static_cast<uint8_t>(0x50 + kb));

        SwapOffer o;
        o.network = SwapNetwork::Regtest;
        o.din_amount_una = kDinAmount;
        o.btc_amount_sat = kBtcAmount;
        crypto::CSHA256().Write(secret.data(), secret.size()).Finalize(o.payment_hash.data());
        o.din_refund_pubkey = XOnly(3);
        o.btc_claim_pubkey = Compressed(4);
        o.expires_unix = base + 1 * kHour;
        o.t_btc_unix = base + 48 * kHour;
        o.t_din_unix = base + 96 * kHour;
        o.n_din_confirmations = kMinDinConfirmations;
        o.n_btc_confirmations = 1;
        o = DecodeOffer(EncodeOffer(o));  // as Bob receives it
        RequireAcceptableNow(o, base);
        SwapAccept a;
        a.offer_id = OfferId(o);
        a.din_claim_pubkey = XOnly(5);
        a.btc_refund_pubkey = Compressed(6);
        a = DecodeAccept(EncodeAccept(a));  // as Alice receives it

        const auto btc_tip = btc("getblockcount", Json::Value(Json::arrayValue));
        if (!btc_tip) throw std::runtime_error("bitcoind unreachable");

        RunnerConfig config;
        config.din_hrp = "rdin";
        config.btc_hrp = "bcrt";
        config.use_tower = use_tower;

        auto make_session = [&](Role role) {
            SwapSession s;
            s.record.role = role;
            s.record.offer = o;
            s.record.accept = a;
            s.record.state_since_unix = base;
            if (role == Role::DinSeller) s.record.secret = secret;
            s.btc_scan_from_height = btc_tip->asUInt();
            // Alice: DIN refund -> key 9, BTC claim -> key 7. Bob: DIN claim -> key 8, BTC refund -> key 10.
            s.din_payout_script = P2tr(role == Role::DinSeller ? kAliceDinRefund : kBobDinClaim);
            s.btc_payout_script = P2tr(role == Role::DinSeller ? kAliceBtcClaim : kBobBtcRefund);
            return s;
        };

        Party alice("alice", SwapKeys{Scalar(3), Scalar(4)}, dir + "/alice-" + scenario + ".swap");
        Party bob("bob", SwapKeys{Scalar(5), Scalar(6)}, dir + "/bob-" + scenario + ".swap");
        alice.clock_offset = bob.clock_offset = offset;
        alice.store.Save(make_session(Role::DinSeller));
        bob.store.Save(make_session(Role::BtcSeller));

        // (Re)build a party from its store file only, as after a crash.
        auto boot = [&](Party& p) {
            p.runner.reset();
            p.io.reset();
            const auto s = PlaintextFileSwapStore::Load(p.store_path);
            p.io = std::make_unique<RpcSwapChainIo>(din, btc, s, config);
            if (use_tower && s.record.role == Role::BtcSeller) {
                p.io->SetTowerSink([&](const std::string& package) {
                    std::cout << "        package -> " << WriteTowerInbox(inbox, package) << "\n";
                });
            }
            p.runner = std::make_unique<SwapRunner>(s, p.keys, config, *p.io, p.store);
        };
        boot(alice);
        boot(bob);

        auto tick = [&](Party& p, int round) {
            const auto r = p.runner->Tick(p.Now());
            p.history.insert(p.history.end(), r.actions.begin(), r.actions.end());
            if (r.before != r.after || !r.events.empty() || !r.observed) {
                std::cout << "  [" << round << "] " << p.name << ": " << StateName(r.before) << " -> "
                          << StateName(r.after) << (r.observed ? "" : " (not observed)") << "\n";
                for (const auto& e : r.events) std::cout << "        " << e << "\n";
            }
        };
        auto din_balance = [&](uint8_t key) -> int64_t {
            Json::Value p(Json::arrayValue);
            const auto x = XOnly(key);
            p.append(bech32::Encode("rdin", 1, std::vector<uint8_t>(x.begin(), x.end()), bech32::Encoding::BECH32M));
            const auto r = din("getaddressbalance", p);
            return r && (*r)["confirmed"].isNumeric() ? (*r)["confirmed"].asInt64() : -1;
        };
        auto btc_balance = [&](uint8_t key) -> int64_t {
            Json::Value desc(Json::arrayValue);
            desc.append("raw(" + Hex(P2tr(key)) + ")");
            Json::Value p(Json::arrayValue);
            p.append("start");
            p.append(desc);
            const auto r = btc("scantxoutset", p);
            return r ? std::llround((*r)["total_amount"].asDouble() * 1e8) : -1;
        };
        const auto alive = [&](Party& p) { return !p.Terminal(); };
        // Read the raw envelope: dinerod's success result is an object, so only an
        // explicit "non-final" refusal counts.
        auto din_refuses_as_non_final = [&](const std::vector<uint8_t>& raw) {
            Json::Value p(Json::arrayValue);
            p.append(Hex(raw));
            const auto env = din_client->call("sendrawtransaction", p);
            const std::string text = (env ? env->toStyledString() : std::string()) + din_client->get_last_error();
            const bool refused = !env || (env->isMember("error") && !(*env)["error"].isNull()) ||
                                 ((*env)["result"].isObject() && (*env)["result"].isMember("error"));
            std::cout << "    node said: " << text << "\n";
            return refused && text.find("non-final") != std::string::npos;
        };
        auto mine_btc = [&](int n) {
            Json::Value p(Json::arrayValue);
            p.append(n);
            p.append(btc_miner);
            if (!btc("generatetoaddress", p)) throw std::runtime_error("BTC mining failed");
        };
        // Blocks WITHOUT mempool transactions: Bitcoin time moves, pending spends stay pending.
        auto mine_btc_empty = [&](int n) {
            for (int i = 0; i < n; ++i) {
                Json::Value p(Json::arrayValue);
                p.append(btc_miner);
                p.append(Json::Value(Json::arrayValue));
                if (!btc("generateblock", p)) throw std::runtime_error("generateblock failed");
            }
        };
        auto mock_btc_past_t_btc = [&] {
            Json::Value mp(Json::arrayValue);
            mp.append(Json::Int64(o.t_btc_unix) + 3600);
            if (!btc("setmocktime", mp)) throw std::runtime_error("setmocktime failed");
        };
        auto display_txid = [](const BtcTx& tx) {
            const auto w = BtcTxid(tx);
            return Hex(std::vector<uint8_t>(w.rbegin(), w.rend()));
        };
        // Which transaction currently spends the BTC HTLC in the mempool (if any).
        auto btc_mempool_spender = [&](const BtcFunding& f) -> std::string {
            Json::Value op(Json::objectValue);
            op["txid"] = Hex(std::vector<uint8_t>(f.txid.rbegin(), f.txid.rend()));
            op["vout"] = f.vout;
            Json::Value list(Json::arrayValue);
            list.append(op);
            Json::Value p(Json::arrayValue);
            p.append(list);
            const auto r = btc("gettxspendingprevout", p);
            return r && r->isArray() && !r->empty() ? (*r)[0]["spendingtxid"].asString() : std::string();
        };
        auto never = [](const Party& p, ActionKind k) {
            return std::find(p.history.begin(), p.history.end(), k) == p.history.end();
        };

        if (happy) {
            bool early_refund_checked = false;
            bool rbf_probed = false, rbf_replaced = false;
            for (int round = 0; round < 120 && (alive(alice) || alive(bob)); ++round) {
                if (alive(alice)) tick(alice, round);
                if (alive(bob)) tick(bob, round);

                if (!early_refund_checked && alice.State() == SwapState::DinLocked) {
                    // T_din is 96 h away: the node itself must refuse Alice's refund.
                    const auto d = alice.io->ObserveDin();
                    if (d.funding) {
                        const auto raw = SignedDinRefund(alice.runner->session(), alice.keys, *d.funding, config.din_fee_una);
                        Check(din_refuses_as_non_final(raw), "dinerod refuses the DIN refund before its timestamp lock");
                        early_refund_checked = true;
                    }
                }
                if (!rbf_probed && bob.State() == SwapState::DinClaimBroadcast) {
                    // Probe (reported, not asserted): does dinerod replace an unconfirmed
                    // swap spend by fee? The watchtower's DIN ladder depends on it.
                    const auto d = bob.io->ObserveDin();
                    if (d.funding) {
                        Json::Value p(Json::arrayValue);
                        p.append(Hex(SignedDinClaim(bob.runner->session(), bob.keys, *d.funding, 2 * config.din_fee_una)));
                        const auto env = din_client->call("sendrawtransaction", p);
                        const std::string text = env ? env->toStyledString() : din_client->get_last_error();
                        rbf_replaced = env && (!env->isMember("error") || (*env)["error"].isNull()) &&
                                       !((*env)["result"].isObject() && (*env)["result"].isMember("error"));
                        std::cout << "  RBF PROBE: double-fee DIN claim " << (rbf_replaced ? "REPLACED" : "REFUSED")
                                  << " by dinerod: " << text << "\n";
                        rbf_probed = true;
                    }
                }
                if (!alice.restarted && alice.State() == SwapState::DinLocked) {
                    std::cout << "  [" << round << "] alice: restart from store\n";
                    boot(alice);
                    alice.restarted = true;
                }
                if (!bob.restarted && bob.State() == SwapState::BtcLocked) {
                    std::cout << "  [" << round << "] bob: restart from store\n";
                    boot(bob);
                    bob.restarted = true;
                }
                mine();
            }
            Check(early_refund_checked, "early DIN refund was attempted");
            Check(alice.restarted && bob.restarted, "both runners were restarted from their stores mid-swap");
            Check(alice.State() == SwapState::Done, std::string("alice Done (is ") + StateName(alice.State()) + ")");
            Check(bob.State() == SwapState::Done, std::string("bob Done (is ") + StateName(bob.State()) + ")");
            Check(btc_balance(kAliceBtcClaim) == int64_t(kBtcAmount - config.btc_fee_sat), "alice received BTC minus fee on chain");
            Check(din_balance(kBobDinClaim) ==
                      int64_t(kDinAmount - (rbf_replaced ? 2 : 1) * config.din_fee_una), "bob received DIN minus fee on chain");
        } else if (use_tower) {
            // Bob locks BTC, arms the tower, and goes offline for good.
            int round = 0;
            for (; round < 60 && !(bob.State() == SwapState::BtcLocked && bob.runner->session().tower_armed); ++round) {
                // tower-refund: Alice funds and then stays silent; tower-claim: she plays on.
                if (tower_claim || alice.State() != SwapState::DinLocked) tick(alice, round);
                tick(bob, round);
                mine();
            }
            Check(bob.runner->session().tower_armed, "bob armed the watchtower");
            std::cout << "  bob goes offline\n";
            bob.runner.reset();
            bob.io.reset();
            const auto id = OfferId(o);
            const std::string pkg = inbox + "/" + Hex(std::vector<uint8_t>(id.begin(), id.begin() + 8)) + ".pkg";
            auto read_package = [&]() -> std::optional<TowerPackage> {
                for (const std::string& path : {pkg, pkg + ".done"}) {
                    std::ifstream in(path);
                    if (!in) continue;
                    std::stringstream t;
                    t << in.rdbuf();
                    return DecodeTowerPackage(t.str());
                }
                return std::nullopt;
            };
            const auto package = read_package();
            Check(package.has_value(), "the tower's inbox holds a valid package");
            auto tower_done = [&] { return std::ifstream(pkg + ".done").good(); };
            if (tower_claim) {
                // Alice claims BTC (revealing the secret); only the tower can take Bob's DIN.
                for (; round < 120 && !tower_done(); ++round) {
                    if (alive(alice)) tick(alice, round);
                    mine();
                }
                Check(alice.State() == SwapState::Done, std::string("alice Done (is ") + StateName(alice.State()) + ")");
                Check(tower_done(), "the tower settled the swap");
                const int64_t got = din_balance(kBobDinClaim);
                bool rung_amount = false;
                if (package) for (const auto& r : package->din_claims) rung_amount |= got == int64_t(kDinAmount - r.fee_una);
                Check(rung_amount, "bob received his DIN through a tower rung (" + std::to_string(got) + " una)");
                Check(btc_balance(kAliceBtcClaim) == int64_t(kBtcAmount - config.btc_fee_sat), "alice received BTC on chain");
                std::cout << "  bob comes back\n";
                boot(bob);
                for (int i = 0; i < 12 && alive(bob); ++i, ++round) {
                    tick(bob, round);
                    mine();
                }
                Check(bob.State() == SwapState::Done, std::string("returning bob reconciles to Done (is ") + StateName(bob.State()) + ")");
            } else {
                // Alice vanishes too; Bitcoin time passes T_btc and only the tower can refund Bob.
                std::cout << "  alice goes offline; bitcoind: mock time to T_btc + 1 h\n";
                mock_btc_past_t_btc();
                mine_btc(12);
                // No Bitcoin blocks for a while: the tower must bump its refund, and
                // the higher rung must replace the lower one in the mempool.
                std::this_thread::sleep_for(std::chrono::seconds(9));
                const std::string spender = package ? btc_mempool_spender([&] {
                    BtcFunding f;
                    f.txid = package->btc_refunds[0].tx.vin[0].prev_txid;
                    f.vout = package->btc_refunds[0].tx.vin[0].prev_vout;
                    return f;
                }()) : "";
                bool bumped = false;
                if (package) {
                    for (size_t i = 1; i < package->btc_refunds.size(); ++i) {
                        bumped |= spender == display_txid(package->btc_refunds[i].tx);
                    }
                }
                Check(bumped && spender != display_txid(package->btc_refunds[0].tx),
                      "a higher BTC refund rung replaced rung 0 in the mempool (" + spender.substr(0, 16) + ")");
                for (; round < 120 && !tower_done(); ++round) mine();
                Check(tower_done(), "the tower settled the swap");
                const int64_t got = btc_balance(kBobBtcRefund);
                bool rung_amount = false;
                if (package) for (const auto& r : package->btc_refunds) rung_amount |= got == int64_t(kBtcAmount - r.fee_sat);
                Check(rung_amount, "bob got his BTC back through a tower rung (" + std::to_string(got) + " sat)");
                std::cout << "  bob comes back\n";
                boot(bob);
                for (int i = 0; i < 12 && alive(bob); ++i, ++round) {
                    tick(bob, round);
                    mine();
                }
                Check(bob.State() == SwapState::Refunded,
                      std::string("returning bob reconciles to Refunded (is ") + StateName(bob.State()) + ")");
            }
        } else if (race_late_reveal) {
            // Alice's claim is still in the BTC mempool when Bitcoin time passes
            // T_btc. Bob must take the DIN with the secret, never race a refund.
            int round = 0;
            bool pushed = false;
            for (; round < 120 && (alive(alice) || alive(bob)); ++round) {
                if (alive(alice)) tick(alice, round);
                if (!pushed && alice.State() == SwapState::BtcClaimBroadcast) {
                    std::cout << "  [" << round << "] claim pending; bitcoind: mock time past T_btc, 12 EMPTY blocks\n";
                    mock_btc_past_t_btc();
                    mine_btc_empty(12);
                    const auto b = bob.io->ObserveBtc();
                    Check(b.mtp_unix >= o.t_btc_unix && b.htlc.spent && b.htlc.spend_confirmations == 0,
                          "Bitcoin MTP passed T_btc with Alice's claim still unconfirmed");
                    pushed = true;
                }
                if (alive(bob)) tick(bob, round);
                mine();
            }
            Check(pushed, "the late-reveal window was created");
            Check(never(bob, ActionKind::RefundBtc), "bob never broadcast a BTC refund against a revealed secret");
            Check(alice.State() == SwapState::Done && bob.State() == SwapState::Done, "both Done");
            Check(btc_balance(kAliceBtcClaim) == int64_t(kBtcAmount - config.btc_fee_sat), "alice received BTC on chain");
            Check(din_balance(kBobDinClaim) == int64_t(kDinAmount - config.din_fee_una), "bob received DIN on chain");
        } else if (race_overtaken) {
            // Bob's refund is in the mempool; a late, higher-fee Alice claim (made
            // outside the client, which refuses) replaces it. Bob must notice the
            // secret and claim the DIN.
            int round = 0;
            for (; round < 60 && bob.State() != SwapState::BtcLocked; ++round) {
                if (alice.State() != SwapState::DinLocked) tick(alice, round);  // funds, then silent
                tick(bob, round);
                mine();
            }
            std::cout << "  alice goes quiet; bitcoind: mock time past T_btc\n";
            mock_btc_past_t_btc();
            mine_btc(12);
            tick(bob, round);
            Check(bob.State() == SwapState::BtcRefundBroadcast, "bob broadcast his BTC refund");
            const auto b = bob.io->ObserveBtc();
            Check(b.funding.has_value(), "BTC HTLC known");
            const std::string refund_txid = b.funding ? btc_mempool_spender(*b.funding) : "";
            tick(alice, round);
            Check(never(alice, ActionKind::ClaimBtc), "alice's client refuses to claim after her cut-off");
            if (b.funding) {
                const auto late = SignedBtcClaim(alice.runner->session(), alice.keys, *b.funding, 5'000);
                Json::Value p(Json::arrayValue);
                p.append(Hex(late));
                const auto env = btc_client->call("sendrawtransaction", p);
                std::cout << "    late claim -> " << (env ? env->toStyledString() : btc_client->get_last_error());
                const std::string now_spender = btc_mempool_spender(*b.funding);
                Check(!refund_txid.empty() && now_spender == display_txid(ParseBtcTx(late)) && now_spender != refund_txid,
                      "the higher-fee claim replaced bob's refund in the mempool");
            }
            for (++round; round < 120 && alive(bob); ++round) {
                tick(bob, round);
                mine();
            }
            Check(bob.State() == SwapState::Done, std::string("bob took the DIN instead (is ") + StateName(bob.State()) + ")");
            Check(din_balance(kBobDinClaim) == int64_t(kDinAmount - config.din_fee_una), "bob received DIN on chain");
            Check(btc_balance(kAliceBtcClaim) == int64_t(kBtcAmount - 5'000), "the late claim paid alice");
        } else if (race_reorg) {
            // Alice's confirmed claim is reorganised out after T_btc. Nobody may
            // treat the swap as over: Bob must not refund, and the claim re-confirms.
            int round = 0;
            std::string claim_block;
            for (; round < 120 && (alive(alice) || alive(bob)); ++round) {
                if (alive(alice)) tick(alice, round);
                if (alive(bob)) tick(bob, round);
                const bool claim_pending = claim_block.empty() && alice.State() == SwapState::BtcClaimBroadcast;
                mine();
                if (claim_pending) {
                    const auto h = btc("getbestblockhash", Json::Value(Json::arrayValue));
                    claim_block = h ? h->asString() : "";
                    const auto a = alice.io->ObserveBtc();
                    Check(a.htlc.spent && a.htlc.spend_confirmations == 1, "alice's claim is 1 block deep");
                    tick(alice, round);  // she sees it 1 deep and must not call it settled
                    Check(alice.State() != SwapState::Done, "alice did not consider a 1-block claim settled");
                    std::cout << "  [" << round << "] reorg: mock time past T_btc, invalidate the claim block, 12 EMPTY blocks\n";
                    mock_btc_past_t_btc();
                    Json::Value p(Json::arrayValue);
                    p.append(claim_block);
                    if (!btc("invalidateblock", p)) throw std::runtime_error("invalidateblock failed");
                    mine_btc_empty(12);
                    const auto after = alice.io->ObserveBtc();
                    Check(after.mtp_unix >= o.t_btc_unix && after.htlc.spent && after.htlc.spend_confirmations == 0,
                          "after the reorg the claim is unconfirmed and Bob's refund is open");
                }
            }
            Check(!claim_block.empty(), "the reorg happened");
            Check(never(bob, ActionKind::RefundBtc), "bob never refunded after the secret was public");
            Check(alice.State() == SwapState::Done && bob.State() == SwapState::Done, "both Done");
            Check(btc_balance(kAliceBtcClaim) == int64_t(kBtcAmount - config.btc_fee_sat), "alice received BTC on chain");
        } else if (din_race) {
            // DIN claim and refund in the same window, both orderings, after T_din:
            // Dinero keeps the first seen (no replace-by-fee); the block decides.
            for (int order = 0; order < 2; ++order) {
                SwapOffer od = o;
                const uint32_t past = real_now - 100 * kHour;
                od.expires_unix = past;
                od.t_btc_unix = past + 12 * kHour;
                od.t_din_unix = past + 36 * kHour;  // already passed by Dinero's MTP
                Bytes32 sec{};
                sec.fill(static_cast<uint8_t>(0xa0 + order));
                crypto::CSHA256().Write(sec.data(), sec.size()).Finalize(od.payment_hash.data());
                od = DecodeOffer(EncodeOffer(od));
                SwapAccept ad = a;
                ad.offer_id = OfferId(od);
                SwapSession as;
                as.record.role = Role::DinSeller;
                as.record.offer = od;
                as.record.accept = ad;
                as.record.secret = sec;
                as.btc_scan_from_height = btc_tip->asUInt();
                as.din_payout_script = P2tr(kAliceDinRefund);
                as.btc_payout_script = P2tr(kAliceBtcClaim);
                SwapSession bs = as;
                bs.record.role = Role::BtcSeller;  // Bob, having learned the secret
                bs.din_payout_script = P2tr(kBobDinClaim);
                RpcSwapChainIo io(din, btc, as, config);
                Json::Value fp(Json::objectValue);
                fp["address"] = DinHtlcAddressFor(as.record, "rdin");
                fp["amount_una"] = Json::UInt64(kDinAmount);
                if (!din("wallet.sendtoaddress", fp)) throw std::runtime_error("funding failed");
                mine();
                const auto d = io.ObserveDin();
                Check(d.funding.has_value(), "race HTLC funded");
                if (!d.funding) continue;
                const auto claim = SignedDinClaim(bs, bob.keys, *d.funding, config.din_fee_una);
                const auto refund = SignedDinRefund(as, alice.keys, *d.funding, config.din_fee_una);
                const auto& first = order == 0 ? claim : refund;
                const auto& second = order == 0 ? refund : claim;
                Json::Value p1(Json::arrayValue), p2(Json::arrayValue);
                p1.append(Hex(first));
                p2.append(Hex(second));
                const auto e1 = din_client->call("sendrawtransaction", p1);
                const auto e2 = din_client->call("sendrawtransaction", p2);
                const std::string t1 = e1 ? e1->toStyledString() : din_client->get_last_error();
                const std::string t2 = e2 ? e2->toStyledString() : din_client->get_last_error();
                const char* name = order == 0 ? "claim-then-refund" : "refund-then-claim";
                Check(t1.find("error") == std::string::npos || t1.find("\"error\" : null") != std::string::npos,
                      std::string(name) + ": the first spend is accepted");
                Check(t2.find("conflict") != std::string::npos, std::string(name) + ": the second is refused as a conflict");
                mine();
                const auto after = io.ObserveDin();
                Check(after.htlc.spent && after.htlc.spent_by_claim == (order == 0),
                      std::string(name) + ": the watcher reports the first one as the winner");
            }
        } else if (stale) {
            for (int round = 0; round < 40 && (alive(alice) || alive(bob)); ++round) {
                if (alive(alice)) tick(alice, round);
                if (alive(bob)) tick(bob, round);
                mine();
            }
            Check(bob.State() == SwapState::Aborted, std::string("bob Aborted (is ") + StateName(bob.State()) + ")");
            Check(std::find(bob.history.begin(), bob.history.end(), ActionKind::FundBtcHtlc) == bob.history.end(),
                  "bob never locked BTC against a DIN lock whose refund was already open");
            Check(alice.State() == SwapState::Refunded, std::string("alice Refunded (is ") + StateName(alice.State()) + ")");
            Check(din_balance(kAliceDinRefund) == int64_t(kDinAmount - config.din_fee_una), "alice got her DIN back minus fee on chain");
        } else {
            // Phase 1: Alice funds the DIN lock, then goes offline.
            int round = 0;
            for (; round < 20 && alice.State() != SwapState::DinLocked; ++round) {
                tick(alice, round);
                mine();
            }
            Check(alice.State() == SwapState::DinLocked, "alice locked DIN");
            std::cout << "  alice goes offline\n";
            alice.runner.reset();
            alice.io.reset();
            // Phase 2: Bob locks BTC after the DIN lock is deep, then refunds when T_btc passes.
            for (; round < 120 && alive(bob); ++round) {
                tick(bob, round);
                if (!bob.restarted && bob.State() == SwapState::BtcLocked) {
                    std::cout << "  [" << round << "] bob: restart from store\n";
                    boot(bob);
                    bob.restarted = true;
                    // Bitcoin time passes T_btc: mock bitcoind's clock and let MTP catch up.
                    std::cout << "  [" << round << "] bitcoind: mock time to T_btc + 1 h, mine 12 blocks\n";
                    Json::Value mp(Json::arrayValue);
                    mp.append(Json::Int64(o.t_btc_unix) + 3600);
                    if (!btc("setmocktime", mp)) throw std::runtime_error("setmocktime failed");
                    mine_btc(12);
                }
                mine();
            }
            Check(bob.State() == SwapState::Refunded, std::string("bob Refunded (is ") + StateName(bob.State()) + ")");
            Check(btc_balance(kBobBtcRefund) == int64_t(kBtcAmount - config.btc_fee_sat), "bob got his BTC back minus fee on chain");
            // Phase 3: Alice returns. Her claim would reveal the secret for nothing.
            std::cout << "  alice comes back\n";
            boot(alice);
            alice.restarted = true;
            for (int i = 0; i < 5; ++i, ++round) {
                tick(alice, round);
                mine();
            }
            Check(std::find(alice.history.begin(), alice.history.end(), ActionKind::ClaimBtc) == alice.history.end(),
                  "alice never broadcast a claim on the refunded BTC (secret not leaked)");
            Check(alice.State() == SwapState::DinLocked,
                  std::string("alice waits for T_din with her DIN locked (is ") + StateName(alice.State()) + ")");
            const auto d = alice.io->ObserveDin();
            Check(d.funding && !d.htlc.spent, "alice's DIN lock is unspent");
            if (d.funding) {
                Check(din_refuses_as_non_final(SignedDinRefund(alice.runner->session(), alice.keys, *d.funding,
                                                               config.din_fee_una)),
                      "dinerod refuses alice's DIN refund until T_din");
            }
        }
        for (Party* p : {&alice, &bob}) {  // from the store: a party may be offline (no runner)
            std::cout << "  " << p->name << " final record: "
                      << StateName(PlaintextFileSwapStore::Load(p->store_path).record.state) << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << "  FAIL exception: " << e.what() << "\n";
        ++g_failures;
    }
    std::cout << "E2E " << scenario << ": " << (g_failures ? "FAIL" : "PASS") << std::endl;
    return g_failures ? 1 : 0;
}

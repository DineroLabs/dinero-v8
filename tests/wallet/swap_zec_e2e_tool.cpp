// Two-chain end-to-end DIN <-> ZEC swap on regtest (tests/wallet/swap_zec_e2e_regtest.sh):
// Alice (sells DIN) and Bob (sells ZEC) each run their own SwapRunner, with
// their own encrypted store, against one dinerod and one Zebra. Bob pays ZEC
// into his per-swap deposit address first; his engine locks from it. Between
// ticks both chains mine a block (Zebra's stamped with the wall clock, so
// Zcash time follows real time as it does on mainnet). Each runner is rebuilt
// from its store once mid-swap. Outcomes are checked on chain.
//   swap_zec_e2e_tool <scenario> <workdir> <din_rpcport> <zebra_rpcport> <din_miner_address> <zebra_mine.py>
//                     [<zebra faketime file>]
// The faketime file is libfaketime's FAKETIME_TIMESTAMP_FILE for zebrad (see
// the script): writing "+49h" moves Zebra's clock, so blocks can be stamped
// past T_zec and Bob's refund mined. Without it the refund steps are skipped.
// happy:        both reach Done; Alice receives ZEC, Bob receives DIN.
// tower-claim:  Bob locks ZEC, arms his (in-process) watchtower and goes
//               offline; Alice claims ZEC; the tower learns the secret from
//               Zcash and claims Bob's DIN with a pre-signed rung.
// bob-silent:   Bob never locks; Alice's DIN stays locked: dinerod refuses her
//               refund before T_din (no mock time on dinerod).
// alice-silent: Alice funds DIN and vanishes after Bob locks ZEC; Zebra
//               refuses Bob's refund before T_zec; then Zebra's clock moves past
//               T_zec and Bob gets his ZEC back (his wallet's refund or his
//               tower's rung, whichever lands first).
// tower-refund: Alice silent, Bob offline after arming his tower; past T_zec
//               the tower's never-expiring rung (signed hours earlier) is mined.
#include "wallet/swap/encrypted_store.h"
#include "wallet/swap/runner.h"
#include "wallet/swap/swap_crypto.h"
#include "wallet/swap/tower.h"
#include "wallet/swap/zec_chain_io.h"

#include "bech32/bech32.hpp"
#include "crypto/sha256.h"
#include "rpc_client.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace dinero;
using namespace dinero::swap;

namespace {

constexpr uint32_t kHour = 3600;
constexpr uint64_t kDinAmount = 10 * 100'000'000ULL;  // 10 DIN
constexpr uint64_t kZecAmount = 1'000'000;            // 0.01 ZEC
constexpr uint64_t kZecSpendFee = 20'000;             // claim / refund: 2 x conventional
constexpr uint8_t kZebraMiner = 7;                    // the zebrad miner_address key (script)

int g_failures = 0;
void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  OK   " : "  FAIL ") << what << std::endl;
    if (!ok) ++g_failures;
}

Bytes32 Scalar(uint8_t s) { Bytes32 a{}; a.back() = s; return a; }
Bytes32 XOnly(uint8_t s) { return detail::XOnlyOf(Scalar(s)); }

std::vector<uint8_t> P2tr(uint8_t scalar) {
    const auto x = XOnly(scalar);
    std::vector<uint8_t> spk{0x51, 0x20};
    spk.insert(spk.end(), x.begin(), x.end());
    return spk;
}

ZecTAddress ZecAddr(uint8_t scalar) { return ZecP2pkhAddressOf(detail::CompressedOf(Scalar(scalar)), ZecNetwork::Regtest); }

std::function<std::optional<Json::Value>(const std::string&, const Json::Value&)> MakeRpc(
    std::shared_ptr<rpc::RpcClient> client, std::string chain) {
    return [client, chain](const std::string& m, const Json::Value& p) -> std::optional<Json::Value> {
        auto r = client->call(m, p);
        const bool write = m == "sendrawtransaction" || m == "wallet.sendtoaddress";
        if (!r || !r->isMember("result") || (r->isMember("error") && !(*r)["error"].isNull())) {
            if (write || std::getenv("SWAP_E2E_DEBUG")) {
                std::cout << "    [" << chain << " " << m << " error] "
                          << (r && r->isMember("error") ? (*r)["error"].toStyledString() : client->get_last_error())
                          << std::flush;
            }
            return std::nullopt;
        }
        return (*r)["result"];
    };
}

// sendrawtransaction to Zebra, keeping Zebra's message for the may-have-left check.
std::function<std::string(const std::string&)> ZebraBroadcast(std::shared_ptr<rpc::RpcClient> client) {
    return [client](const std::string& hex) {
        Json::Value p(Json::arrayValue);
        p.append(hex);
        const auto r = client->call("sendrawtransaction", p);
        if (r && (*r)["result"].isString()) return (*r)["result"].asString();
        throw std::runtime_error(r && r->isMember("error") ? (*r)["error"]["message"].asString()
                                                           : "RPC failed: " + client->get_last_error());
    };
}

struct Party {
    std::string name;
    SwapKeys keys;
    std::string store_path;
    Bytes32 store_key;
    EncryptedFileSwapStore store;
    std::unique_ptr<ZecSwapChainIo> io;
    std::unique_ptr<SwapRunner> runner;
    bool restarted{false};

    Party(std::string n, SwapKeys k, std::string path, uint8_t master)
        : name(std::move(n)), keys(k), store_path(path),
          store_key(DeriveSwapStoreKey([&] { Bytes32 m{}; m.fill(master); return m; }())), store(path, store_key) {}
    static uint32_t Now() { return static_cast<uint32_t>(std::time(nullptr)); }
    SwapState State() const { return runner->session().record.state; }
    bool Terminal() const {
        const auto& ss = runner->session();
        const auto st = ss.record.state;
        // Bob's DIN still at his sweep key: not finished until swept to his wallet.
        if (st == SwapState::Done && ss.record.role == Role::BtcSeller && ss.din_sweep_pubkey != Bytes32{} &&
            !ss.din_swept) {
            return false;
        }
        return st == SwapState::Done || st == SwapState::Refunded || st == SwapState::Aborted || st == SwapState::Lost;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "miner-address") {  // for zebrad's miner_address
        std::cout << EncodeZecTAddress(ZecAddr(kZebraMiner)) << std::endl;
        return 0;
    }
    if (argc != 7 && argc != 8) {
        std::cerr << "usage: see file header\n";
        return 2;
    }
    const std::string scenario = argv[1], dir = argv[2], din_miner = argv[5], mine_py = argv[6];
    const bool happy = scenario == "happy", tower_claim = scenario == "tower-claim";
    const bool bob_silent = scenario == "bob-silent", alice_silent = scenario == "alice-silent";
    const bool tower_refund = scenario == "tower-refund";
    if (!happy && !tower_claim && !bob_silent && !alice_silent && !tower_refund) return 2;
    const bool use_tower = tower_claim || alice_silent || tower_refund;
    const std::string faketime_file = argc == 8 ? argv[7] : "";
    // Separate payout keys per scenario, so a balance can only come from this run.
    const uint8_t kb = happy ? 20 : tower_claim ? 30 : bob_silent ? 40 : alice_silent ? 50 : 60;
    const uint8_t kAliceZecClaim = kb, kBobDinClaim = kb + 1, kAliceDinRefund = kb + 2, kBobZecRefund = kb + 3;
    const uint8_t kDeposit = kb + 4, kBobSweep = kb + 5;
    const uint16_t zebra_port = static_cast<uint16_t>(std::stoi(argv[4]));
    auto din_client = std::make_shared<rpc::RpcClient>("127.0.0.1", uint16_t(std::stoi(argv[3])), "test", "test");
    auto zec_client = std::make_shared<rpc::RpcClient>("127.0.0.1", zebra_port, "", "");
    const DinRpc din = MakeRpc(din_client, "DIN");
    const ZecRpc zec = MakeRpc(zec_client, "ZEC");

    int64_t zec_clock_offset = 0;  // how far Zebra's (libfaketime) clock runs ahead of ours
    auto mine_zec = [&](int n) {
        for (int i = 0; i < n; ++i) {
            const std::string when = zec_clock_offset ? std::to_string(int64_t(std::time(nullptr)) + zec_clock_offset) : "now";
            const std::string cmd = "python3 '" + mine_py + "' " + std::to_string(zebra_port) + " 1 " + when + " >/dev/null";
            if (std::system(cmd.c_str()) != 0) throw std::runtime_error("Zebra mining failed");
        }
    };
    // Moves Zebra's clock `seconds` ahead (it accepts block times up to its
    // clock + 2 h) and mines until Zcash's median time is past `until`.
    auto jump_zebra_past = [&](uint32_t until) {
        const int64_t seconds = int64_t(until) - int64_t(std::time(nullptr)) + kHour;
        std::ofstream(faketime_file) << "+" << seconds << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
        zec_clock_offset = seconds;
        for (int i = 0; i < 30; ++i) {
            const auto c = ObserveZecChain(zec, "test");
            if (c && c->mtp_unix > until) return true;
            mine_zec(1);
        }
        return false;
    };
    auto mine = [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));  // dinerod rate-limits RPC per IP
        Json::Value p(Json::arrayValue);
        p.append(1);
        p.append(din_miner);
        for (int attempt = 0;; ++attempt) {
            const auto r = din_client->call("generatetoaddress", p);
            if (r && r->isMember("result") && !(*r)["result"].isNull()) break;
            if (attempt == 10) throw std::runtime_error("DIN mining failed");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        mine_zec(1);
    };
    auto din_balance = [&](uint8_t key) -> int64_t {
        Json::Value p(Json::arrayValue);
        const auto x = XOnly(key);
        p.append(bech32::Encode("rdin", 1, std::vector<uint8_t>(x.begin(), x.end()), bech32::Encoding::BECH32M));
        const auto r = din("getaddressbalance", p);
        return r && (*r)["confirmed"].isNumeric() ? (*r)["confirmed"].asInt64() : -1;
    };
    auto zec_balance = [&](uint8_t key) -> int64_t {
        const auto d = ObserveZecDeposit(zec, EncodeZecTAddress(ZecAddr(key)), "test");
        return d.ok ? int64_t(d.confirmed_zat(1)) : -1;
    };
    // Zebra's verdict on a transaction, without changing anything else.
    auto zebra_says = [&](const std::vector<uint8_t>& raw) {
        try {
            return "accepted " + ZebraBroadcast(zec_client)(detail::ToHex(raw));
        } catch (const std::exception& e) {
            return std::string("rejected ") + e.what();
        }
    };

    try {
        const auto info = din("getblockchaininfo", Json::Value(Json::arrayValue));
        Check(info && (*info)["contextual_locks_active"].asBool(), "dinerod enforces transaction locks");
        const auto chain = ObserveZecChain(zec, "test");
        if (!chain) throw std::runtime_error("Zebra unreachable");
        Check(chain->mtp_unix + 2 * kHour > Party::Now(), "Zcash time follows the wall clock");

        // ---- Offer and accept --------------------------------------------------
        const uint32_t base = Party::Now();
        Bytes32 secret{};
        secret.fill(static_cast<uint8_t>(0x50 + kb));
        SwapOffer o;
        o.network = SwapNetwork::Regtest;
        o.chain = CounterChain::Zcash;
        o.din_amount_una = kDinAmount;
        o.btc_amount_sat = kZecAmount;
        crypto::CSHA256().Write(secret.data(), secret.size()).Finalize(o.payment_hash.data());
        o.din_refund_pubkey = XOnly(3);
        o.btc_claim_pubkey = detail::CompressedOf(Scalar(4));
        o.expires_unix = base + 1 * kHour;
        o.t_btc_unix = base + 48 * kHour;
        o.t_din_unix = base + 96 * kHour;
        o.n_din_confirmations = kMinDinConfirmations;
        o.n_btc_confirmations = kMinZecConfirmations;
        o = DecodeOffer(EncodeOffer(o));  // as Bob receives it
        Check(EncodeOffer(o).rfind("dinswap2o", 0) == 0, "the offer travels as dinswap2o");
        RequireAcceptableNow(o, base);

        // ---- Bob's deposit: paid from "any wallet" (the Zebra miner's coins) ----
        const std::string deposit_addr = EncodeZecTAddress(ZecAddr(kDeposit));
        if (!bob_silent) {
            const auto miner = ObserveZecDeposit(zec, EncodeZecTAddress(ZecAddr(kZebraMiner)), "test");
            std::vector<ZecCoin> coins;
            uint64_t total = 0;
            for (const auto& c : miner.coins) {
                if (c.confirmations < 101 || total > kZecAmount + 100'000) continue;
                coins.push_back(c.coin);
                total += c.coin.value_zat;
            }
            const auto c = ObserveZecChain(zec, "test");
            auto pay = BuildZecFundingTx(coins, ZecScriptPubKey(DecodeZecTAddress(deposit_addr)), kZecAmount + 20'000,
                                         ZecScriptPubKey(ZecAddr(kZebraMiner)), 1, c->next_block_branch_id, 0);
            SignZecP2pkhInputs(pay.tx, coins, Scalar(kZebraMiner));
            ZebraBroadcast(zec_client)(detail::ToHex(SerializeZecTx(pay.tx)));
            // Zebra may serve a cached block template, so the payment can land a block late.
            for (int i = 0; i < 20 && ObserveZecDeposit(zec, deposit_addr, "test").confirmed_zat(kZecDepositConfirmations) == 0; ++i) {
                mine_zec(1);
            }
            const auto dep = ObserveZecDeposit(zec, deposit_addr, "test");
            Check(dep.ok && dep.confirmed_zat(kZecDepositConfirmations) == kZecAmount + 20'000,
                  "bob's deposit is confirmed (amount + funding fee) before he accepts");
        }
        SwapAccept a;
        a.offer_id = OfferId(o);
        a.din_claim_pubkey = XOnly(5);
        a.btc_refund_pubkey = detail::CompressedOf(Scalar(6));
        a = DecodeAccept(EncodeAccept(a));

        const auto din_tip = din("getblockcount", Json::Value(Json::arrayValue));
        const auto zec_tip = ObserveZecChain(zec, "test");
        if (!din_tip || !zec_tip) throw std::runtime_error("a node is unreachable");

        RunnerConfig config;
        config.din_hrp = "rdin";
        config.use_tower = use_tower;

        // Alice's DIN lock is paid by the wallet's retained funding owner, as in
        // production: the fixture daemon exposes it and the payer's identity.
        const auto payer = din("test.swap_funding.identity", Json::Value(Json::arrayValue));
        if (!payer || !(*payer)["wallet_id"].isString()) {
            throw std::runtime_error("Real fixture payer identity unavailable (run against swap_e2e_fixture_daemon)");
        }
        const auto funding_binding = ParseDinFundingBinding((*payer)["wallet_id"].asString(), "2", "5000000");
        const DinFundingOwner funding_owner = [&](const DinFundingRequest& request, DinFundingOperation operation,
                                                  const std::vector<uint8_t>& expected) -> PreparedDinFunding {
            Json::Value args(Json::objectValue);
            args["wallet_id"] = detail::ToHex(std::vector<uint8_t>(request.binding.wallet_id.begin(), request.binding.wallet_id.end()));
            args["offer_id"] = detail::ToHex(std::vector<uint8_t>(request.offer_id.begin(), request.offer_id.end()));
            args["address"] = request.address;
            args["amount_una"] = Json::UInt64(request.amount_una);
            args["fee_rate_hint"] = Json::UInt64(request.binding.fee_rate_hint);
            args["maximum_fee_una"] = Json::UInt64(request.binding.maximum_fee_una);
            args["operation"] = static_cast<unsigned>(operation);
            args["expected_body"] = detail::ToHex(expected);
            const auto result = din("test.swap_funding.owner", args);
            if (!result || !(*result)["hex"].isString() || !(*result)["txid"].isString()) {
                throw std::runtime_error("Real retained funding owner refused request");
            }
            return {detail::FromHex((*result)["hex"].asString()), (*result)["txid"].asString()};
        };

        auto make_session = [&](Role role) {
            SwapSession s;
            s.record.role = role;
            s.record.offer = o;
            s.record.accept = a;
            s.record.state_since_unix = base;
            if (role == Role::DinSeller) {
                s.record.secret = secret;
                s.din_funding_binding = funding_binding;
            }
            s.btc_scan_from_height = zec_tip->tip_height;
            s.din_scan_from_height = din_tip->asUInt();
            s.din_payout_script = P2tr(role == Role::DinSeller ? kAliceDinRefund : kBobDinClaim);
            s.btc_payout_script = ZecScriptPubKey(ZecAddr(role == Role::DinSeller ? kAliceZecClaim : kBobZecRefund));
            if (role == Role::BtcSeller) s.din_sweep_pubkey = XOnly(kBobSweep);  // production Bob: CPFP sweep key
            return s;
        };
        Party alice("alice", SwapKeys{Scalar(3), Scalar(4), {}}, dir + "/zec-alice-" + scenario + ".swap", 0xa1);
        Party bob("bob", SwapKeys{Scalar(5), Scalar(6), Scalar(kBobSweep)}, dir + "/zec-bob-" + scenario + ".swap", 0xb0);
        alice.store.Save(make_session(Role::DinSeller));
        bob.store.Save(make_session(Role::BtcSeller));

        // Bob's watchtower runs in this process (dinero-swap-tower is Bitcoin-only
        // until the Zcash code ships in the release build).
        std::string delivered;
        std::unique_ptr<ZecSwapChainIo> tower_io;
        std::unique_ptr<Watchtower> tower;
        bool tower_healthy = false;

        auto make_io = [&](const SwapSession& s, bool bob_side) {
            ZecIoConfig zc;
            zc.expected_chain = "test";
            zc.network = ZecNetwork::Regtest;
            zc.broadcast = ZebraBroadcast(zec_client);
            if (bob_side) {
                zc.deposit_secret_key = Scalar(kDeposit);
                zc.change_script = s.btc_payout_script;
            }
            return std::make_unique<ZecSwapChainIo>(din, zec, WatchSpecFor(s, config), zc);
        };
        auto boot = [&](Party& p) {
            p.runner.reset();
            p.io.reset();
            const auto s = EncryptedFileSwapStore::Load(p.store_path, p.store_key);
            p.io = make_io(s, s.record.role == Role::BtcSeller);
            p.io->SetDinFundingOwner(funding_owner);
            if (use_tower && s.record.role == Role::BtcSeller) {
                p.io->SetTowerSink([&, s](const std::string& text) {  // s by value: boot's local goes away
                    delivered = text;
                    const auto pkg = DecodeTowerPackage(text);
                    tower_io = make_io(s, false);
                    tower_io->PinFunding(TowerWatchSpec(pkg, "test").din_funding_txid,
                                         TowerWatchSpec(pkg, "test").btc_funding_txid);
                    tower = std::make_unique<Watchtower>(pkg, TowerConfig{}, *tower_io);
                    std::cout << "        package -> in-process tower (" << pkg.zec_refunds.size() << " ZEC rung)\n";
                });
                p.io->SetTowerAck([&](const std::string&, const std::string& hash) {
                    return tower && tower_healthy && TowerPackageHash(delivered) == hash;
                });
            }
            p.runner = std::make_unique<SwapRunner>(s, p.keys, config, *p.io, p.store);
        };
        boot(alice);
        boot(bob);

        auto tick = [&](Party& p, int round) {
            const auto r = p.runner->Tick(Party::Now());
            if (r.before != r.after || !r.events.empty() || !r.observed) {
                std::cout << "  [" << round << "] " << p.name << ": " << StateName(r.before) << " -> "
                          << StateName(r.after) << (r.observed ? "" : " (not observed)") << "\n";
                for (const auto& e : r.events) std::cout << "        " << e << "\n";
            }
        };
        auto tick_tower = [&](int round) {
            if (!tower) return;
            const auto r = tower->Tick(Party::Now());
            tower_healthy = r.healthy;
            for (const auto& e : r.events) {
                if (e.find("not observed") == std::string::npos) std::cout << "  [" << round << "] tower: " << e << "\n";
            }
        };

        if (happy) {
            for (int round = 0; round < 160 && !(alice.Terminal() && bob.Terminal()); ++round) {
                if (!alice.Terminal()) tick(alice, round);
                if (!bob.Terminal()) tick(bob, round);
                if (!alice.restarted && alice.State() == SwapState::DinLocked) {
                    boot(alice);
                    alice.restarted = true;
                }
                if (!bob.restarted && bob.State() == SwapState::BtcLocked) {
                    boot(bob);
                    bob.restarted = true;
                }
                mine();
            }
            Check(alice.restarted && bob.restarted, "both runners were restarted from their stores mid-swap");
            Check(alice.State() == SwapState::Done, std::string("alice Done (is ") + StateName(alice.State()) + ")");
            Check(bob.State() == SwapState::Done, std::string("bob Done (is ") + StateName(bob.State()) + ")");
            Check(zec_balance(kAliceZecClaim) == int64_t(kZecAmount - kZecSpendFee), "alice received ZEC minus fee on chain");
            Check(bob.runner->session().din_swept, "bob swept his claimed DIN to his wallet (CPFP sweep key)");
            Check(din_balance(kBobDinClaim) == int64_t(kDinAmount - 2 * config.din_fee_una),
                  "bob received DIN minus claim and sweep fees on chain");
            Check(ObserveZecDeposit(zec, deposit_addr, "test").coins.empty(), "bob's deposit was swept into the lock");
        } else if (tower_claim) {
            int round = 0;
            for (; round < 80 && !(bob.State() == SwapState::BtcLocked && bob.runner->session().tower_armed); ++round) {
                tick(alice, round);
                tick(bob, round);
                tick_tower(round);
                mine();
            }
            Check(bob.runner->session().tower_armed, "bob's tower holds the package and acknowledged it");
            std::cout << "  bob goes offline\n";
            bob.runner.reset();
            bob.io.reset();
            for (; round < 200 && !(alice.Terminal() && din_balance(kBobDinClaim) > 0); ++round) {
                if (!alice.Terminal()) tick(alice, round);
                tick_tower(round);
                mine();
            }
            for (int i = 0; i < 8; ++i) mine();  // let the tower's claim settle
            Check(alice.State() == SwapState::Done, std::string("alice Done (is ") + StateName(alice.State()) + ")");
            Check(zec_balance(kAliceZecClaim) == int64_t(kZecAmount - kZecSpendFee), "alice received ZEC minus fee");
            const auto pkg = DecodeTowerPackage(delivered);
            bool rung_amount = false;
            // A tower rung pays Bob's sweep output; the tower's first child sweeps it to his wallet.
            for (const auto& r : pkg.din_claims) {
                rung_amount |= din_balance(kBobDinClaim) == int64_t(kDinAmount - r.fee_una - r.children.front().fee_una);
            }
            Check(rung_amount, "the tower claimed bob's DIN with one of his pre-signed rungs (" +
                                   std::to_string(din_balance(kBobDinClaim)) + " una)");
        } else if (bob_silent) {
            // Bob never deposited: he never locks. Alice funds and waits for T_din.
            bob.runner.reset();
            bob.io.reset();
            int round = 0;
            for (; round < 40 && alice.State() != SwapState::DinLocked; ++round) {
                tick(alice, round);
                mine();
            }
            for (int i = 0; i < 5; ++i, ++round) {
                tick(alice, round);
                mine();
            }
            Check(alice.State() == SwapState::DinLocked, std::string("alice waits with her DIN locked (is ") +
                                                             StateName(alice.State()) + ")");
            const auto d = alice.io->ObserveDin();
            Check(d.funding.has_value(), "alice's DIN lock is on chain");
            if (d.funding) {
                Json::Value p(Json::arrayValue);
                p.append(detail::ToHex(SignedDinRefund(alice.runner->session(), alice.keys, *d.funding, config.din_fee_una)));
                const auto env = din_client->call("sendrawtransaction", p);
                const std::string text = (env ? env->toStyledString() : std::string()) + din_client->get_last_error();
                Check(text.find("non-final") != std::string::npos, "dinerod refuses her DIN refund before T_din");
            }
            Check(zec_balance(kAliceZecClaim) == 0, "no ZEC moved");
        } else if (alice_silent) {
            int round = 0;
            for (; round < 80 && !(bob.State() == SwapState::BtcLocked && bob.runner->session().tower_armed); ++round) {
                if (alice.State() != SwapState::DinLocked) tick(alice, round);  // funds, then goes silent
                tick(bob, round);
                tick_tower(round);
                mine();
            }
            Check(bob.State() == SwapState::BtcLocked && bob.runner->session().tower_armed,
                  "bob locked ZEC and armed his tower");
            for (int i = 0; i < 45; ++i, ++round) {  // N_zec deep: Alice could claim, but is silent
                tick(bob, round);
                tick_tower(round);
                mine();
            }
            const auto z = bob.io->ObserveBtc();
            Check(z.funding && !z.htlc.spent, "the ZEC lock is unspent (Alice silent)");
            if (z.funding) {
                const auto refund = bob.io->SignCounterRefund(bob.runner->session(), bob.keys, *z.funding, kZecSpendFee);
                const auto said = zebra_says(refund);
                std::cout << "    zebra said: " << said << "\n";
                Check(said.find("rejected") == 0 && said.find("locked until") != std::string::npos,
                      "Zebra refuses bob's refund before T_zec");
            }
            const auto pkg = DecodeTowerPackage(delivered);
            Check(pkg.zec_refunds.size() == 1, "the tower holds one ZEC refund rung");
            Check(bob.State() == SwapState::BtcLocked, "bob keeps waiting for T_zec (no early action)");
            if (faketime_file.empty()) {
                std::cout << "  SKIP refund after T_zec: no shifted Zebra clock (libfaketime)\n";
            } else {
                Check(jump_zebra_past(o.t_btc_unix), "Zcash's median time passed T_zec (Zebra's clock moved ahead)");
                for (; round < 200 && !bob.Terminal(); ++round) {
                    tick(bob, round);
                    tick_tower(round);
                    mine();
                }
                Check(bob.State() == SwapState::Refunded, std::string("bob Refunded (is ") + StateName(bob.State()) + ")");
                const int64_t got = zec_balance(kBobZecRefund);
                Check(got == int64_t(kZecAmount - kZecSpendFee) || got == int64_t(kZecAmount - pkg.zec_refunds[0].fee_zat),
                      "bob got his ZEC back on chain (" + std::to_string(got) + " zat: " +
                          (got == int64_t(kZecAmount - kZecSpendFee) ? "his wallet's refund" : "his tower's rung") + ")");
            }
        } else if (tower_refund) {
            int round = 0;
            for (; round < 80 && !(bob.State() == SwapState::BtcLocked && bob.runner->session().tower_armed); ++round) {
                if (alice.State() != SwapState::DinLocked) tick(alice, round);  // funds, then goes silent
                tick(bob, round);
                tick_tower(round);
                mine();
            }
            Check(bob.runner->session().tower_armed, "bob locked ZEC and armed his tower");
            std::cout << "  bob goes offline\n";
            bob.runner.reset();
            bob.io.reset();
            for (int i = 0; i < 10; ++i, ++round) {  // hours before T_zec: the tower must wait
                tick_tower(round);
                mine();
            }
            Check(zec_balance(kBobZecRefund) == 0, "the tower sends nothing before T_zec");
            const auto pkg = DecodeTowerPackage(delivered);
            if (faketime_file.empty()) {
                std::cout << "  SKIP refund after T_zec: no shifted Zebra clock (libfaketime)\n";
            } else {
                Check(jump_zebra_past(o.t_btc_unix), "Zcash's median time passed T_zec (Zebra's clock moved ahead)");
                for (int i = 0; i < 30 && zec_balance(kBobZecRefund) <= 0; ++i, ++round) {
                    tick_tower(round);
                    mine();
                }
                Check(zec_balance(kBobZecRefund) == int64_t(kZecAmount - pkg.zec_refunds[0].fee_zat),
                      "the tower's rung, signed while Bob was online, refunded his ZEC (" +
                          std::to_string(zec_balance(kBobZecRefund)) + " zat)");
            }
        }
    } catch (const std::exception& e) {
        std::cout << "  FAIL exception: " << e.what() << "\n";
        ++g_failures;
    }
    std::cout << (g_failures ? "SCENARIO FAIL " : "SCENARIO PASS ") << scenario << std::endl;
    return g_failures ? 1 : 0;
}

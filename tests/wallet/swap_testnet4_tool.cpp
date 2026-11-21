// A DIN <-> BTC swap against the real Bitcoin testnet4 (tests/wallet/swap_testnet4_rehearsal.sh).
// Dinero runs on a local regtest node with transaction locks enforced (Dinero
// testnet does not enforce them, so a swap there is refused); Bitcoin is the
// public testnet4: real blocks, fees, relay and median time, no mining or mock
// time. Bob funds his BTC lock from the node's wallet; Alice's BTC payout and
// Bob's BTC refund go back to that wallet.
//
// Every key is random (testnet4 is public: a known key would let anyone race
// Alice's claim once the secret shows) and is written to <workdir>/keys so the
// coins can be recovered by hand. Both runners keep their stores in <workdir>;
// started again on the same workdir, the tool resumes the swap.
//   swap_testnet4_tool happy|bob-refund <workdir> <din_rpcport> <din_miner_address>
//                      <btc_rpcport> <btc_user> <btc_pass> <btc_sat> [max_minutes]
// happy:      both claim; Alice's BTC claim needs 6 testnet4 confirmations.
// bob-refund: Alice goes offline once her DIN is locked and never claims; Bob
//             takes his BTC back when T_btc opens by testnet4's median time
//             (the shortest the offer rules allow: 13 h after the start, so
//             this run takes ~14 h).
#include "wallet/swap/encrypted_store.h"
#include "wallet/swap/runner.h"
#include "wallet/swap/swap_crypto.h"

#include "bech32/bech32.hpp"
#include "crypto/evp_secp256k1.h"
#include "crypto/secure_random.h"
#include "crypto/sha256.h"
#include "rpc_client.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace dinero;
using namespace dinero::swap;

namespace {

constexpr uint32_t kHour = 3600;
constexpr uint64_t kDinAmount = 10 * 100'000'000ULL;  // 10 DIN

int g_failures = 0;
void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  OK   " : "  FAIL ") << what << std::endl;
    if (!ok) ++g_failures;
}

std::string Hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
std::string Hex(const std::vector<uint8_t>& b) { return Hex(b.data(), b.size()); }
std::vector<uint8_t> Unhex(const std::string& h) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

Bytes32 RandomKey() {
    const auto r = secure_random_bytes(32);
    Bytes32 k{};
    std::copy(r.begin(), r.end(), k.begin());
    return k;
}

Bytes32 XOnly(const Bytes32& sk) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_keypair kp;
    if (!secp256k1_keypair_create(secp, &kp, sk.data())) throw std::runtime_error("bad key");
    secp256k1_xonly_pubkey x;
    secp256k1_keypair_xonly_pub(secp, &x, nullptr, &kp);
    Bytes32 out{};
    secp256k1_xonly_pubkey_serialize(secp, out.data(), &x);
    return out;
}

std::array<uint8_t, 33> Compressed(const Bytes32& sk) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_pubkey pk;
    if (!secp256k1_ec_pubkey_create(secp, &pk, sk.data())) throw std::runtime_error("bad key");
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(secp, out.data(), &len, &pk, SECP256K1_EC_COMPRESSED);
    return out;
}

std::vector<uint8_t> P2tr(const Bytes32& sk) {
    const auto x = XOnly(sk);
    std::vector<uint8_t> spk{0x51, 0x20};
    spk.insert(spk.end(), x.begin(), x.end());
    return spk;
}

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
        return (*r)["result"];
    };
}

// The random keys of one run, kept in <workdir>/keys (secret: chmod 600).
struct Keys {
    Bytes32 secret, alice_din, alice_btc, bob_din, bob_btc, bob_sweep, bob_din_payout, alice_din_refund;
    std::string alice_btc_payout_spk, bob_btc_refund_spk;  // the node wallet's own addresses
    uint32_t base{0};

    void Save(const std::string& path) const {
        std::ofstream f(path);
        auto k = [&](const char* n, const Bytes32& v) { f << n << "=" << Hex(v.data(), v.size()) << "\n"; };
        k("secret", secret); k("alice_din", alice_din); k("alice_btc", alice_btc); k("bob_din", bob_din);
        k("bob_btc", bob_btc); k("bob_sweep", bob_sweep); k("bob_din_payout", bob_din_payout);
        k("alice_din_refund", alice_din_refund);
        f << "alice_btc_payout_spk=" << alice_btc_payout_spk << "\nbob_btc_refund_spk=" << bob_btc_refund_spk
          << "\nbase=" << base << "\n";
        f.close();
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    }
    static Keys Load(const std::string& path) {
        Keys out;
        std::ifstream f(path);
        std::string line;
        auto b32 = [](const std::string& h) { Bytes32 v{}; const auto u = Unhex(h); std::copy(u.begin(), u.end(), v.begin()); return v; };
        while (std::getline(f, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const auto n = line.substr(0, eq), v = line.substr(eq + 1);
            if (n == "secret") out.secret = b32(v);
            else if (n == "alice_din") out.alice_din = b32(v);
            else if (n == "alice_btc") out.alice_btc = b32(v);
            else if (n == "bob_din") out.bob_din = b32(v);
            else if (n == "bob_btc") out.bob_btc = b32(v);
            else if (n == "bob_sweep") out.bob_sweep = b32(v);
            else if (n == "bob_din_payout") out.bob_din_payout = b32(v);
            else if (n == "alice_din_refund") out.alice_din_refund = b32(v);
            else if (n == "alice_btc_payout_spk") out.alice_btc_payout_spk = v;
            else if (n == "bob_btc_refund_spk") out.bob_btc_refund_spk = v;
            else if (n == "base") out.base = uint32_t(std::stoul(v));
        }
        return out;
    }
};

struct Party {
    std::string name;
    SwapKeys keys;
    std::string store_path;
    Bytes32 store_key;
    EncryptedFileSwapStore store;
    std::unique_ptr<RpcSwapChainIo> io;
    std::unique_ptr<SwapRunner> runner;

    Party(std::string n, SwapKeys k, std::string path, uint8_t master)
        : name(std::move(n)), keys(k), store_path(path),
          store_key(DeriveSwapStoreKey([&] { Bytes32 m{}; m.fill(master); return m; }())), store(path, store_key) {}
    bool Terminal() const {
        const auto& ss = runner->session();
        const auto st = ss.record.state;
        if (st == SwapState::Done && ss.record.role == Role::BtcSeller && ss.din_sweep_pubkey != Bytes32{} &&
            !ss.din_swept) {
            return false;
        }
        return st == SwapState::Done || st == SwapState::Refunded || st == SwapState::Aborted || st == SwapState::Lost;
    }
    SwapState State() const { return runner->session().record.state; }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string scenario = argc > 1 ? argv[1] : "";
    const bool refund = scenario == "bob-refund";
    if ((argc != 9 && argc != 10) || (scenario != "happy" && !refund)) {
        std::cerr << "usage: see file header\n";
        return 2;
    }
    const std::string dir = argv[2];
    auto din_client = std::make_shared<rpc::RpcClient>("127.0.0.1", uint16_t(std::stoi(argv[3])), "test", "test");
    const std::string din_miner = argv[4];
    auto btc_client = std::make_shared<rpc::RpcClient>("127.0.0.1", uint16_t(std::stoi(argv[5])), argv[6], argv[7]);
    const uint64_t btc_sat = std::stoull(argv[8]);
    const int max_minutes = argc == 10 ? std::stoi(argv[9]) : refund ? 1080 : 240;
    const DinRpc din = MakeRpc(din_client, "DIN");
    const BtcRpc btc = MakeRpc(btc_client, "BTC");

    try {
        // Only a regtest Dinero with locks enforced, and only Bitcoin testnet4.
        const auto dinfo = din("getblockchaininfo", Json::Value(Json::arrayValue));
        if (!dinfo || (*dinfo)["chain"].asString() != "regtest" || !(*dinfo)["contextual_locks_active"].asBool()) {
            std::cerr << "ABORT: dinerod must be regtest with transaction locks enforced\n";
            return 1;
        }
        const auto binfo = btc("getblockchaininfo", Json::Value(Json::arrayValue));
        if (!binfo || (*binfo)["chain"].asString() != BitcoinChainFor(SwapNetwork::Testnet) ||
            (*binfo)["initialblockdownload"].asBool()) {
            std::cerr << "ABORT: bitcoind must be a synced testnet4 node\n";
            return 1;
        }

        const std::string keys_path = dir + "/keys";
        const bool resume = std::filesystem::exists(keys_path);
        Keys k;
        if (resume) {
            k = Keys::Load(keys_path);
            std::cout << "resuming the swap in " << dir << "\n";
        } else {
            std::filesystem::create_directories(dir);
            k.secret = RandomKey();
            k.alice_din = RandomKey(); k.alice_btc = RandomKey();
            k.bob_din = RandomKey(); k.bob_btc = RandomKey(); k.bob_sweep = RandomKey();
            k.bob_din_payout = RandomKey(); k.alice_din_refund = RandomKey();
            auto wallet_spk = [&](const char* label) {
                Json::Value p(Json::arrayValue);
                p.append(label);
                p.append("bech32m");
                const auto a = btc("getnewaddress", p);
                if (!a) throw std::runtime_error("bitcoind wallet getnewaddress failed");
                Json::Value q(Json::arrayValue);
                q.append(a->asString());
                const auto info = btc("getaddressinfo", q);
                if (!info) throw std::runtime_error("getaddressinfo failed");
                std::cout << "  " << label << ": " << a->asString() << "\n";
                return (*info)["scriptPubKey"].asString();
            };
            k.alice_btc_payout_spk = wallet_spk("swap-alice-claim");
            k.bob_btc_refund_spk = wallet_spk("swap-bob-refund");
            k.base = std::max(static_cast<uint32_t>(std::time(nullptr)), (*binfo)["mediantime"].asUInt() + 60);
            k.Save(keys_path);
        }

        SwapOffer o;
        o.network = SwapNetwork::Testnet;  // the Bitcoin leg's network; Dinero is local regtest
        o.din_amount_una = kDinAmount;
        o.btc_amount_sat = btc_sat;
        crypto::CSHA256().Write(k.secret.data(), k.secret.size()).Finalize(o.payment_hash.data());
        o.din_refund_pubkey = XOnly(k.alice_din);
        o.btc_claim_pubkey = Compressed(k.alice_btc);
        o.expires_unix = k.base + 1 * kHour;
        o.t_btc_unix = k.base + (refund ? 13 : 48) * kHour;
        o.t_din_unix = k.base + (refund ? 37 : 96) * kHour;
        o.n_din_confirmations = 30;
        o.n_btc_confirmations = 1;
        o = DecodeOffer(EncodeOffer(o));
        SwapAccept a;
        a.offer_id = OfferId(o);
        a.din_claim_pubkey = XOnly(k.bob_din);
        a.btc_refund_pubkey = Compressed(k.bob_btc);
        a = DecodeAccept(EncodeAccept(a));

        RunnerConfig config;
        config.din_hrp = "rdin";
        config.btc_hrp = "tb";
        config.btc_chain = BitcoinChainFor(SwapNetwork::Testnet);

        Party alice("alice", SwapKeys{k.alice_din, k.alice_btc}, dir + "/alice.swap", 0xa1);
        Party bob("bob", SwapKeys{k.bob_din, k.bob_btc, k.bob_sweep}, dir + "/bob.swap", 0xb0);
        if (!resume) {
            RequireAcceptableNow(o, k.base);
            const auto btc_tip = btc("getblockcount", Json::Value(Json::arrayValue));
            const auto din_tip = din("getblockcount", Json::Value(Json::arrayValue));
            if (!btc_tip || !din_tip) throw std::runtime_error("node unreachable");
            auto make_session = [&](Role role) {
                SwapSession s;
                s.record.role = role;
                s.record.offer = o;
                s.record.accept = a;
                s.record.state_since_unix = k.base;
                if (role == Role::DinSeller) s.record.secret = k.secret;
                s.btc_scan_from_height = btc_tip->asUInt() > 3 ? btc_tip->asUInt() - 3 : 0;
                s.din_scan_from_height = din_tip->asUInt();
                s.din_payout_script = P2tr(role == Role::DinSeller ? k.alice_din_refund : k.bob_din_payout);
                s.btc_payout_script = Unhex(role == Role::DinSeller ? k.alice_btc_payout_spk : k.bob_btc_refund_spk);
                if (role == Role::BtcSeller) s.din_sweep_pubkey = XOnly(k.bob_sweep);
                return s;
            };
            alice.store.Save(make_session(Role::DinSeller));
            bob.store.Save(make_session(Role::BtcSeller));
        }
        auto boot = [&](Party& p) {
            p.runner.reset();
            p.io.reset();
            const auto s = EncryptedFileSwapStore::Load(p.store_path, p.store_key);
            p.io = std::make_unique<RpcSwapChainIo>(din, btc, s, config);
            p.runner = std::make_unique<SwapRunner>(s, p.keys, config, *p.io, p.store);
        };
        boot(alice);
        boot(bob);

        auto tick = [&](Party& p, int round) {
            const auto r = p.runner->Tick(static_cast<uint32_t>(std::time(nullptr)));
            if (r.before != r.after || !r.events.empty()) {
                std::cout << "  [" << round << "] " << p.name << ": " << StateName(r.before) << " -> "
                          << StateName(r.after) << (r.observed ? "" : " (not observed)") << "\n";
                for (const auto& e : r.events) std::cout << "        " << e << "\n";
            }
        };
        auto mine_din = [&] {
            Json::Value p(Json::arrayValue);
            p.append(1);
            p.append(din_miner);
            for (int attempt = 0; attempt < 10; ++attempt) {
                if (din("generatetoaddress", p)) return;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            throw std::runtime_error("DIN mining failed");
        };

        const auto start = std::chrono::steady_clock::now();
        uint32_t last_btc_tip = 0;
        auto finished = [&] { return bob.Terminal() && (refund || alice.Terminal()); };
        for (int round = 0; !finished(); ++round) {
            const bool alice_offline = refund && alice.State() == SwapState::DinLocked;
            if (!alice.Terminal() && !alice_offline) tick(alice, round);
            if (!bob.Terminal()) tick(bob, round);
            mine_din();
            const auto tip = btc("getblockcount", Json::Value(Json::arrayValue));
            if (tip && tip->asUInt() != last_btc_tip) {
                last_btc_tip = tip->asUInt();
                std::cout << "  [" << round << "] testnet4 block " << last_btc_tip << "  alice=" << StateName(alice.State())
                          << " bob=" << StateName(bob.State()) << std::endl;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::minutes(max_minutes)) {
                std::cout << "  stopped after " << max_minutes << " min; run again on " << dir << " to resume\n";
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }

        if (refund) {
            Check(alice.State() == SwapState::DinLocked, std::string("alice never claimed (is ") + StateName(alice.State()) + ")");
            Check(bob.State() == SwapState::Refunded, std::string("bob Refunded (is ") + StateName(bob.State()) + ")");
            Json::Value desc(Json::arrayValue);
            desc.append("raw(" + k.bob_btc_refund_spk + ")");
            Json::Value sp(Json::arrayValue);
            sp.append("start");
            sp.append(desc);
            const auto scan = btc("scantxoutset", sp);
            const int64_t got = scan ? std::llround((*scan)["total_amount"].asDouble() * 1e8) : -1;
            Check(got > 0 && got <= int64_t(btc_sat) && got >= int64_t(btc_sat) - 20 * int64_t(config.btc_fee_sat),
                  "bob's refund paid " + std::to_string(got) + " sat back to the wallet on testnet4");
            std::cout << (g_failures ? "SWAP TESTNET4 BOB-REFUND: FAIL" : "SWAP TESTNET4 BOB-REFUND: PASS") << std::endl;
            return g_failures ? 1 : 0;
        }
        Check(alice.State() == SwapState::Done, std::string("alice Done (is ") + StateName(alice.State()) + ")");
        Check(bob.State() == SwapState::Done, std::string("bob Done (is ") + StateName(bob.State()) + ")");
        // Alice's BTC claim paid the wallet's address, confirmed on testnet4.
        Json::Value desc(Json::arrayValue);
        desc.append("raw(" + k.alice_btc_payout_spk + ")");
        Json::Value sp(Json::arrayValue);
        sp.append("start");
        sp.append(desc);
        const auto scan = btc("scantxoutset", sp);
        const int64_t got = scan ? std::llround((*scan)["total_amount"].asDouble() * 1e8) : -1;
        Check(got == int64_t(btc_sat - config.btc_fee_sat),
              "alice received " + std::to_string(got) + " sat on testnet4 (swap minus the claim fee)");
        // Bob's DIN reached his payout key on the Dinero side.
        Json::Value dp(Json::arrayValue);
        const auto x = XOnly(k.bob_din_payout);
        dp.append(bech32::Encode("rdin", 1, std::vector<uint8_t>(x.begin(), x.end()), bech32::Encoding::BECH32M));
        const auto db = din("getaddressbalance", dp);
        const int64_t din_got = db && (*db)["confirmed"].isNumeric() ? (*db)["confirmed"].asInt64() : -1;
        Check(din_got > 0, "bob received " + std::to_string(din_got) + " una of DIN");
        std::cout << (g_failures ? "SWAP TESTNET4: FAIL" : "SWAP TESTNET4: PASS") << std::endl;
        return g_failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}

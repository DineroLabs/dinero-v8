#include "daemon/mempool_chain_view.h"
#include "daemon/mempool_scan_sink.h"
#include "daemon/mempool.h"
#include "consensus/chainparams.h"
#include "consensus/script_verify.h"
#include "crypto/sha256.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dinero;
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
class FakeChain final : public IMempoolChainView {
public:
    MempoolChainCoin coin;
    uint256 funding;
    std::array<uint8_t, 32> secret{};
    Status read_status = Status::Ok;
    FakeChain() {
        funding.data[0] = 55;
        secret.back() = 67;
        auto* context = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
        secp256k1_keypair pair; secp256k1_xonly_pubkey pubkey;
        std::array<uint8_t, 32> encoded{};
        const bool ok = secp256k1_keypair_create(context, &pair, secret.data()) &&
            secp256k1_keypair_xonly_pub(context, &pubkey, nullptr, &pair) &&
            secp256k1_xonly_pubkey_serialize(context, encoded.data(), &pubkey);
        secp256k1_context_destroy(context);
        Require(ok, "fixture public key");
        std::vector<uint8_t> script{0x51, 0x20}; // P2TR output key
        script.insert(script.end(), encoded.begin(), encoded.end());
        coin.amount = 1000000; coin.script_pubkey = crypto::bytes_to_hex(script); coin.height = 1;
    }
    StatusOr<MempoolChainTip> GetTip() const override { return MempoolChainTip{110, uint256{}}; }
    StatusOr<MempoolChainCoin> GetCoinWithConfidentialFallback(
        const uint256& id, uint32_t n) const override {
        if (read_status != Status::Ok) return read_status;
        return id == funding && n == 0 ? StatusOr<MempoolChainCoin>(coin)
                                      : StatusOr<MempoolChainCoin>(Status::NotFound);
    }
    bool HaveTransaction(const uint256&) const override { return false; }
    bool HaveConfidentialCommitment(const std::vector<uint8_t>&) const override { return false; }
};
class Scan final : public IMempoolScanSink {
public:
    std::vector<std::string> observed;
    std::unique_ptr<Guard> AcquireEmptyRegistry() override { return std::make_unique<Guard>(); }
    void ScanMempoolTransaction(const std::string& hex) override { observed.push_back(hex); }
};
Transaction Spend(const FakeChain& chain, unsigned sequence = 0xfffffffd) {
    Transaction tx;
    tx.version = 2;
    tx.vin.emplace_back();
    tx.vin[0].prevout.txid = TxId(chain.funding);
    tx.vin[0].prevout.vout = 0;
    tx.vin[0].sequence = sequence;
    tx.vout.emplace_back(AmountUna::Una(998000), crypto::hex_to_bytes(chain.coin.script_pubkey));
    const auto hash = consensus::ScriptVerifier::ComputeTaprootSighash(tx, 0,
        {1000000}, {crypto::hex_to_bytes(chain.coin.script_pubkey)});
    Require(hash.size() == 32, "fixture signature hash");
    auto* context = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    secp256k1_keypair pair;
    std::array<uint8_t, 64> signature{}; std::array<uint8_t, 32> aux{};
    const bool ok = secp256k1_keypair_create(context, &pair, chain.secret.data()) &&
        secp256k1_schnorrsig_sign32(context, signature.data(), hash.data(), &pair, aux.data());
    secp256k1_context_destroy(context);
    Require(ok, "fixture signature");
    tx.vin[0].witness = {std::vector<uint8_t>(signature.begin(), signature.end())};
    return tx;
}
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "supply an owned scratch directory");
        dinero::SelectParams(dinero::Chain::MAINNET);
        const auto root = std::filesystem::path(argv[1]) / ("standalone-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        struct Cleanup {
            std::filesystem::path root;
            ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); }
        } cleanup{root};
        FakeChain chain;
        auto scanner = std::make_unique<Scan>();
        auto* captured_scanner = scanner.get();
        dinero::Mempool pool(&chain, nullptr, false, std::move(scanner));
        auto tx = Spend(chain);
        const auto admitted = pool.submitTransaction(tx, "standalone", false);
        Require(admitted.accepted(), "valid witness spend refused: " + admitted.message);
        Require(pool.size() == 1 && pool.getTotalFees() == 2000, "admission fee/inventory");
        Require(pool.isOutputSpentInMempool({tx.vin[0].prevout.txid, 0}), "spent-input ownership");
        Require(captured_scanner->observed == std::vector<std::string>{tx.SerializeHex()},
            "accepted historical body reaches the injected scanner");
        const auto duplicate = pool.submitTransaction(tx, "standalone", false);
        Require(duplicate.code == dinero::TxRejectCode::ALREADY_IN_MEMPOOL, "duplicate contract");
        auto invalid = Spend(chain, 0xfffffffc); invalid.vout.clear();
        Require(pool.submitTransaction(invalid, "standalone", false).rejected(), "invalid body admitted");
        Require(pool.size() == 1 && captured_scanner->observed.size() == 1,
            "refused/duplicate bodies must not publish scans or pool entries");
        const auto file = root / "mempool.dat";
        Require(pool.saveToDisk(file.string()), "save real mempool file");
        std::ifstream input(file, std::ios::binary); char magic[8]{};
        input.read(magic, 8);
        Require(std::string(magic, 8) == "MEMPOOLV", "persistence format changed");
        pool.clear();
        Require(pool.size() == 0, "clear before reload");
        Require(pool.loadFromDisk(file.string()), "readmit saved body");
        Require(pool.size() == 1 && pool.hasTransaction(tx.GetTxid().AsUint256()) &&
            pool.getTotalFees() == 2000, "reloaded membership and fee");
        pool.clear(); chain.read_status = dinero::Status::Io;
        const auto unavailable = pool.submitTransaction(tx, "standalone", false);
        Require(unavailable.code == dinero::TxRejectCode::UNAVAILABLE && pool.size() == 0,
            "storage failure must remain unavailable, not a missing coin or accepted spend");
        std::cout << "Standalone mempool: real admission, rejection, scanner, persistence and unavailable checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

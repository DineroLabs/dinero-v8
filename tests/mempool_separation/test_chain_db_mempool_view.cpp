#include "storage/chain_db_mempool_view.h"
#include "storage/chain_write_token.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dinero;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
uint256 Id(unsigned char byte) {
    uint256 id;
    id.data[0] = byte;
    return id;
}
void Check(const std::filesystem::path& parent) {
    // Catch accidental adapter caching/state in addition to the live-update
    // observations below. The adapter owns only a vptr and one borrowed DB.
    static_assert(sizeof(ChainDbMempoolView) == 2 * sizeof(void*));
    const auto root = parent / ("chain-view-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    } cleanup{root};
    ChainDB db;
    Require(db.init(root / "db") == Status::Ok, "database initialization");
    ChainDbMempoolView view(&db);
    const auto token = ChainWriteToken::CreateForTesting();
    Require(view.GetTip().status() == Status::NotFound, "missing tip status");
    Require(db.setTip(token, Id(7), 110, arith_uint256(110)) == Status::Ok, "write first tip");
    Require(view.GetTip().ok() && view.GetTip().value().height == 110 &&
        view.GetTip().value().hash == Id(7), "first selected tip");
    Require(db.setTip(token, Id(8), 111, arith_uint256(111)) == Status::Ok, "write next tip");
    Require(view.GetTip().value().height == 111 && view.GetTip().value().hash == Id(8),
        "adapter must not cache tip");

    Require(view.GetCoinWithConfidentialFallback(Id(1), 0).status() == Status::NotFound,
        "missing coin status must survive the boundary");
    Coin coin;
    coin.amount = 123456; coin.script_pubkey = "5120aabb"; coin.height = 37;
    coin.coinbase = true; coin.is_confidential = true; coin.commitment = {2, 3, 4};
    Require(db.putCoin(token, Id(1), 0, coin) == Status::Ok, "write confidential coin");
    const auto captured = view.GetCoinWithConfidentialFallback(Id(1), 0);
    Require(captured.ok(), "capture coin");
    const auto& c = captured.value();
    Require(c.amount == 123456 && c.script_pubkey == "5120aabb" && c.height == 37 &&
        c.coinbase && c.is_confidential && c.commitment == std::vector<uint8_t>({2, 3, 4}),
        "all coin fields must survive the boundary");

    Require(!view.HaveTransaction(Id(4)), "unknown transaction");
    Require(db.putTxIndex(token, Id(4), Id(8), 9) == Status::Ok, "index transaction");
    Require(view.HaveTransaction(Id(4)), "fresh transaction index read");
    Require(!view.HaveConfidentialCommitment({}), "empty commitment must be absent");
    Require(view.HaveConfidentialCommitment({2, 3, 4}), "first confidential commitment");
    coin.commitment = {8, 9};
    Require(db.putCoin(token, Id(2), 0, coin) == Status::Ok, "write second confidential coin");
    Require(view.HaveConfidentialCommitment({8, 9}), "second confidential commitment");
    Require(!view.HaveConfidentialCommitment({7, 7}), "unmatched commitment");
    coin.is_confidential = false; coin.commitment = {5, 6};
    Require(db.putCoin(token, Id(3), 0, coin) == Status::Ok, "write ordinary coin");
    Require(!view.HaveConfidentialCommitment({5, 6}), "ordinary coin is not a confidential match");
    db.close();
    Require(view.GetTip().status() == db.getTip().status(), "closed database tip status");
    Require(view.GetCoinWithConfidentialFallback(Id(1), 0).status() ==
        db.getCoinWithConfidentialFallback(Id(1), 0).status(), "closed database coin status");
}
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "supply an owned scratch directory");
        Check(argv[1]);
        std::cout << "ChainDbMempoolView: live tip/coin/index/commitment/status checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

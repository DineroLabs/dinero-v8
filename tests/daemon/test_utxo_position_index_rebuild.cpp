#include "consensus/utreexo_accumulator.h"
#include "consensus/chainparams.h"
#include "indexing/utxo_position_index.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

bool Require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

dinero::uint256 MakeTxId(uint64_t id) {
    dinero::uint256 txid;
    std::memset(txid.data, 0, 32);
    std::memcpy(txid.data, &id, sizeof(id));
    return txid;
}

std::string HexEncode(const std::vector<uint8_t>& bytes) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

void StoreCoin(dinero::ChainDB& chain_db,
               const dinero::uint256& txid,
               uint32_t vout,
               uint64_t amount,
               const std::vector<uint8_t>& script_pubkey,
               int height) {
    dinero::Coin coin;
    coin.amount = amount;
    coin.script_pubkey = HexEncode(script_pubkey);
    coin.height = height;
    coin.coinbase = false;
    coin.is_confidential = false;

    const auto status = chain_db.putCoin(dinero::ChainWriteToken::CreateForTesting(),
                                         txid,
                                         vout,
                                         coin);
    assert(status == dinero::Status::Ok);
}

}  // namespace

int main() {
    dinero::SelectParams(dinero::Chain::REGTEST);

    const fs::path test_dir =
        fs::temp_directory_path() / ("dinero-utxo-position-rebuild-" + std::to_string(::getpid()));
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    dinero::ChainDB chain_db;
    if (!Require(chain_db.init(test_dir / "chainstate") == dinero::Status::Ok,
                 "ChainDB init should succeed")) {
        return 1;
    }

    const std::vector<uint8_t> script_pubkey = {0x51, 0x20, 0x11, 0x22};

    const auto txid_present = MakeTxId(101);
    const auto txid_missing = MakeTxId(202);
    StoreCoin(chain_db, txid_present, 0, 5000, script_pubkey, 100);
    StoreCoin(chain_db, txid_missing, 1, 9000, script_pubkey, 101);

    size_t utxo_count = 0;
    const auto iterate_status = chain_db.forEachUTXO(
        [&](const dinero::uint256&, uint32_t, const dinero::Coin&) -> bool {
            ++utxo_count;
            return true;
        });
    if (!Require(iterate_status == dinero::Status::Ok, "ChainDB forEachUTXO should succeed") ||
        !Require(utxo_count == 2, "ChainDB should contain the two test UTXOs")) {
        chain_db.close();
        fs::remove_all(test_dir);
        return 1;
    }

    dinero::consensus::UtreexoForest forest;
    const auto present_leaf =
        dinero::consensus::HashUTXOForCreationHeight(txid_present, 0, 5000, script_pubkey, 100, false);
    const auto missing_leaf =
        dinero::consensus::HashUTXOForCreationHeight(txid_missing, 1, 9000, script_pubkey, 101, false);
    forest.add(present_leaf);

    dinero::indexing::UTXOPositionIndex index;
    const auto report = index.Rebuild(chain_db, forest);

    if (!Require(report.success, "Rebuild should succeed") ||
        !Require(report.matched == 1, "Exactly one UTXO should match the forest") ||
        !Require(report.missing == 1, "Exactly one UTXO should be reported missing") ||
        !Require(report.malformed == 0, "No malformed scripts expected") ||
        !Require(index.GetPositionCount() == 1, "Index should contain only the present UTXO") ||
        !Require(index.GetPosition(dinero::TxId(txid_present), 0).has_value(),
                 "Present UTXO should get a position") ||
        !Require(!index.GetPosition(dinero::TxId(txid_missing), 1).has_value(),
                 "Missing UTXO should not get a position") ||
        !Require(!forest.findLeafPosition(missing_leaf).has_value(),
                 "Forest should not contain the missing leaf")) {
        chain_db.close();
        fs::remove_all(test_dir);
        return 1;
    }

    std::cout << "UTXO position rebuild missing-leaf detection PASSED" << std::endl;

    // Exercise the successful rebuild path with a forest that has tombstoned
    // positions. The rebuilt index must still generate valid proofs against the
    // live forest, not a synthetic sequential one.
    const auto spend_txid = MakeTxId(303);
    const auto keep_txid_a = MakeTxId(404);
    const auto keep_txid_b = MakeTxId(505);

    StoreCoin(chain_db, spend_txid, 0, 11000, script_pubkey, 102);
    StoreCoin(chain_db, keep_txid_a, 0, 12000, script_pubkey, 103);
    StoreCoin(chain_db, keep_txid_b, 0, 13000, script_pubkey, 104);

    const auto spend_leaf =
        dinero::consensus::HashUTXOForCreationHeight(spend_txid, 0, 11000, script_pubkey, 102, false);
    const auto keep_leaf_a =
        dinero::consensus::HashUTXOForCreationHeight(keep_txid_a, 0, 12000, script_pubkey, 103, false);
    const auto keep_leaf_b =
        dinero::consensus::HashUTXOForCreationHeight(keep_txid_b, 0, 13000, script_pubkey, 104, false);

    forest.add(spend_leaf);
    forest.add(keep_leaf_a);
    forest.add(keep_leaf_b);

    const auto spend_position = forest.findLeafPosition(spend_leaf);
    if (!Require(spend_position.has_value(), "Spent leaf should have an initial position")) {
        chain_db.close();
        fs::remove_all(test_dir);
        return 1;
    }

    const auto spend_proof = forest.prove(*spend_position);
    if (!Require(spend_proof.has_value(), "Spent leaf should have an initial proof") ||
        !Require(forest.remove(spend_leaf, *spend_proof), "Spent leaf removal should succeed")) {
        chain_db.close();
        fs::remove_all(test_dir);
        return 1;
    }

    // Simulate spend in ChainDB by deleting the consumed coin.
    const auto delete_status = chain_db.deleteCoin(dinero::ChainWriteToken::CreateForTesting(),
                                                   spend_txid,
                                                   0);
    if (!Require(delete_status == dinero::Status::Ok, "Spent coin delete should succeed")) {
        chain_db.close();
        fs::remove_all(test_dir);
        return 1;
    }

    dinero::indexing::UTXOPositionIndex rebuilt_index;
    const auto live_report = rebuilt_index.Rebuild(chain_db, forest);
    if (!Require(live_report.success, "Live rebuild should succeed") ||
        !Require(live_report.matched == 3, "Three live UTXOs should match after spend") ||
        !Require(live_report.missing == 1, "Only the earlier synthetic missing UTXO should remain unmatched")) {
        chain_db.close();
        fs::remove_all(test_dir);
        return 1;
    }

    for (const auto& [txid, amount, leaf] : std::vector<std::tuple<dinero::uint256, uint64_t, dinero::consensus::UtreexoHash>>{
             {txid_present, 5000, present_leaf},
             {keep_txid_a, 12000, keep_leaf_a},
             {keep_txid_b, 13000, keep_leaf_b},
         }) {
        auto pos = rebuilt_index.GetPosition(dinero::TxId(txid), 0);
        if (!Require(pos.has_value(), "Rebuilt index should return a live position") ||
            !Require(*pos < forest.getNumLeaves(), "Live position must fit within forest numLeaves")) {
            chain_db.close();
            fs::remove_all(test_dir);
            return 1;
        }

        auto proof = forest.prove(*pos);
        if (!Require(proof.has_value(), "Rebuilt live position should generate a proof") ||
            !Require(proof->verify(leaf, forest.getRoots()), "Rebuilt live proof should verify")) {
            chain_db.close();
            fs::remove_all(test_dir);
            return 1;
        }
    }

    std::cout << "UTXO position rebuild live-proof generation PASSED" << std::endl;

    chain_db.close();
    fs::remove_all(test_dir);
    return 0;
}

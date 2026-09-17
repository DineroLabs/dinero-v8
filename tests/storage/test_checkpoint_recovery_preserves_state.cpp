// The utreexo column family also holds canonical shielded state and replay
// records. A forest-checkpoint reset must preserve those records byte-for-byte.
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>

using namespace dinero;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ \
    << ": " << #c << '\n'; std::exit(1); } } while (false)

int main() {
    const auto dir = std::filesystem::temp_directory_path() /
        ("dinero_checkpoint_reset_" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    const auto token = ChainWriteToken::CreateForTesting();
    uint256 hash;
    hash.data[0] = 42;
    uint8_t nullifier[32]{};
    nullifier[0] = 19;
    const std::string frontier("frontier\0bytes", 14);
    const std::string anchors("anchors\0bytes", 13);
    const std::string replay("replay\0bytes", 12);
    const std::vector<uint8_t> proof{1, 2, 3, 4};
    const auto preserved = [&](ChainDB& db) {
        auto f = db.getUtreexoMeta("shielded_frontier");
        CHECK(f.ok() && f.value() == frontier);
        auto a = db.getUtreexoMeta("shielded_anchor_history");
        CHECK(a.ok() && a.value() == anchors);
        auto journal = db.getUtreexoMeta("consensus_journal_125");
        CHECK(journal.ok() && journal.value() == "journal-state");
        auto promoted = db.getUtreexoMeta("assumeutxo_promotion_done");
        CHECK(promoted.ok() && promoted.value() == "1");
        auto n = db.countShieldedNullifiers();
        CHECK(n.ok() && n.value() == 1);
        unsigned seen = 0;
        CHECK(db.forEachShieldedNullifier([&](uint32_t h, const uint8_t* nf) {
            CHECK(h == 124 && std::equal(nf, nf + 32, nullifier));
            ++seen;
            return true;
        }) == Status::Ok);
        CHECK(seen == 1);
        auto marker = db.getShieldedTipMarker();
        CHECK(marker.ok() && marker.value().height == 125);
        CHECK(marker.value().block_hash == hash && marker.value().shielded_root == hash);
        CHECK(marker.value().tree_size == 2 && marker.value().nullifier_count == 1);
        auto targets = db.getCSNSpendTargets(hash);
        CHECK(targets.ok() && targets.value() == replay);
        auto transition = db.getTransitionProof(125);
        CHECK(transition.ok() && transition.value() == proof);
        CHECK(db.getCoin(hash, 0).ok());
        auto tip = db.getTip();
        CHECK(tip.ok() && tip.value().height == 125 && tip.value().hash == hash);
    };
    {
        ChainDB db;
        CHECK(db.init(dir) == Status::Ok);
        CHECK(db.putUtreexoMeta(token, "shielded_frontier", frontier) == Status::Ok);
        CHECK(db.putUtreexoMeta(token, "shielded_anchor_history", anchors) == Status::Ok);
        CHECK(db.putUtreexoMeta(token, "consensus_journal_125", "journal-state") == Status::Ok);
        CHECK(db.putUtreexoMeta(token, "assumeutxo_promotion_done", "1") == Status::Ok);
        CHECK(db.putShieldedNullifier(token, 124, nullifier) == Status::Ok);
        ChainDB::ShieldedTipMarker shielded;
        shielded.height = 125;
        shielded.block_hash = hash;
        shielded.shielded_root = hash;
        shielded.tree_size = 2;
        shielded.nullifier_count = 1;
        CHECK(db.putShieldedTipMarker(token, shielded) == Status::Ok);
        CHECK(db.putCSNSpendTargets(token, hash, replay) == Status::Ok);
        CHECK(db.putTransitionProof(token, 125, proof) == Status::Ok);
        Coin coin;
        coin.amount = 100;
        coin.height = 125;
        coin.coinbase = true;
        CHECK(db.putCoin(token, hash, 0, coin) == Status::Ok);
        CHECK(db.setTip(token, hash, 125, arith_uint256{}) == Status::Ok);
        ChainDB::ForestTipMarker forest;
        forest.height = 125;
        forest.block_hash = hash;
        forest.forest_root = hash;
        CHECK(db.putForestTipMarker(token, forest) == Status::Ok);
        CHECK(db.putUtreexoCheckpointWithChecksum(token, 0, {0, 1}) == Status::Ok);
        CHECK(db.putUtreexoCheckpointWithChecksum(token, 125, {2, 3}) == Status::Ok);
        preserved(db);
        CHECK(db.getUtreexoChecksum(125).ok());
        CHECK(db.wipeAllUtreexoCheckpoints() == Status::Ok);
        preserved(db);
        for (int h : {0, 125}) {
            CHECK(db.getUtreexoCheckpoint(h).status() == Status::NotFound);
            CHECK(db.getUtreexoChecksum(h).status() == Status::NotFound);
        }
        CHECK(db.getForestTipMarker().status() == Status::NotFound);
        CHECK(db.wipeAllUtreexoCheckpoints() == Status::Ok);
        preserved(db);
    }
    {
        ChainDB db;
        CHECK(db.init(dir) == Status::Ok);
        preserved(db);
        CHECK(db.getForestTipMarker().status() == Status::NotFound);
    }
    // An empty checkpoint column must not prevent deleting a forest marker.
    {
        ChainDB db;
        CHECK(db.init(dir / "marker-only") == Status::Ok);
        ChainDB::ForestTipMarker marker;
        marker.height = 125;
        CHECK(db.putForestTipMarker(token, marker) == Status::Ok);
        CHECK(db.wipeAllUtreexoCheckpoints() == Status::Ok);
        CHECK(db.getForestTipMarker().status() == Status::NotFound);
    }
    std::filesystem::remove_all(dir);
    std::cout << "CheckpointRecoveryPreservesState: PASS\n";
}

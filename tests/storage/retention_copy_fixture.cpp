// Synthetic state/proof oracle for subprocess crash and RocksDB lock gates.
// This is not a daemon fixture and does not run block acceptance or networking.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <rocksdb/write_batch.h>
#include <sqlite3.h>

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_delta.h"
#include "consensus/utreexo_delta_codec.h"
#include "dinero/core/consensus/chainparams.h"
#include "primitives/block.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "storage/forest_restore.h"

namespace {
namespace fs = std::filesystem;
using dinero::Status;
using dinero::consensus::UtreexoForest;
using dinero::consensus::UtreexoHash;
constexpr uint32_t kTip = 32, kFuture = 80;

void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

dinero::uint256 Hash(uint32_t height) {
    dinero::uint256 hash;
    for (size_t i = 0; i < 32; ++i)
        hash.data[i] = static_cast<uint8_t>((height * 37) ^ (0x91 + i));
    for (size_t i = 0; i < 4; ++i)
        hash.data[i] = static_cast<uint8_t>(height >> (i * 8));
    return hash;
}

UtreexoHash Leaf(uint64_t ordinal) {
    UtreexoHash leaf(32);
    for (size_t i = 0; i < 32; ++i)
        leaf[i] = static_cast<uint8_t>((ordinal * 131) ^ (0x3C + i));
    for (size_t i = 0; i < 8; ++i)
        leaf[24 + i] = static_cast<uint8_t>(ordinal >> (i * 8));
    return leaf;
}

struct HeightState {
    std::vector<uint8_t> forest;
    std::string delta;
    dinero::BlockHeader header;
    std::vector<UtreexoHash> leaves;
    std::vector<std::vector<uint8_t>> proofs;
};

std::vector<HeightState> ExpectedChain() {
    std::vector<HeightState> states(kTip + 1);
    UtreexoForest forest;
    forest.setCanonicalEmptyRoots(true);
    std::vector<UtreexoHash> live;
    uint64_t next_leaf = 0;
    for (uint32_t height = 0; height <= kTip; ++height) {
        auto& state = states[height];
        if (height) {
            dinero::consensus::UtreexoDelta delta;
            delta.numLeavesBefore = forest.getNumLeaves();
            const size_t deletes = std::min<size_t>(live.size(), height % 3);
            for (size_t i = 0; i < deletes; ++i) {
                const auto victim = (height * 7 + i) % live.size();
                const auto leaf = live[victim];
                const auto position = forest.findLeafPosition(leaf);
                Require(position.has_value(), "expected delete position missing");
                Require(forest.removeAtKnownPosition(*position, leaf), "expected delete failed");
                delta.recordDelete(*position, leaf);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(victim));
            }
            for (uint32_t i = 0; i < 2 + height % 4; ++i) {
                const auto leaf = Leaf(next_leaf++);
                const auto position = forest.add(leaf);
                Require(position != UINT64_MAX, "expected add failed");
                delta.recordAdd(leaf, position);
                live.push_back(leaf);
            }
            std::string error;
            Require(dinero::SerializeUtreexoDelta(delta, state.delta, error), error);
        }
        state.forest = forest.serialize();
        state.leaves = live;
        for (const auto& leaf : live) {
            const auto position = forest.findLeafPosition(leaf);
            Require(position.has_value(), "expected proof position missing");
            const auto proof = forest.prove(*position);
            Require(proof.has_value(), "expected proof missing");
            state.proofs.push_back(proof->serialize());
        }
        state.header.version = 1;
        if (height) state.header.prev_block_hash = Hash(height - 1);
        state.header.timestamp = 1000 + height;
        const auto commitment = forest.getCommitment();
        Require(commitment.size() == 32, "expected root has wrong size");
        std::memcpy(state.header.utreexo_root.data, commitment.data(), 32);
    }
    return states;
}

void Create(const fs::path& root, const std::vector<HeightState>& states) {
    Require(!fs::exists(root), "synthetic fixture destination must not exist");
    fs::create_directories(root / "blockchain");
    dinero::ChainDB db;
    Require(db.init(root / "blockchain" / "chaindb") == Status::Ok, "fixture DB init failed");
    const auto token = dinero::ChainWriteToken::CreateForTesting();
    for (uint32_t height = 0; height <= kTip; ++height) {
        const auto& state = states[height];
        Require(db.putHeader(token, Hash(height), state.header, height,
                             dinero::arith_uint256(height)) == Status::Ok, "put header failed");
        Require(db.putHeightIndex(token, height, Hash(height)) == Status::Ok,
                "put height index failed");
        Require(db.putUtreexoCheckpointWithChecksum(token, height, state.forest) == Status::Ok,
                "put checkpoint failed");
        Require(db.putTransitionProof(token, height,
            {0x51, static_cast<uint8_t>(height), 0xA7}) == Status::Ok, "put proof failed");
        if (height) {
            rocksdb::WriteBatch batch;
            batch.Put(dinero::MakeUtreexoDeltaUndoKey(Hash(height)), state.delta);
            Require(db.writeBatch(token, std::move(batch), true) == Status::Ok,
                    "put delta failed");
        }
    }
    Require(db.putUtreexoCheckpointWithChecksum(token, kFuture, states.back().forest) == Status::Ok,
            "put future checkpoint failed");
    Require(db.putUtreexoMeta(token, "subprocess-sentinel", "keep") == Status::Ok,
            "put sentinel failed");
    Require(db.setTip(token, Hash(kTip), kTip, dinero::arith_uint256(kTip)) == Status::Ok,
            "put tip failed");
    const dinero::ChainDB::ForestTipMarker marker{
        static_cast<int32_t>(kTip), Hash(kTip), states.back().header.utreexo_root};
    Require(db.putForestTipMarker(token, marker) == Status::Ok, "put forest tip marker failed");
    Require(db.getValidatedTip().status() == Status::NotFound,
            "synthetic fixture must match daemon shape without legacy validated tip");
    db.close();

    sqlite3* sqlite = nullptr;
    Require(sqlite3_open((root / "blockchain" / "utxo").c_str(), &sqlite) == SQLITE_OK,
            "fixture SQLite open failed");
    const std::string sql =
        "CREATE TABLE utxo_metadata (key TEXT PRIMARY KEY, value TEXT);"
        "INSERT INTO utxo_metadata VALUES ('assumeutxo_base_height','13');"
        "INSERT INTO utxo_metadata VALUES ('assumeutxo_base_block','" + Hash(13).GetHex() + "');";
    const int result = sqlite3_exec(sqlite, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(sqlite);
    Require(result == SQLITE_OK, "fixture SQLite schema failed");
    std::cout << "event=created synthetic_tip=" << kTip << '\n';
}

void Verify(const fs::path& root, const std::vector<HeightState>& states,
            const std::string& stage) {
    Require(stage == "original" || stage == "partial" || stage == "pruned" || stage == "skipped",
            "unknown verification stage");
    dinero::ChainDB db;
    Require(db.init(root / "blockchain" / "chaindb") == Status::Ok, "verify DB open failed");
    std::set<int> expected;
    for (uint32_t height = 0; height <= kTip; ++height) expected.insert(height);
    if (stage == "partial") for (int height : {1, 2, 3}) expected.erase(height);
    if (stage == "pruned") expected = {0, 10, 13, 20, 26, 27, 28, 29, 30, 31, 32};
    if (stage == "skipped") {
        expected = {0, 10, 13, 20, 26, 27, 28, 29, 30, 31, 32};
        for (int height = 1; height < 10; ++height) expected.insert(height);
    }
    expected.insert(kFuture);
    const auto heights = db.listUtreexoCheckpoints();
    Require(heights.ok(), "verify checkpoint enumeration failed");
    Require(std::set<int>(heights.value().begin(), heights.value().end()) == expected,
            "durable retained checkpoint heights differ from expected " + stage);
    uint64_t proofs = 0;
    for (int height = kTip; height >= 0; --height) {
        const auto& state = states[height];
        const auto checkpoint = db.getUtreexoCheckpoint(height);
        const auto checksum = db.getUtreexoChecksum(height);
        Require(checkpoint.status() == checksum.status(), "non-atomic checkpoint/checksum pair");
        if (expected.count(height))
            Require(checkpoint.ok() && checkpoint.value() == state.forest,
                    "retained checkpoint bytes changed");
        dinero::consensus::UtreexoForest restored;
        std::string error;
        const auto status = dinero::storage::RestoreHistoricalForest(db, height, restored, error);
        Require(status == Status::Ok, "restore height " + std::to_string(height) + ": " + error);
        Require(restored.serialize() == state.forest, "restored forest bytes changed");
        for (size_t i = 0; i < state.leaves.size(); ++i) {
            const auto position = restored.findLeafPosition(state.leaves[i]);
            Require(position.has_value(), "restored leaf position missing");
            const auto proof = restored.prove(*position);
            Require(proof.has_value(), "restored proof missing");
            Require(proof->serialize() == state.proofs[i], "historical proof bytes changed");
            Require(proof->verify(state.leaves[i], restored.getRoots()), "restored proof failed verification");
            ++proofs;
        }
        const auto transition = db.getTransitionProof(height);
        Require(transition.ok() && transition.value() == std::vector<uint8_t>{
                    0x51, static_cast<uint8_t>(height), 0xA7}, "transition proof changed");
        if (height) {
            std::string delta;
            const auto status = db.getRaw(dinero::MakeUtreexoDeltaUndoKey(Hash(height)), delta);
            if (stage == "skipped" && height == 8)
                Require(status == Status::NotFound, "intentionally missing delta unexpectedly exists");
            else
                Require(status == Status::Ok && delta == state.delta, "delta sidecar changed");
        }
    }
    const auto meta = db.getUtreexoMeta("subprocess-sentinel");
    Require(meta.ok() && meta.value() == "keep", "metadata changed");
    const auto future = db.getUtreexoCheckpoint(kFuture);
    Require(future.ok() && future.value() == states.back().forest, "future checkpoint changed");
    std::cout << "event=verified stage=" << stage << " verified_heights=" << kTip + 1
              << " verified_proofs=" << proofs << '\n';
}

void HoldLock(const fs::path& root, const fs::path& ready) {
    dinero::ChainDB db;
    Require(db.init(root / "blockchain" / "chaindb") == Status::Ok, "lock fixture DB open failed");
    std::ofstream(ready) << "RocksDB lock held\n";
    Require(fs::exists(ready), "lock ready marker failed");
    std::string release;
    std::getline(std::cin, release);
}

void RemoveDeltaEight(const fs::path& root) {
    dinero::ChainDB db;
    Require(db.init(root / "blockchain" / "chaindb") == Status::Ok, "mutation DB open failed");
    const auto token = dinero::ChainWriteToken::CreateForTesting();
    rocksdb::WriteBatch batch;
    batch.Delete(dinero::MakeUtreexoDeltaUndoKey(Hash(8)));
    Require(db.writeBatch(token, std::move(batch), true) == Status::Ok, "delta-eight deletion failed");
    std::cout << "event=synthetic_mutation missing_delta_height=8\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        Require(argc >= 3, "usage: retention_copy_fixture create DIR | verify DIR STAGE | hold-lock DIR READY | remove-delta-eight DIR");
        dinero::SelectParams(dinero::Chain::TESTNET);
        const std::string mode = argv[1];
        const fs::path root = argv[2];
        if (mode == "create" && argc == 3) Create(root, ExpectedChain());
        else if (mode == "verify" && argc == 4) Verify(root, ExpectedChain(), argv[3]);
        else if (mode == "hold-lock" && argc == 4) HoldLock(root, argv[3]);
        else if (mode == "remove-delta-eight" && argc == 3) RemoveDeltaEight(root);
        else throw std::runtime_error("invalid fixture mode or arguments");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fixture failure: " << error.what() << '\n';
        return 1;
    }
}

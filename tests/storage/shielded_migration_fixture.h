#pragma once
#include "shielded_store_fixture.h"
#include "consensus/shielded/shielded_root.h"
namespace shielded_store_fixture {
namespace fs = std::filesystem;
namespace sh = dinero::consensus::shielded;
using dinero::arith_uint256;
struct CanonicalTemp : TempDir { CanonicalTemp() { path = fs::canonical(path); } };
void ValidSeed(const fs::path& path, bool empty = false) {
    Seed(path, false, std::nullopt);
    ChainDB db; Setup(db.init(path) == Status::Ok, "open legacy fixture");
    const auto token = ChainWriteToken::CreateForTesting();
    sh::CommitmentTree tree;
    if (!empty) { sh::Hash note{}; note[31] = 7; tree.Append(note); }
    sh::AnchorHistory anchors; anchors.RecordRoot(3, tree.Root());
    std::vector<sh::NullifierEntry> entries;
    if (empty) Setup(db.deleteAllShieldedNullifiers(token).ok(), "empty nullifiers");
    else for (uint32_t h : {1, 3}) {
        sh::NullifierEntry entry; entry.height = h; entry.nullifier.fill(h); entries.push_back(entry);
    }
    ChainDB::ShieldedTipMarker marker;
    marker.height = 3; marker.block_hash.data[0] = 3; marker.tree_size = tree.Size();
    marker.nullifier_count = entries.size();
    const auto root = tree.Root();
    // CurrentShieldedStateSnapshot persists the commitment-tree root here,
    // not ComputeShieldedRoot's composite consensus hash. Match the writer.
    std::memcpy(marker.shielded_root.data, root.data(), root.size());
    Setup(db.setTip(token, marker.block_hash, 3, arith_uint256(1)) == Status::Ok, "tip");
    Setup(db.putShieldedTipMarker(token, marker) == Status::Ok, "shielded marker");
    ChainDB::ForestTipMarker forest; forest.height = 3; forest.block_hash = marker.block_hash;
    // This marker checks identity only; forest reconstruction is a separate gate.
    Setup(db.putForestTipMarker(token, forest) == Status::Ok, "forest marker");
    const auto frontier = tree.SerializeFrontier(), history = anchors.SerializePersistenceBytes();
    Setup(db.putShieldedState(token, ChainDB::ShieldedStateRecord::Frontier,
        {frontier.begin(), frontier.end()}) == Status::Ok, "frontier");
    Setup(db.putShieldedState(token, ChainDB::ShieldedStateRecord::AnchorHistory,
        {history.begin(), history.end()}) == Status::Ok, "anchors");
}

void Clone(const fs::path& source, const fs::path& destination) {
    const auto rows = Inspect(source); Raw target(destination, legacy);
    for (const auto& [name, records] : rows)
        for (const auto& [key, value] : records) target.put(name, key, value);
}
} // namespace shielded_store_fixture

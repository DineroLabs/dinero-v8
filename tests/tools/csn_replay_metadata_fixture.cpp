// Offline, test-only legacy CSN fixture. Never installed or linked into dinerod.
#include "consensus/chainparams.h"
#include "consensus/csn_replay_data.h"
#include "consensus/block_lifecycle.h"
#include "consensus/merkle_root.h"
#include "daemon/datadir_guard.h"
#include "storage/archival_block_reader.h"
#include "storage/chain_write_token.h"
#include <json/json.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;
using namespace dinero;

namespace {
void Require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
void Check(Status status, const char* operation) {
    Require(status == Status::Ok, std::string(operation) + ": " + StatusToString(status));
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    Require(file.good(), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}
void SelectFixtureStorageProfile(const fs::path& path) {
    // main.cpp derives flatfile/P2P magic from this persisted PoW profile.
    // This offline tool reads bytes, not consensus: it must use the same
    // framing identity without claiming to reconstruct all consensus settings.
    constexpr std::string_view prefix = "regtest-pow-profile-v1\n";
    const auto marker = Read(path / "regtest-pow-profile");
    Require(marker.size() == prefix.size() + 65 &&
            marker.compare(0, prefix.size(), prefix) == 0 && marker.back() == '\n' &&
            std::all_of(marker.begin() + prefix.size(), marker.end() - 1, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }), "invalid PoW fixture profile");
    const auto magic = static_cast<uint32_t>(std::stoul(marker.substr(prefix.size(), 8), nullptr, 16));
    Require(magic != 0 && magic != 0xFABFB5DAu && magic != 0xD1A0C0DEu && magic != 0xDAB5BFFAu,
            "reserved PoW fixture profile magic");
    SelectParams(Chain::REGTEST);
    MutableParams().magic = magic;
}
Json::Value Inspect(ChainDB& db, BlockStorage& storage, const uint256& hash) {
    const auto metadata = db.getHeaderMetadata(hash);
    Require(metadata.ok(), "missing target header metadata");
    const auto body = storage::ReadArchivalBlockDetailed(
        db, &storage, hash, storage::ArchivalReadMode::RequireFlatfiles);
    Require(body.result.ok(), std::string("cannot read target archival body: ") +
            StatusToString(body.result.status()));
    const auto blob = db.getCSNSpendTargets(hash);
    Require(blob.ok(), "missing CSN replay record");
    consensus::CsnReplayData replay;
    Require(consensus::DecodeCsnReplayData(blob.value(), replay), "malformed CSN replay record");
    Json::Value result;
    result["hash"] = hash.GetHex();
    result["height"] = metadata->height;
    result["status_flags"] = metadata->status_flags;
    result["failed"] = (metadata->status_flags & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD)) != 0;
    result["format"] = replay.has_spent_outputs ? "CSN2" : "legacy";
    result["spent_outputs"] = Json::UInt64(replay.spent_outputs.size());
    result["body_has_utreexo"] = body.result->utreexo.has_value();
    result["targets"] = Json::arrayValue;
    for (const auto& target : replay.spend_targets) {
        uint256 value;
        Require(target.size() == sizeof(value.data), "wrong target length");
        std::copy(target.begin(), target.end(), value.begin());
        result["targets"].append(value.GetHex());
    }
    result["txids"] = Json::arrayValue;
    for (const auto& tx : body.result->vtx) result["txids"].append(tx.GetTxid().AsUint256().GetHex());
    const auto undo = storage::ReadArchivalUndoDetailed(
        db, &storage, hash, storage::ArchivalReadMode::AllowLegacyFallback);
    result["undo_spent"] = undo.result.ok() ? Json::Value(Json::UInt64(undo.result->spent.size())) : Json::Value();
    const auto legacy_body = db.getBlock(hash);
    result["legacy_body_has_utreexo"] = legacy_body.ok() && legacy_body->utreexo.has_value();
    const auto legacy_undo = db.getUndo(hash);
    result["legacy_undo_spent"] = legacy_undo.ok() ? Json::Value(Json::UInt64(legacy_undo->spent.size())) : Json::Value();
    return result;
}

Json::Value InspectMarkerlessState(ChainDB& db, const uint256& hash) {
    const auto metadata = db.getHeaderMetadata(hash);
    Require(metadata.ok(), "missing target header metadata");
    const auto replay = db.getCSNSpendTargets(hash);
    std::string marker;
    const auto marker_status = db.getRaw(
        consensus::CsnSpeculativeProofMarkerKey(hash), marker);
    Require(marker_status == Status::Ok || marker_status == Status::NotFound,
            "cannot inspect speculative marker");
    Json::Value result;
    result["hash"] = hash.GetHex();
    result["height"] = metadata->height;
    result["status_flags"] = metadata->status_flags;
    result["have_data"] = (metadata->status_flags & BLOCK_HAVE_DATA) != 0;
    result["transactions_valid"] =
        (metadata->status_flags & BLOCK_VALID_TRANSACTIONS) != 0;
    result["data_size"] = metadata->data_size;
    result["has_replay_sidecar"] = replay.status() == Status::Ok;
    result["has_proof_marker"] = marker_status == Status::Ok;
    return result;
}
}

int main(int argc, char** argv) {
    try {
        fs::path path;
        uint256 hash;
        bool explicit_regtest = false, downgrade = false, inspect = false;
        bool stage_markerless = false, inspect_markerless = false;
        bool strip_undo = false, keep_undo = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--datadir" && i + 1 < argc) path = argv[++i];
            else if (arg == "--hash" && i + 1 < argc) Require(uint256::FromHex(argv[++i], hash), "invalid block hash");
            else if (arg == "--regtest-fixture") explicit_regtest = true;
            else if (arg == "--downgrade") downgrade = true;
            else if (arg == "--inspect") inspect = true;
            else if (arg == "--stage-markerless") stage_markerless = true;
            else if (arg == "--inspect-markerless") inspect_markerless = true;
            else if (arg == "--strip-local-undo-spent") strip_undo = true;
            else if (arg == "--keep-local-undo") keep_undo = true;
            else throw std::runtime_error("unknown or incomplete argument: " + arg);
        }
        Require(explicit_regtest && !path.empty() && !hash.IsNull() &&
                    int(downgrade) + int(inspect) + int(stage_markerless) +
                        int(inspect_markerless) == 1,
                "required: --regtest-fixture --datadir PATH --hash HASH and one operation");
        Require(downgrade ? strip_undo != keep_undo : !strip_undo && !keep_undo,
                "downgrade requires exactly one explicit local-undo policy; inspect accepts neither");
        Require(fs::is_directory(path) && !fs::is_symlink(path), "datadir must be an existing real directory");
        path = fs::canonical(path);
        Require(Read(path / "csn-replay-metadata-test-only") ==
                "csn-replay-metadata-fixture-v1\n" + path.string() + "\n", "missing or mismatched test-only sentinel");
        Require(fs::is_regular_file(path / "blockchain/chaindb/CURRENT"), "missing existing ChainDB");
        daemon::DatadirGuard owner;
        std::string error;
        const bool acquired = owner.Acquire(path, error);
        Require(acquired, "datadir must be stopped: " + error);
        SelectFixtureStorageProfile(path);
        ChainDB db;
        Check(db.init(path / "blockchain/chaindb"), "open ChainDB");
        const auto genesis = db.getBlockHashByHeight(0);
        Require(genesis.ok() && genesis->GetHex() == Params().genesis_hash, "ChainDB does not contain regtest genesis");
        BlockStorage storage;
        Check(storage.init(path), "open block storage");
        const auto tip = db.getTip();
        Require(tip.ok(), "missing canonical tip");
        Json::Value result;
        result["active_tip"] = tip->hash.GetHex();
        result["active_height"] = tip->height;
        result["before"] = (stage_markerless || inspect_markerless)
            ? InspectMarkerlessState(db, hash) : Inspect(db, storage, hash);
        if (stage_markerless) {
            auto meta = db.getHeaderMetadata(hash).value();
            const auto canonical = db.getBlockHashByHeight(meta.height);
            Require(hash != tip->hash &&
                    (meta.height > tip->height || (canonical.ok() && canonical.value() != hash)) &&
                    (meta.status_flags & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD)) != 0,
                    "only an explicitly invalidated, noncanonical candidate may be staged");
            Require((meta.status_flags & BLOCK_HAVE_DATA) != 0 && meta.data_size != 0,
                    "fixture requires a durably stored body");
            const auto body = storage::ReadArchivalBlockDetailed(
                db, &storage, hash, storage::ArchivalReadMode::RequireFlatfiles).result;
            Require(body.ok() && body->GetHash() == hash,
                    "fixture requires exact stored body");
            bool mutated = false;
            Require(consensus::ComputeMerkleRoot(body->vtx, &mutated) ==
                        body->header.merkle_root && !mutated,
                    "fixture requires Merkle-bound valid body");
            const auto replay = db.getCSNSpendTargets(hash);
            Require(replay.ok(), "fixture requires genuine CSN replay sidecar");
            consensus::CsnReplayData decoded;
            Require(consensus::DecodeCsnReplayDataForBlock(*body, &replay.value(), decoded),
                    "fixture requires replay sidecar bound to the body");
            // Model an older speculative side branch: the body and its replay
            // data exist, but neither ingress validation nor CSN-SP1 attest
            // the stored proof. Reconsiderblock later clears the invalidation.
            meta.status_flags &= ~(BLOCK_VALID_TRANSACTIONS | BLOCK_VALID_CHAIN |
                                   BLOCK_VALID_SCRIPTS);
            const auto token = ChainWriteToken::CreateForTesting();
            rocksdb::WriteBatch batch;
            Check(db.putHeaderMetadata(token, hash, meta, &batch),
                  "stage pre-marker candidate metadata");
            batch.Delete(consensus::CsnSpeculativeProofMarkerKey(hash));
            Check(db.writeBatch(token, std::move(batch), true),
                  "commit markerless fixture");
            result["after"] = InspectMarkerlessState(db, hash);
            Require(result["after"]["have_data"].asBool() &&
                    result["after"]["has_replay_sidecar"].asBool() &&
                    !result["after"]["transactions_valid"].asBool() &&
                    !result["after"]["has_proof_marker"].asBool(),
                    "markerless fixture did not preserve body and replay only");
        }
        if (downgrade) {
            auto meta = db.getHeaderMetadata(hash).value();
            const auto canonical = db.getBlockHashByHeight(meta.height);
            Require(hash != tip->hash &&
                    (meta.height > tip->height || (canonical.ok() && canonical.value() != hash)) &&
                    (meta.status_flags & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD)) != 0,
                    "only an explicitly invalidated, noncanonical candidate may be downgraded");
            const auto original = storage::ReadArchivalBlockDetailed(
                db, &storage, hash, storage::ArchivalReadMode::RequireFlatfiles).result.value();
            const auto blob = db.getCSNSpendTargets(hash).value();
            consensus::CsnReplayData replay;
            Require(consensus::DecodeCsnReplayData(blob, replay) && replay.has_spent_outputs &&
                    !replay.spend_targets.empty() && !replay.spent_outputs.empty(), "expected nonempty CSN2 source");
            std::string legacy;
            const auto count = static_cast<uint32_t>(replay.spend_targets.size());
            for (unsigned i = 0; i < 4; ++i) legacy.push_back(static_cast<char>(count >> (8 * i)));
            for (const auto& target : replay.spend_targets) legacy.append(target.begin(), target.end());
            Block stripped = original;
            stripped.utreexo.reset();
            Require(stripped.GetHash() == hash && stripped.header.Serialize() == original.header.Serialize(), "header changed");
            for (size_t i = 0; i < original.vtx.size(); ++i)
                Require(stripped.vtx[i].Serialize() == original.vtx[i].Serialize(), "transaction bytes changed");
            const auto position = storage.writeBlock(hash, stripped);
            Require(position.ok() && position->offset <= std::numeric_limits<uint32_t>::max(), "cannot append stripped body");
            meta.file_number = position->file_number;
            meta.data_pos = static_cast<uint32_t>(position->offset);
            meta.data_size = position->size;
            auto undo = storage::ReadArchivalUndoDetailed(
                db, &storage, hash, storage::ArchivalReadMode::AllowLegacyFallback).result;
            Require(undo.ok(), "missing source undo");
            const auto original_undo_bytes = undo->Serialize();
            if (strip_undo) {
                undo->spent.clear();
                const auto undo_position = storage.writeUndo(hash, undo->Serialize());
                Require(undo_position.ok() && undo_position->offset <= std::numeric_limits<uint32_t>::max(), "cannot append stripped undo");
                meta.undo_file = undo_position->file_number;
                meta.undo_pos = static_cast<uint32_t>(undo_position->offset);
                meta.undo_size = undo_position->size;
            } else {
                Require(!undo->spent.empty(), "local-undo fixture requires genuine spent metadata");
            }
            const auto token = ChainWriteToken::CreateForTesting();
            rocksdb::WriteBatch batch;
            Check(db.putHeaderMetadata(token, hash, meta, &batch), "stage locators");
            Check(db.putCSNSpendTargets(token, hash, legacy, &batch), "stage legacy replay");
            if (db.getBlock(hash).ok()) Check(db.putBlock(token, hash, stripped, &batch), "stage legacy body");
            if (strip_undo && db.getUndo(hash).ok())
                Check(db.putUndo(token, hash, undo.value(), &batch), "stage legacy undo");
            Check(db.writeBatch(token, std::move(batch), true), "commit fixture");
            result["after"] = Inspect(db, storage, hash);
            result["undo_policy"] = strip_undo ? "strip" : "keep";
            Require(result["after"]["format"] == "legacy" && !result["after"]["body_has_utreexo"].asBool() &&
                    !result["after"]["legacy_body_has_utreexo"].asBool(),
                    "fixture still has a replay/body proof-metadata fallback");
            if (strip_undo) {
                Require(result["after"]["undo_spent"].asUInt64() == 0 &&
                        (result["after"]["legacy_undo_spent"].isNull() ||
                         result["after"]["legacy_undo_spent"].asUInt64() == 0),
                        "peer fixture still has a local undo fallback");
            } else {
                const auto preserved_undo = storage::ReadArchivalUndoDetailed(
                    db, &storage, hash, storage::ArchivalReadMode::AllowLegacyFallback).result;
                Require(preserved_undo.ok() && preserved_undo->Serialize() == original_undo_bytes &&
                        result["after"]["undo_spent"] == result["before"]["undo_spent"] &&
                        result["after"]["legacy_undo_spent"] == result["before"]["legacy_undo_spent"],
                        "local-undo fixture changed historical undo metadata");
            }
            Require(result["before"]["targets"] == result["after"]["targets"] &&
                    result["before"]["txids"] == result["after"]["txids"] &&
                    result["before"]["status_flags"] == result["after"]["status_flags"], "unrelated candidate state changed");
            Require(db.getTip()->hash == tip->hash && db.getTip()->height == tip->height, "canonical tip changed");
        }
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        std::cout << "FIXTURE_RESULT " << Json::writeString(writer, result) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fixture refused: " << error.what() << '\n';
        return 1;
    }
}

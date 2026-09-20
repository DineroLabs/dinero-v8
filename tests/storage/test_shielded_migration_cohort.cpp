// Generated datadirs, real SQLite, and synthetic header/forest chains only.
// No live daemon, network, PoW or combined-release qualification.
#include "shielded_migration_fixture.h"
#include "storage/shielded_migration_cohort.h"
#include "daemon/datadir_guard.h"
#include "daemon/regtest_pow_profile.h"
#include <fstream>
#include <sqlite3.h>
#include "storage/forest_restore.h"
#include "crypto/sha256.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_delta_codec.h"
#include "consensus/utreexo_canonical_roots_activation.h"
#include "consensus/chainparams.h"
#include "common/serialization.h"
#include <sys/wait.h>

using namespace shielded_store_fixture;
using dinero::daemon::DatadirGuard;
using dinero::storage::MigrateShieldedDatadirCopy;
constexpr dinero::storage::ShieldedMigrationLimits db_limits{32768, 1, 16384, 100};
constexpr dinero::storage::ShieldedCompanionLimits file_limits{100, 1024 * 1024, 100, 4096, 1000000, 100, 65536, 1000, 100, 100};
std::string executable;

void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary);
    out << bytes; Setup(out.good(), "write fixture");
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary); Setup(in.good(), "read fixture");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void Sql(const fs::path& path, const std::string& sql) {
    sqlite3* raw = nullptr; Setup(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "open fixture SQLite");
    const auto rc = sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr);
    const std::string error = sqlite3_errmsg(raw); sqlite3_close(raw);
    if (rc != SQLITE_OK) throw std::runtime_error("fixture SQL: " + error);
}
struct Fixture;
void SeedDefaultForest(Fixture&);
struct Fixture {
    CanonicalTemp temp;
    fs::path original = temp.path / "original", candidate = temp.path / "candidate";
    explicit Fixture(bool seed_forest = true) {
        fs::create_directories(original / "blockchain"); fs::create_directories(candidate / "blockchain");
        ValidSeed(original / "blockchain/chaindb");
        for (const auto& root : {original, candidate}) {
            DatadirGuard guard; std::string error; Setup(guard.Acquire(root, error), "seed daemon lock");
            guard.Release();
            Write(root / "blocks/blk00000.dat", "retained body");
            Write(root / "headers/fixture", "retained headers");
            Sql(root / "blockchain/utxo", "CREATE TABLE utxo_metadata(key TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL); PRAGMA user_version=1;");
            Sql(root / "blockchain/shielded_nullifiers.db", "CREATE TABLE nullifiers(nullifier BLOB PRIMARY KEY NOT NULL, block_height INTEGER NOT NULL); PRAGMA user_version=1;");
            Write(root / "blockchain/shielded_frontier.bin", "unchanged legacy file");
            Write(root / "checkpoints/mainnet/snapshot", "retained import input");
        }
        // Outside the migration's chain-companion inventory: do not open these.
        fs::create_symlink("missing-secret", original / "wallets");
        Write(original / "dinerod.pid", "30000000\n");
        Write(candidate / "dinerod.pid", "30000001\n");
        if (seed_forest) SeedDefaultForest(*this);
    }
    auto Run(bool apply = true, const std::function<void(const char*)>& hook = {}) {
        return MigrateShieldedDatadirCopy(original, candidate, db_limits, file_limits, apply, hook);
    }
};
int Child(const std::vector<std::string>& args) {
    std::vector<char*> argv{const_cast<char*>(executable.c_str())};
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork(); Setup(pid >= 0, "fork");
    if (pid == 0) {
        // No allocation or RocksDB calls after fork with engine threads alive.
        execv(executable.c_str(), argv.data()); _exit(127);
    }
    int status = 0; Setup(waitpid(pid, &status, 0) == pid && WIFEXITED(status), "child exit");
    return WEXITSTATUS(status);
}
void Success(bool inspect) {
    Fixture f; const auto original = Inspect(f.original / "blockchain/chaindb");
    const auto result = f.Run(!inspect);
    if (!result.ok) std::cerr << result.error << '\n';
    CHECK(result.ok); CHECK(result.ready == !inspect);
    CHECK(Inspect(f.original / "blockchain/chaindb") == original);
    CHECK(Read(f.original / "dinerod.pid") == "30000000\n");
    CHECK(Read(f.candidate / "dinerod.pid") == "30000001\n");
    CHECK(Read(f.candidate / "blocks/blk00000.dat") == "retained body");
    CHECK(fs::is_symlink(f.original / "wallets"));
    if (!inspect) CHECK(f.Run().ok);
    DatadirGuard guard; std::string error; CHECK(guard.Acquire(f.candidate, error));
}
void Refusal(const std::string& fault) {
    Fixture f; DatadirGuard guard; std::string error;
    auto source = f.original, copy = f.candidate;
    auto budget = file_limits;
    if (fault == "source_running" || fault == "copy_running")
        Setup(guard.Acquire(fault == "source_running" ? source : copy, error), "hold daemon guard");
    else if (fault == "recovery") Write(copy / "chainstate_recovery.marker", "incomplete");
    else if (fault == "reindex") Write(source / "blockchain/reindex_promotion.marker", "incomplete");
    else if (fault == "reindex_temporary") Write(copy / "blockchain/shielded_frontier.bin.reindex.tmp", "partial");
    else if (fault == "maintenance") Write(f.temp.path / ".nodecore-maintenance-v1/intent.json", "unknown");
    else if (fault == "different_body") Write(copy / "blocks/blk00000.dat", "mismatched body");
    else if (fault == "missing_header") fs::remove(copy / "headers/fixture");
    else if (fault == "extra_companion") Write(copy / "blockchain/future-file", "unexplained");
    else if (fault == "missing_lock") fs::remove(copy / "dinerod.lock");
    else if (fault == "lock_symlink") { fs::remove(copy / "dinerod.lock"); fs::create_symlink(source / "dinerod.lock", copy / "dinerod.lock"); }
    else if (fault == "file_symlink") { fs::remove(copy / "blocks/blk00000.dat"); fs::create_symlink(source / "blocks/blk00000.dat", copy / "blocks/blk00000.dat"); }
    else if (fault == "file_hardlink") { fs::remove(copy / "blocks/blk00000.dat"); fs::create_hard_link(source / "blocks/blk00000.dat", copy / "blocks/blk00000.dat"); }
    else if (fault == "same") copy = source;
    else if (fault == "root_symlink") { fs::create_directory_symlink(copy, f.temp.path / "alias"); copy = f.temp.path / "alias"; }
    else if (fault == "entry_budget") budget.max_entries = 1;
    else if (fault == "byte_budget") budget.max_bytes = 1;
    else if (fault == "zero_budget") budget.max_bytes = 0;
    else if (fault == "dirty_sqlite") for (const auto& root : {source, copy}) Write(root / "blockchain/utxo-wal", "pending");
    else Setup(false, "unknown refusal");
    const auto before = Inspect(f.candidate / "blockchain/chaindb");
    const auto result = MigrateShieldedDatadirCopy(source, copy, db_limits, budget, true);
    CHECK(!result.ok); CHECK(!result.ready); CHECK(!result.error.empty());
    CHECK(Inspect(f.candidate / "blockchain/chaindb") == before);
}
void Ownership() {
    Fixture f; unsigned stages = 0;
    const auto result = f.Run(true, [&](const char* stage) {
        if (std::string(stage) != "after_prepare" && std::string(stage) != "after_ready") return;
        ++stages;
        for (const auto& root : {f.original, f.candidate}) {
            DatadirGuard guard; std::string error; CHECK(!guard.Acquire(root, error));
            CHECK(Child({"--acquire", root.string()}) == 10);
        }
    });
    CHECK(result.ok); CHECK(stages == 2);
}
void Interrupt(const std::string& mutation) {
    Fixture f;
    CHECK(Child({"--interrupt", f.original.string(), f.candidate.string()}) == 86);
    const auto before = Inspect(f.candidate / "blockchain/chaindb");
    if (mutation == "source" || mutation == "both") Write(f.original / "blocks/blk00000.dat", "new retained body");
    if (mutation == "candidate" || mutation == "both") Write(f.candidate / "blocks/blk00000.dat", "new retained body");
    const auto result = f.Run();
    if (mutation.empty()) CHECK(result.ok && result.ready);
    else { CHECK(!result.ok); CHECK(Inspect(f.candidate / "blockchain/chaindb") == before); }
}
void During(const std::string& fault) {
    Fixture f;
    const auto result = f.Run(true, [&](const char* stage) {
        if (std::string(stage) != "before_ready") return;
        if (fault == "replace_lock") {
            fs::rename(f.candidate / "dinerod.lock", f.candidate / "old.lock");
            Write(f.candidate / "dinerod.lock", "");
        } else if (fault == "add_marker") Write(f.candidate / "chainstate_recovery.marker", "unexpected");
        else for (const auto& root : {f.original, f.candidate}) Write(root / "blocks/blk00000.dat", "new retained body");
    });
    CHECK(!result.ok); CHECK(!result.ready);
    CHECK(Inspect(f.candidate / "blockchain/chaindb")["meta"]["storage_layout_v1"] != ready);
}
void CannotBypassBinding() {
    Fixture f; CHECK(Child({"--interrupt", f.original.string(), f.candidate.string()}) == 86);
    const auto before = Inspect(f.candidate / "blockchain/chaindb");
    const auto result = dinero::storage::MigrateShieldedStateCopy(f.original / "blockchain/chaindb",
        f.candidate / "blockchain/chaindb", db_limits, true);
    CHECK(!result.ok); CHECK(Inspect(f.candidate / "blockchain/chaindb") == before);
}

void Metadata(const std::string& fault) {
    Fixture f; auto budget = file_limits;
    for (const auto& root : {f.original, f.candidate}) {
        const auto db = root / "blockchain/utxo", cache = root / "blockchain/shielded_nullifiers.db";
        auto set = [&](const std::string& key, const std::string& value) { Sql(db, "INSERT INTO utxo_metadata VALUES('" + key + "','" + value + "');"); };
        if (fault == "missing") fs::remove(db);
        else if (fault == "corrupt") Write(db, "not SQLite");
        else if (fault == "missing_table") Sql(db, "DROP TABLE utxo_metadata;");
        else if (fault == "view") Sql(db, "DROP TABLE utxo_metadata; CREATE VIEW utxo_metadata AS SELECT 'x' AS key, 'y' AS value;");
        else if (fault == "future_schema") Sql(db, "PRAGMA user_version=2;");
        else if (fault == "reorg") set("reorg_in_progress", "unfinished");
        else if (fault == "recovery") set("incomplete_reorg_recovery_tip", std::string(64, 'a'));
        else if (fault == "active") set("assumeutxo_active", "true");
        else if (fault == "active_bad") set("assumeutxo_active", "0");
        else if (fault == "unknown_state") set("assumeutxo_lifecycle_state", "future_state");
        else if (fault == "snapshot_loaded" || fault == "validating_history" || fault == "validation_stalled" || fault == "fatal_mismatch") set("assumeutxo_lifecycle_state", fault);
        else if (fault == "orphan_base") { set("assumeutxo_base_height", "1"); set("assumeutxo_base_block", std::string(64, 'a')); }
        else if (fault == "incomplete_validated") set("assumeutxo_lifecycle_state", "fully_validated");
        else if (fault == "orphan_validated") set("assumeutxo_fully_validated", "true");
        else if (fault == "orphan_fatal") set("assumeutxo_fatal_reason", "fatal");
        else if (fault == "unknown_reserved") set("assumeutxo_future_state", "1");
        else if (fault == "wallet_bad_height") set("wallet_snapshot_recovery_base_height", "01");
        else if (fault == "nul_value") Sql(db, "INSERT INTO utxo_metadata VALUES('reorg_in_progress',cast(x'00' AS TEXT));");
        else if (fault == "blob_value") Sql(db, "INSERT INTO utxo_metadata VALUES('reorg_in_progress',x'00');");
        else if (fault == "duplicate") Sql(db, "DROP TABLE utxo_metadata; CREATE TABLE utxo_metadata(key TEXT,value TEXT); INSERT INTO utxo_metadata VALUES('x','a'),('x','b');");
        else if (fault == "null_value") Sql(db, "DROP TABLE utxo_metadata; CREATE TABLE utxo_metadata(key TEXT,value TEXT); INSERT INTO utxo_metadata VALUES('x',NULL);");
        else if (fault == "legacy_cache") Sql(cache, "PRAGMA user_version=0; INSERT INTO nullifiers VALUES(zeroblob(32),1);");
        else if (fault == "future_cache") Sql(cache, "PRAGMA user_version=2;");
        else if (fault == "cache_schema") Sql(cache, "DROP TABLE nullifiers;");
        else if (fault == "cache_corrupt") Write(cache, "not SQLite");
        else if (fault == "rows") { set("x","1"); set("y","2"); budget.max_metadata_rows = 1; }
        else if (fault == "value_limit") { set("x",std::string(5000,'x')); }
        else if (fault == "steps") budget.max_sqlite_steps = 1;
        else if (fault == "step_exhaustion") {
            Sql(db, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1000) INSERT INTO utxo_metadata SELECT 'key'||x,'value' FROM n;");
            budget.max_metadata_rows = 2000; budget.max_sqlite_steps = 1000;
        }
        else if (fault == "empty_legacy_cache") Sql(cache, "PRAGMA user_version=0;");
        else if (fault == "absent_cache") fs::remove(cache);
        else if (fault == "disabled") { set("assumeutxo_active","false"); set("assumeutxo_lifecycle_state","disabled"); set("reorg_in_progress",""); }
        else Setup(false, "unknown metadata fault");
    }
    const bool good = fault == "empty_legacy_cache" || fault == "absent_cache" || fault == "disabled";
    const auto original = Inspect(f.original / "blockchain/chaindb");
    const auto before = Inspect(f.candidate / "blockchain/chaindb");
    const auto original_file = fs::exists(f.original / "blockchain/utxo") ? Read(f.original / "blockchain/utxo") : "";
    const auto result = MigrateShieldedDatadirCopy(f.original, f.candidate, db_limits, budget, true);
    if (good && !result.ok) std::cerr << result.error << '\n';
    CHECK(result.ok == good); CHECK(result.ready == good);
    if (!good) CHECK(Inspect(f.candidate / "blockchain/chaindb") == before);
    CHECK(Inspect(f.original / "blockchain/chaindb") == original);
    if (fs::exists(f.original / "blockchain/utxo")) CHECK(Read(f.original / "blockchain/utxo") == original_file);
    CHECK(!fs::exists(f.original / "blockchain/utxo-wal")); CHECK(!fs::exists(f.original / "blockchain/utxo-shm"));
}
struct ForestData {
    std::vector<dinero::uint256> hashes;
    std::vector<std::vector<uint8_t>> states;
};
ForestData SeedForest(Fixture& fixture) {
    using namespace dinero;
    using consensus::UtreexoForest;
    const auto path = fixture.original / "blockchain/chaindb";
    ChainDB db; Setup(db.init(path) == Status::Ok, "open forest fixture");
    const auto token = ChainWriteToken::CreateForTesting();
    Setup(db.wipeAllUtreexoCheckpoints() == Status::Ok, "remove opaque fixture checkpoints");
    UtreexoForest forest; forest.setCanonicalEmptyRoots(consensus::IsUtreexoCanonicalRootsActive(0));
    ForestData result; result.hashes.push_back(uint256{}); result.states.push_back(forest.serialize());
    Setup(db.putUtreexoCheckpoint(token, 0, forest.serialize()) == Status::Ok, "genesis checkpoint");
    consensus::UtreexoHash first(32, 0); first[0] = 1;
    for (uint32_t h = 1; h <= 12; ++h) {
        if (consensus::IsUtreexoCanonicalRootsActive(h) && !forest.isCanonicalEmptyRoots()) {
            forest.setCanonicalEmptyRoots(true); forest.rebuildRoots();
        }
        consensus::UtreexoDelta delta; delta.numLeavesBefore = forest.getNumLeaves();
        if (h == 3) {
            const auto position = forest.findLeafPosition(first); Setup(position.has_value(), "find spent leaf");
            Setup(forest.removeAtKnownPosition(*position, first), "delete spent leaf"); delta.recordDelete(*position, first);
        }
        for (uint8_t index : {0, 1}) {
            consensus::UtreexoHash leaf(32, 0); leaf[0] = h; leaf[1] = index;
            const auto position = forest.add(leaf); Setup(position != UINT64_MAX, "append leaf"); delta.recordAdd(leaf, position);
        }
        BlockHeader header{}; header.version = 1; header.prev_block_hash = result.hashes.back(); header.timestamp = 1000 + h;
        const auto root = forest.getCommitment(); std::memcpy(header.utreexo_root.data, root.data(), 32);
        const auto hash = header.GetHash(); result.hashes.push_back(hash); result.states.push_back(forest.serialize());
        Setup(db.putHeader(token, hash, header, h, arith_uint256(h)) == Status::Ok, "header");
        Setup(db.putHeightIndex(token, h, hash) == Status::Ok, "height index");
        std::string bytes, error; Setup(SerializeUtreexoDelta(delta, bytes, error), "delta encoding");
        rocksdb::WriteBatch batch; batch.Put(MakeUtreexoDeltaUndoKey(hash), bytes);
        Setup(db.writeBatch(token, std::move(batch), true) == Status::Ok, "delta write");
        if (h % 5 == 0) Setup(db.putUtreexoCheckpoint(token, h, forest.serialize()) == Status::Ok, "checkpoint");
    }
    auto marker = RequiredValue(db.getShieldedTipMarker()); marker.height = 12; marker.block_hash = result.hashes.back();
    Setup(db.putShieldedTipMarker(token, marker) == Status::Ok, "shielded marker alignment");
    ChainDB::ForestTipMarker tip; tip.height = 12; tip.block_hash = marker.block_hash;
    std::memcpy(tip.forest_root.data, forest.getCommitment().data(), 32);
    Setup(db.putForestTipMarker(token, tip) == Status::Ok, "forest marker alignment");
    Setup(db.setTip(token, tip.block_hash, 12, arith_uint256(12)) == Status::Ok, "active tip");
    db.close(); fs::remove_all(fixture.candidate / "blockchain/chaindb"); Clone(path, fixture.candidate / "blockchain/chaindb");
    return result;
}
void SeedDefaultForest(Fixture& f) { (void)SeedForest(f); }
void SetMetadata(Fixture& f, const std::string& key, const std::string& value) {
    for (const auto& root : {f.original, f.candidate}) Sql(root / "blockchain/utxo", "INSERT OR REPLACE INTO utxo_metadata VALUES('" + key + "','" + value + "');");
}
void ProtectedBase(const std::string& mode) {
    Fixture f(false); const auto forest = SeedForest(f); auto budget = file_limits;
    const auto hash = forest.hashes[5].GetHex();
    if (mode != "wallet" && mode != "prebase" && mode != "bad_prebase" && mode != "malformed_prebase" && mode != "orphan_prebase") {
        SetMetadata(f,"assumeutxo_lifecycle_state","fully_validated"); SetMetadata(f,"assumeutxo_fully_validated","true");
        SetMetadata(f,"assumeutxo_lc_base_height","5"); SetMetadata(f,"assumeutxo_lc_base_block",hash);
        SetMetadata(f,"assumeutxo_lc_progress_height","5");
    }
    if (mode == "wallet" || mode == "future_wallet") SetMetadata(f,"wallet_snapshot_recovery_base_height",mode == "wallet" ? "7" : "13");
    if (mode == "wrong_hash") SetMetadata(f,"assumeutxo_lc_base_block", std::string(64,'a'));
    if (mode == "bad_height") SetMetadata(f,"assumeutxo_lc_base_height","05");
    if (mode == "short_progress") SetMetadata(f,"assumeutxo_lc_progress_height","4");
    if (mode == "conflict") { SetMetadata(f,"assumeutxo_base_height","6"); SetMetadata(f,"assumeutxo_base_block",hash); }
    if (mode == "ancestry_budget") budget.max_ancestry_headers = 1;
    for (const auto& root : {f.original, f.candidate}) {
        Raw raw(root / "blockchain/chaindb", legacy);
        if (mode != "missing_promotion") raw.put("utreexo", "Massumeutxo_promoted:" + (mode == "wrong_hash" ? std::string(64,'a') : hash), "1");
        if (mode == "prebase" || mode == "bad_prebase" || mode == "malformed_prebase") {
            dinero::VectorWriter writer; writer.writeString(mode == "prebase" ? hash : std::string(64,'a')); writer.write(uint32_t{5});
            raw.put("prebase_coins", "M:base", mode == "malformed_prebase" ? "bad" : writer.release_string());
        }
        if (mode == "orphan_prebase") raw.put("prebase_coins", "unowned", "orphan");
        if (mode == "corrupt_header") {
            std::string bytes; Setup(raw.db->Get({},raw.cf("headers"),"h"+forest.hashes[8].GetHex(),&bytes).ok(), "load header");
            bytes[100] ^= 1; raw.put("headers","h"+forest.hashes[8].GetHex(),bytes);
        }
        if (mode == "missing_ancestry") Setup(raw.db->Delete({},raw.cf("headers"),"h"+forest.hashes[8].GetHex()).ok(), "remove ancestry");
        if (mode == "stale_index") raw.put("height",dinero::KH(5),std::string(64,'b'));
    }
    const bool good = mode == "promoted" || mode == "wallet" || mode == "prebase" || mode == "stale_index";
    const auto before = Inspect(f.candidate / "blockchain/chaindb");
    const auto result = MigrateShieldedDatadirCopy(f.original, f.candidate, db_limits, budget, true);
    if (good && !result.ok) std::cerr << result.error << '\n';
    CHECK(result.ok == good); if (!good) CHECK(Inspect(f.candidate / "blockchain/chaindb") == before);
}
void ForestParity(const std::string& mode) {
    Fixture f(false); const auto data = SeedForest(f); CHECK(f.Run().ok);
    // Reopen through the actual reader on both sides. These are generated
    // disposable stores: opening them here is qualification, not a migrator step.
    for (const auto& path : {f.original / "blockchain/chaindb", f.candidate / "blockchain/chaindb"}) {
        ChainDB db; CHECK(db.init(path) == Status::Ok);
        const auto resolver = [&](uint32_t h, dinero::uint256& hash) { if (h >= data.hashes.size()) return false; hash = data.hashes[h]; return true; };
        for (uint32_t height : {2, 5, 7, 10, 12}) {
            dinero::consensus::UtreexoForest restored; std::string error;
            CHECK(dinero::storage::RestoreHistoricalForest(db,height,restored,error,resolver) == Status::Ok);
            CHECK(restored.serialize() == data.states[height]);
            dinero::consensus::UtreexoHash leaf(32,0); leaf[0] = 1; leaf[1] = 1;
            const auto position = restored.findLeafPosition(leaf); CHECK(position.has_value());
            const auto proof = restored.prove(*position); CHECK(proof.has_value()); CHECK(proof->verify(leaf,restored.getRoots()));
            auto continuous = dinero::consensus::UtreexoForest::deserialize(data.states[height]);
            CHECK(proof->serialize() == continuous.prove(*position)->serialize());
            leaf[1] = 0; CHECK(restored.findLeafPosition(leaf).has_value() == (height < 3));
        }
        if (mode != "healthy") {
            rocksdb::WriteBatch batch;
            if (mode == "missing_delta") batch.Delete(dinero::MakeUtreexoDeltaUndoKey(data.hashes[7]));
            else batch.Put(dinero::MakeUtreexoDeltaUndoKey(data.hashes[7]), "corrupt");
            CHECK(db.writeBatch(ChainWriteToken::CreateForTesting(), std::move(batch), true) == Status::Ok);
            dinero::consensus::UtreexoForest refused; std::string error;
            CHECK(dinero::storage::RestoreHistoricalForest(db,7,refused,error,resolver) != Status::Ok);
            CHECK(!error.empty());
        }
    }
}

std::string ForestKey(char prefix, uint32_t height) {
    std::string key(5, '\0'); key[0] = prefix;
    for (unsigned i=0;i<4;++i) key[1+i] = static_cast<char>(height >> (24-8*i));
    return key;
}
void ForestAudit(const std::string& fault) {
    Fixture f(false); const auto data = SeedForest(f); auto budget=file_limits;
    if (fault=="budget_zero") budget.max_forest_leaves=0;
    if (fault=="budget_leaves") budget.max_forest_leaves=2;
    if (fault=="budget_records") budget.max_forest_record_bytes=128;
    if (fault=="budget_replay") budget.max_replay_blocks=11;
    if (fault=="budget_checkpoints") budget.max_checkpoints=2;
    if (fault=="budget_headers") budget.max_ancestry_headers=12;
    if (fault == "base_below_history") SetMetadata(f,"wallet_snapshot_recovery_base_height","2");
    for (const auto& root : {f.original,f.candidate}) {
        Raw raw(root / "blockchain/chaindb", legacy);
        auto erase = [&](const std::string& cf,const std::string& key) {
            Setup(raw.db->Delete({},raw.cf(cf),key).ok(),"remove forest fixture row");
        };
        if (fault == "missing_delta") erase("default",dinero::MakeUtreexoDeltaUndoKey(data.hashes[7]));
        else if (fault == "corrupt_delta") raw.put("default",dinero::MakeUtreexoDeltaUndoKey(data.hashes[11]),"corrupt");
        else if (fault == "delta_count") raw.put("default",dinero::MakeUtreexoDeltaUndoKey(data.hashes[7]), std::string("\1",1)+std::string(8,'\0')+std::string(9,'\xff'));
        else if (fault == "wrong_checkpoint") raw.put("utreexo",ForestKey('U',5),std::string(data.states[6].begin(),data.states[6].end()));
        else if (fault == "corrupt_checkpoint") raw.put("utreexo",ForestKey('U',5),"broken");
        else if (fault == "corrupt_genesis") raw.put("utreexo",ForestKey('U',0),"broken");
        else if (fault == "nonempty_genesis") raw.put("utreexo",ForestKey('U',0),std::string(data.states[1].begin(),data.states[1].end()));
        else if (fault == "valid_checksum") {
            dinero::crypto::CSHA256 hasher; hasher.Write(data.states[5].data(),data.states[5].size());
            const auto hash=hasher.Finalize(); raw.put("utreexo",ForestKey('C',5),std::string(reinterpret_cast<const char*>(hash.data()),hash.size()));
        }
        else if (fault == "checkpoint_count") {
            auto bytes=data.states[5]; for (size_t i=2;i<10;++i) bytes[i]=255;
            raw.put("utreexo",ForestKey('U',5),std::string(bytes.begin(),bytes.end()));
        }
        else if (fault == "bad_checksum") raw.put("utreexo",ForestKey('C',5),std::string(32,'x'));
        else if (fault == "orphan_checksum") raw.put("utreexo",ForestKey('C',6),std::string(32,'x'));
        else if (fault == "malformed_key") raw.put("utreexo","Ubad","preserve this");
        else if (fault == "future_checkpoint") raw.put("utreexo",ForestKey('U',13),std::string(data.states[12].begin(),data.states[12].end()));
        else if (fault == "no_checkpoint") for (unsigned h:{0,5,10}) erase("utreexo",ForestKey('U',h));
        else if (fault == "base_below_history") erase("utreexo",ForestKey('U',0));
        else if (fault == "missing_header") erase("headers","h"+data.hashes[7].GetHex());
        else if (fault == "tip_root") {
            auto value = raw.rows()["meta"]["forest_tip"]; value[36] ^= 1; raw.put("meta","forest_tip",value);
        } else if (fault == "legacy_v2") {
            auto value=data.states[5]; value.erase(value.begin()+1); value[0]=2;
            raw.put("utreexo",ForestKey('U',5),std::string(value.begin(),value.end()));
        } else if (fault == "stale_index") raw.put("height",dinero::KH(7),std::string(64,'c'));
        else if (fault != "healthy" && fault.rfind("budget_",0)!=0) Setup(false,"unknown audit case");
    }
    const bool good=fault=="healthy" || fault=="legacy_v2" || fault=="stale_index" || fault=="valid_checksum";
    const auto original=Inspect(f.original / "blockchain/chaindb"), candidate=Inspect(f.candidate / "blockchain/chaindb");
    const auto result=MigrateShieldedDatadirCopy(f.original,f.candidate,db_limits,budget,true);
    if (good && !result.ok) std::cerr << result.error << '\n';
    CHECK(result.ok==good); CHECK(result.ready==good);
    CHECK(Inspect(f.original / "blockchain/chaindb")==original);
    if (!good) CHECK(Inspect(f.candidate / "blockchain/chaindb")==candidate);
}

void ProfileMarker(const std::string& mode) {
    Fixture f;
    const auto marker = [](const fs::path& root) { return root / "regtest-pow-profile"; };
    // Use the daemon's real writer. These synthetic checks cover preservation
    // and framing, not authentication of the network or consensus parameters.
    for (const auto& root : {f.original, f.candidate})
        dinero::daemon::BindRegtestPowProfile(root, std::string(64, 'a'));
    const auto expected = Read(marker(f.original));
    if (mode == "missing_source") fs::remove(marker(f.original));
    if (mode == "missing_candidate") fs::remove(marker(f.candidate));
    if (mode == "different") Write(marker(f.candidate), "regtest-pow-profile-v1\n" + std::string(64, 'b') + "\n");
    for (const auto& root : {f.original, f.candidate}) {
        if (mode == "empty") Write(marker(root), "");
        if (mode == "short") Write(marker(root), "regtest-pow-profile-v1\na\n");
        if (mode == "nonhex") Write(marker(root), "regtest-pow-profile-v1\n" + std::string(64, 'g') + "\n");
        if (mode == "version") Write(marker(root), "regtest-pow-profile-v2\n" + std::string(64, 'a') + "\n");
        if (mode == "trailing") Write(marker(root), expected + "unexpected\n");
        if (mode == "temporary") Write(root / "regtest-pow-profile.tmp", expected);
        if (mode == "directory") { fs::remove(marker(root)); fs::create_directory(marker(root)); }
    }
    if (mode == "symlink" || mode == "hardlink") {
        fs::remove(marker(f.candidate));
        if (mode == "symlink") fs::create_symlink(marker(f.original), marker(f.candidate));
        else fs::create_hard_link(marker(f.original), marker(f.candidate));
    }
    if (mode == "resume" || mode == "resume_changed") {
        CHECK(Child({"--interrupt", f.original.string(), f.candidate.string()}) == 86);
        if (mode == "resume_changed") for (const auto& root : {f.original, f.candidate})
            Write(marker(root), "regtest-pow-profile-v1\n" + std::string(64, 'b') + "\n");
    }
    const auto source_before = Inspect(f.original / "blockchain/chaindb");
    const auto copy_before = Inspect(f.candidate / "blockchain/chaindb");
    bool changed = false;
    const auto result = f.Run(true, [&](const char* stage) {
        if (mode == "during" && !changed && std::string(stage) == "after_prepare") {
            changed = true;
            for (const auto& root : {f.original, f.candidate})
                Write(marker(root), "regtest-pow-profile-v1\n" + std::string(64, 'b') + "\n");
        }
    });
    const bool good = mode == "matching" || mode == "resume";
    if (good && !result.ok) std::cerr << result.error << '\n';
    CHECK(result.ok == good); CHECK(result.ready == good);
    CHECK(Inspect(f.original / "blockchain/chaindb") == source_before);
    if (!good && mode != "during") CHECK(Inspect(f.candidate / "blockchain/chaindb") == copy_before);
    if (mode == "during") CHECK(changed);
    if (good) for (const auto& root : {f.original, f.candidate}) CHECK(Read(marker(root)) == expected);
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    executable = fs::canonical(argv[0]);
    dinero::SelectParams(dinero::Chain::REGTEST);
    // Test-only entry points for ShieldedMigrationDaemonMarker. No production
    // launcher: the Python test creates and stops every datadir it supplies.
    if (argc == 4 && std::string(argv[1]) == "--daemon-copy") {
        const dinero::storage::ShieldedMigrationLimits db_budget{4 * 1024 * 1024, 2000, 1024 * 1024, 10000};
        const dinero::storage::ShieldedCompanionLimits file_budget{
            10000, 1024ull * 1024 * 1024, 10000, 1024 * 1024, 10000000,
            10000, 16 * 1024 * 1024, 1000000, 10000, 10000};
        const auto result = MigrateShieldedDatadirCopy(argv[2], argv[3], db_budget, file_budget, true);
        if (!result.ok || !result.ready) { std::cerr << result.error << '\n'; return 1; }
        std::cout << "READY\n"; return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "--damage-daemon-marker") {
        ChainDB db;
        Setup(db.init(fs::path(argv[2]) / "blockchain/chaindb") == Status::Ok && db.hasSeparatedShieldedState(), "open READY fixture");
        auto marker = RequiredValue(db.getShieldedTipMarker());
        const std::string field(argv[3]);
        if (field == "root") marker.shielded_root.data[0] ^= 1;
        else if (field == "tree_size") ++marker.tree_size;
        else if (field == "nullifier_count") ++marker.nullifier_count;
        else if (field == "height") ++marker.height;
        else if (field == "duplicate_nullifier" || field == "future_nullifier") {
            Setup(marker.nullifier_count == 0 && marker.height > 0, "empty daemon fixture");
            sh::Hash nf{}; nf[0] = 42;
            const auto token = ChainWriteToken::CreateForTesting();
            if (field == "duplicate_nullifier") {
                Setup(db.putShieldedNullifier(token, 0, nf.data()) == Status::Ok, "first nullifier row");
                Setup(db.putShieldedNullifier(token, marker.height, nf.data()) == Status::Ok, "duplicate nullifier row");
                marker.nullifier_count = 2;
            } else {
                Setup(db.putShieldedNullifier(token, marker.height + 1, nf.data()) == Status::Ok, "future nullifier row");
                marker.nullifier_count = 1;
            }
        }
        else return 2;
        Setup(db.putShieldedTipMarker(ChainWriteToken::CreateForTesting(), marker) == Status::Ok, "damage marker");
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--acquire") {
        DatadirGuard guard; std::string error; return guard.Acquire(argv[2], error) ? 0 : 10;
    }
    if (argc == 4 && std::string(argv[1]) == "--interrupt") {
        const auto result = MigrateShieldedDatadirCopy(argv[2], argv[3], db_limits, file_limits, true,
            [](const char* stage) { if (std::string(stage) == "after_copy") _exit(86); });
        if (!result.ok) std::cerr << result.error << '\n';
        return result.ok ? 0 : 1;
    }
    std::map<std::string, std::function<void()>> cases;
    for (const std::string mode : {"matching", "missing_source", "missing_candidate", "different", "empty", "short", "nonhex", "version", "trailing", "temporary", "directory", "symlink", "hardlink", "resume", "resume_changed", "during"}) cases["profile_" + mode] = [=] { ProfileMarker(mode); };
    for (const std::string mode : {"healthy","legacy_v2","stale_index","valid_checksum","budget_zero","budget_leaves","budget_records","budget_replay","budget_checkpoints","budget_headers","checkpoint_count","missing_delta","corrupt_delta","delta_count","wrong_checkpoint","corrupt_checkpoint","corrupt_genesis","nonempty_genesis","bad_checksum","orphan_checksum","malformed_key","future_checkpoint","no_checkpoint","base_below_history","missing_header","tip_root"}) cases["audit_"+mode] = [=] { ForestAudit(mode); };
    for (const std::string mode : {"promoted", "wallet", "prebase", "stale_index", "future_wallet", "bad_prebase", "missing_promotion", "wrong_hash", "bad_height", "short_progress", "conflict", "ancestry_budget", "missing_ancestry", "malformed_prebase", "orphan_prebase", "corrupt_header"}) cases["protected_"+mode] = [=] { ProtectedBase(mode); };
    for (const std::string mode : {"healthy", "missing_delta", "corrupt_delta"}) cases["forest_"+mode] = [=] { ForestParity(mode); };
    for (const std::string fault : {"missing", "corrupt", "missing_table", "view", "future_schema", "reorg", "recovery", "active", "active_bad", "unknown_state", "snapshot_loaded", "validating_history", "validation_stalled", "fatal_mismatch", "orphan_base", "incomplete_validated", "orphan_validated", "orphan_fatal", "unknown_reserved", "wallet_bad_height", "nul_value", "blob_value", "duplicate", "null_value", "legacy_cache", "future_cache", "cache_schema", "cache_corrupt", "rows", "value_limit", "steps", "step_exhaustion", "empty_legacy_cache", "absent_cache", "disabled"}) cases["metadata_"+fault] = [=] { Metadata(fault); };
    cases["escaped_uri"] = [] {
        Fixture f; const auto renamed = f.temp.path / "original?mode=rw#%";
        fs::rename(f.original,renamed); f.original=renamed;
        CHECK(f.Run().ok); CHECK(!fs::exists(renamed / "blockchain/utxo-shm"));
    };
    cases["inspect"] = [] { Success(true); }; cases["move"] = [] { Success(false); };
    cases["ownership"] = Ownership; cases["resume"] = [] { Interrupt(""); };
    cases["no_unbound_resume"] = CannotBypassBinding;
    for (const std::string change : {"source", "candidate", "both"}) cases["resume_changed_" + change] = [=] { Interrupt(change); };
    for (const std::string change : {"replace_lock", "add_marker", "bytes"}) cases["during_" + change] = [=] { During(change); };
    for (const std::string fault : {"source_running", "copy_running", "recovery", "reindex", "reindex_temporary", "maintenance", "different_body", "missing_header", "extra_companion", "missing_lock", "lock_symlink", "file_symlink", "file_hardlink", "same", "root_symlink", "entry_budget", "byte_budget", "zero_budget", "dirty_sqlite"}) cases[fault] = [=] { Refusal(fault); };
    if (argc == 2 && std::string(argv[1]) == "--list") { for (const auto& [name, test] : cases) std::cout << name << '\n'; return 0; }
    unsigned passed = 0, failed = 0;
    for (const auto& [name, test] : cases) {
        if (argc == 2 && name != argv[1]) continue;
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const Failure& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
        catch (const std::exception& e) { std::cerr << "SETUP ERROR " << name << ": " << e.what() << '\n'; return 2; }
    }
    if (!passed && !failed) return 2;
    std::cout << passed << " passed, " << failed << " failed\n"; return failed ? 1 : 0;
}

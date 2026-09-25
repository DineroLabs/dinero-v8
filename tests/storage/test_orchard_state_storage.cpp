// Generated temporary stores only. Frontier bytes here are opaque storage
// fixtures, not a claim that the Orchard tree or block validator is integrated.
#include "shielded_store_fixture.h"
#include "consensus/tx_validation.h"
#include <rocksdb/env.h>
#include <atomic>
#include <limits>
#include <ctime>
#include <sys/wait.h>
using namespace shielded_store_fixture;
using dinero::storage::OrchardStoredState;
using dinero::uint256;

uint256 H(uint8_t value) { uint256 h; h.data[0] = value; return h; }
OrchardStoredState State(uint8_t block, uint32_t height = 10) {
    return {height, H(block), H(90), 40, 2, std::string("frontier\0fixture", 16)};
}
const auto token = ChainWriteToken::CreateForTesting();
using dinero::consensus::OrchardValueFlow;
using dinero::consensus::ApplyOrchardValueFlows;
const std::vector<OrchardValueFlow> funding{{100, 60, 0}};
void TurnstileArithmetic() {
    using dinero::consensus::MAX_MONEY;
    CHECK(RequiredValue(ApplyOrchardValueFlows(0, {{100, 60, 5}})) == 35);
    CHECK(RequiredValue(ApplyOrchardValueFlows(40, {{0, 0, 7}})) == 33);
    CHECK(RequiredValue(ApplyOrchardValueFlows(40, {{0, 39, 1}})) == 0);
    CHECK(ApplyOrchardValueFlows(40, {{0, 40, 1}}).status() == Status::Invalid);
    CHECK(ApplyOrchardValueFlows(0, {{0, 1, 0}, {1, 0, 0}}).status() == Status::Invalid);
    CHECK(RequiredValue(ApplyOrchardValueFlows(0, {{1, 0, 0}, {0, 1, 0}})) == 0);
    CHECK(ApplyOrchardValueFlows(100, {{0, 60, 0}, {0, 50, 0}}).status() == Status::Invalid);
    CHECK(RequiredValue(ApplyOrchardValueFlows(0, {{MAX_MONEY, 0, 0}, {0, MAX_MONEY, 0}, {MAX_MONEY, 0, 0}})) == MAX_MONEY);
    // Independent widened-integer oracle at the money and machine bounds.
    const std::vector<uint64_t> values{0, 1, 39, 40, MAX_MONEY - 1, MAX_MONEY, MAX_MONEY + 1,
                                      std::numeric_limits<uint64_t>::max()};
    for (const auto parent : values) for (const auto in : values)
    for (const auto out : values) for (const auto fee : values) {
        const __int128 wanted = __int128(parent) + __int128(in) - __int128(out) - __int128(fee);
        const bool valid = parent <= MAX_MONEY && in <= MAX_MONEY &&
            __int128(out) + __int128(fee) <= MAX_MONEY && wanted >= 0 && wanted <= MAX_MONEY;
        const auto result = ApplyOrchardValueFlows(parent, {{in, out, fee}});
        CHECK(result.ok() == valid);
        if (valid) CHECK(result.value() == static_cast<uint64_t>(wanted));
    }
}
dinero::Coin Input() { dinero::Coin c; c.amount = 100; c.script_pubkey = "input"; c.height = 1; return c; }
dinero::Coin Output() { dinero::Coin c; c.amount = 60; c.script_pubkey = "output"; c.height = 10; return c; }
void Initial(ChainDB& db) {
    rocksdb::WriteBatch batch;
    CHECK(db.putCoin(token, H(11), 0, Input(), &batch) == Status::Ok);
    CHECK(db.setTip(token, H(9), 9, dinero::arith_uint256(9), &batch) == Status::Ok);
    CHECK(db.setValidatedTip(token, H(9), 9, &batch) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(batch), true) == Status::Ok);
}
void Companion(ChainDB& db, rocksdb::WriteBatch& batch, bool connect) {
    CHECK(db.deleteCoin(token, H(connect ? 11 : 12), 0, &batch) == Status::Ok);
    CHECK(db.putCoin(token, H(connect ? 12 : 11), 0, connect ? Output() : Input(), &batch) == Status::Ok);
    CHECK(db.setTip(token, H(connect ? 10 : 9), connect ? 10 : 9,
                    dinero::arith_uint256(connect ? 10 : 9), &batch) == Status::Ok);
    CHECK(db.setValidatedTip(token, H(connect ? 10 : 9), connect ? 10 : 9, &batch) == Status::Ok);
}
void CheckOld(ChainDB& db) {
    CHECK(db.getOrchardState().status() == Status::NotFound);
    CHECK(db.getOrchardNullifierOwner(H(31)).status() == Status::NotFound);
    CHECK(db.getOrchardAnchorReferences(H(90)).status() == Status::NotFound);
    CHECK(RequiredValue(db.getCoin(H(11), 0)).amount == 100);
    CHECK(db.getCoin(H(12), 0).status() == Status::NotFound);
    CHECK(RequiredValue(db.getTip()).height == 9);
}
void CheckNew(ChainDB& db) {
    CHECK(RequiredValue(db.getOrchardState()) == State(10));
    CHECK(RequiredValue(db.getOrchardNullifierOwner(H(31))) == H(10));
    CHECK(RequiredValue(db.getOrchardAnchorReferences(H(90))) == 1);
    CHECK(db.getCoin(H(11), 0).status() == Status::NotFound);
    CHECK(RequiredValue(db.getCoin(H(12), 0)).amount == 60);
    CHECK(RequiredValue(db.getTip()).height == 10);
}
void CheckRestored(const Rows& original, Rows restored, uint32_t before_restore) {
    // setTip records the time of the write, not a consensus block timestamp.
    // All other bytes must restore exactly, including the rest of this record.
    const auto& old_tip = original.at("meta").at("tip");
    auto& new_tip = restored.at("meta").at("tip");
    CHECK(new_tip.size() == old_tip.size() && new_tip.size() >= 4);
    uint32_t written_at = 0;
    for (unsigned i = 0; i < 4; ++i)
        written_at |= uint32_t(static_cast<unsigned char>(new_tip[new_tip.size() - 4 + i])) << (8 * i);
    CHECK(written_at >= before_restore && written_at <= static_cast<uint32_t>(std::time(nullptr)));
    new_tip.replace(new_tip.size() - 4, 4, old_tip, old_tip.size() - 4, 4);
    CHECK(restored == original);
}
void RoundTrip() {
    TempDir temp; Seed(temp.path); ChainDB db; CHECK(db.init(temp.path) == Status::Ok); Initial(db);
    db.close(); const auto original = Inspect(temp.path); CHECK(db.init(temp.path) == Status::Ok);
    {
        rocksdb::WriteBatch abandoned; Companion(db, abandoned, true);
        CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {H(32), H(31)}, funding, abandoned) == Status::Ok);
        CheckOld(db); // Stage does not touch RocksDB or the caller's live state.
    }
    db.close(); CHECK(Inspect(temp.path) == original); CHECK(db.init(temp.path) == Status::Ok);
    rocksdb::WriteBatch connect; Companion(db, connect, true);
    CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {H(32), H(31)}, funding, connect) == Status::Ok);
    const auto staged = connect.Data();
    CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {}, funding, connect) == Status::Invalid);
    CHECK(connect.Data() == staged);
    CHECK(db.writeBatch(token, std::move(connect), true) == Status::Ok);
    db.close(); CHECK(db.init(temp.path) == Status::Ok); CheckNew(db);
    CHECK(RequiredValue(db.getOrchardNullifierOwner(H(32))) == H(10));
    // Repeated roots on a later empty block need reference counts, not a set.
    rocksdb::WriteBatch second;
    CHECK(db.stageOrchardConnect(token, State(10), State(20, 11), {}, {}, second) == Status::Ok);
    CHECK(db.setTip(token, H(20), 11, dinero::arith_uint256(11), &second) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(second), true) == Status::Ok);
    CHECK(RequiredValue(db.getOrchardAnchorReferences(H(90))) == 2);
    rocksdb::WriteBatch undo_second;
    CHECK(db.stageOrchardDisconnect(token, State(20, 11), undo_second) == Status::Ok);
    CHECK(db.setTip(token, H(10), 10, dinero::arith_uint256(10), &undo_second) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(undo_second), true) == Status::Ok); CheckNew(db);
    const auto before_restore = static_cast<uint32_t>(std::time(nullptr));
    rocksdb::WriteBatch disconnect; Companion(db, disconnect, false);
    CHECK(db.stageOrchardDisconnect(token, State(10), disconnect) == Status::Ok);
    CheckNew(db); CHECK(db.writeBatch(token, std::move(disconnect), true) == Status::Ok);
    db.close(); CheckRestored(original, Inspect(temp.path), before_restore); CHECK(db.init(temp.path) == Status::Ok); CheckOld(db);
    // The same valid transition can be connected again after complete undo.
    rocksdb::WriteBatch again; Companion(db, again, true);
    CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {H(31)}, funding, again) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(again), true) == Status::Ok); CheckNew(db);
}
void Rejects() {
    TempDir temp; Seed(temp.path); ChainDB db; CHECK(db.init(temp.path) == Status::Ok); Initial(db);
    auto reject = [&](auto parent, auto next, auto nfs, Status want) {
        rocksdb::WriteBatch b; Companion(db, b, true); b.SetSavePoint();
        const auto staged = b.Data();
        CHECK(db.stageOrchardConnect(token, parent, next, nfs, parent ? std::vector<OrchardValueFlow>{} : funding, b) == want);
        CHECK(b.Data() == staged); CHECK(b.RollbackToSavePoint().ok()); CHECK(b.Data() == staged);
    };
    using Parent = std::optional<OrchardStoredState>; using Nfs = std::vector<uint256>;
    reject(Parent{}, State(10), Nfs{H(31), H(31)}, Status::Invalid);
    auto bad = State(10); bad.pool_balance = dinero::consensus::MAX_MONEY + 1;
    reject(Parent{}, bad, Nfs{}, Status::Invalid);
    bad = State(10); bad.frontier.resize(dinero::storage::ORCHARD_STORED_FRONTIER_LIMIT + 1);
    reject(Parent{}, bad, Nfs{}, Status::Invalid);
    bad = State(10); bad.tree_size = (uint64_t{1} << 32) + 1;
    reject(Parent{}, bad, Nfs{}, Status::Invalid);
    reject(Parent{}, State(10), Nfs(dinero::storage::ORCHARD_STORED_BLOCK_NULLIFIER_LIMIT + 1), Status::Invalid);
    rocksdb::WriteBatch initial;
    const auto first_status = db.stageOrchardConnect(token, std::nullopt, State(10), {H(31)}, funding, initial);
    if (first_status != Status::Ok) throw Failure(std::string("initial Orchard stage: ") + dinero::StatusToString(first_status));
    CHECK(db.writeBatch(token, std::move(initial), true) == Status::Ok);
    reject(Parent{}, State(10), Nfs{}, Status::AlreadyExists);
    reject(Parent{State(10)}, State(20, 11), Nfs{H(31)}, Status::AlreadyExists);
    reject(Parent{State(12)}, State(20, 11), Nfs{}, Status::Invalid);
    reject(Parent{State(10)}, State(20, 12), Nfs{}, Status::Invalid);
    bad = State(20, 11); bad.anchor = H(91); // Same tree size cannot silently change root/frontier.
    reject(Parent{State(10)}, bad, Nfs{}, Status::Invalid);
    rocksdb::WriteBatch wrong_tip; Companion(db, wrong_tip, false); const auto saved = wrong_tip.Data();
    CHECK(db.stageOrchardDisconnect(token, State(12), wrong_tip) == Status::Invalid);
    CHECK(wrong_tip.Data() == saved);
    // Zero is a field element, not a storage sentinel for an absent anchor.
    // Cryptographic root eligibility must be decided by the Orchard tree.
    auto zero_root = State(20, 11); zero_root.tree_size = 3; zero_root.anchor = H(0);
    rocksdb::WriteBatch zero_batch;
    CHECK(db.stageOrchardConnect(token, State(10), zero_root, {}, {}, zero_batch) == Status::Ok);
}
void LayoutRefusal() {
    TempDir temp; Seed(temp.path, false, std::nullopt);
    const auto before = Inspect(temp.path); ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
    rocksdb::WriteBatch b; const auto empty = b.Data();
    CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {}, funding, b) == Status::Invalid);
    CHECK(b.Data() == empty); CHECK(db.getOrchardState().status() == Status::Invalid);
    db.close(); CHECK(Inspect(temp.path) == before);
    CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {}, funding, b) == Status::Internal);
}
void TurnstileStorage() {
    TempDir temp; Seed(temp.path); ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
    rocksdb::WriteBatch b;
    CHECK(db.putUtreexoMeta(token, "caller-entry", "preserved", &b) == Status::Ok);
    const auto saved = b.Data();
    // No legacy pool carry-forward and no unexplained next-balance assignment.
    CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {}, {}, b) == Status::Invalid);
    CHECK(b.Data() == saved);
    auto wrong = State(10); wrong.pool_balance = 41;
    CHECK(db.stageOrchardConnect(token, std::nullopt, wrong, {}, funding, b) == Status::Invalid);
    CHECK(b.Data() == saved);
    CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {}, funding, b) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(b), true) == Status::Ok);
    auto drained = State(20, 11); drained.pool_balance = 0;
    rocksdb::WriteBatch withdrawal;
    const auto empty = withdrawal.Data();
    CHECK(db.stageOrchardConnect(token, State(10), drained, {}, {{0, 40, 1}}, withdrawal) == Status::Invalid);
    CHECK(withdrawal.Data() == empty && RequiredValue(db.getOrchardState()).pool_balance == 40);
    CHECK(db.stageOrchardConnect(token, State(10), drained, {}, {{0, 39, 1}}, withdrawal) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(withdrawal), true) == Status::Ok);
    db.close(); CHECK(db.init(temp.path) == Status::Ok); CHECK(RequiredValue(db.getOrchardState()).pool_balance == 0);
    rocksdb::WriteBatch undo;
    CHECK(db.stageOrchardDisconnect(token, drained, undo) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(undo), true) == Status::Ok);
    CHECK(RequiredValue(db.getOrchardState()).pool_balance == 40);
}
void BranchReplacement() {
    TempDir temp; Seed(temp.path); ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
    auto connect = [&](auto parent, const OrchardStoredState& next, std::vector<uint256> nfs, std::vector<OrchardValueFlow> flows) {
        rocksdb::WriteBatch b;
        CHECK(db.stageOrchardConnect(token, parent, next, nfs, flows, b) == Status::Ok);
        CHECK(db.setTip(token, next.block_hash, next.height, dinero::arith_uint256(next.height), &b) == Status::Ok);
        CHECK(db.writeBatch(token, std::move(b), true) == Status::Ok);
    };
    connect(std::optional<OrchardStoredState>{}, State(10), {H(31)}, funding);
    auto left = State(20, 11); left.tree_size = 3; left.anchor = H(91); left.frontier = "left"; left.pool_balance = 20;
    connect(std::optional<OrchardStoredState>{State(10)}, left, {H(32)}, {{0, 20, 0}});
    db.close(); CHECK(db.init(temp.path) == Status::Ok);
    rocksdb::WriteBatch undo;
    CHECK(db.stageOrchardDisconnect(token, left, undo) == Status::Ok);
    CHECK(db.setTip(token, H(10), 10, dinero::arith_uint256(10), &undo) == Status::Ok);
    CHECK(db.writeBatch(token, std::move(undo), true) == Status::Ok);
    CHECK(RequiredValue(db.getOrchardState()) == State(10));
    CHECK(db.getOrchardAnchorReferences(H(91)).status() == Status::NotFound);
    CHECK(db.getOrchardNullifierOwner(H(32)).status() == Status::NotFound);
    auto right = State(21, 11); right.tree_size = 4; right.anchor = H(92); right.frontier = "right"; right.pool_balance = 35;
    connect(std::optional<OrchardStoredState>{State(10)}, right, {H(32)}, {{0, 5, 0}});
    db.close(); CHECK(db.init(temp.path) == Status::Ok);
    CHECK(RequiredValue(db.getOrchardState()) == right);
    CHECK(RequiredValue(db.getOrchardNullifierOwner(H(32))) == H(21));
    CHECK(RequiredValue(db.getOrchardNullifierOwner(H(31))) == H(10));
    CHECK(RequiredValue(db.getOrchardAnchorReferences(H(92))) == 1);
    CHECK(RequiredValue(db.getOrchardAnchorReferences(H(90))) == 1);
}
void CorruptUndo() {
    TempDir temp; Seed(temp.path);
    { ChainDB db; CHECK(db.init(temp.path) == Status::Ok); Initial(db);
      rocksdb::WriteBatch b; Companion(db, b, true);
      CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {H(31)}, funding, b) == Status::Ok);
      CHECK(db.writeBatch(token, std::move(b), true) == Status::Ok); }
    auto names = legacy; names.push_back(shielded);
    { Raw raw(temp.path, names); const auto hash = H(10);
      raw.put(shielded, std::string("O1U") + std::string(reinterpret_cast<const char*>(hash.data), 32), "truncated"); }
    const auto before = Inspect(temp.path); ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
    rocksdb::WriteBatch b; Companion(db, b, false); const auto saved = b.Data();
    CHECK(db.stageOrchardDisconnect(token, State(10), b) == Status::Corruption); CHECK(b.Data() == saved);
    db.close(); CHECK(Inspect(temp.path) == before);
}
void CorruptRecords() {
    for (const auto* mode : {"state-tail", "state-length", "state-version", "undo-tail",
                             "nullifier-owner", "nullifier-length", "anchor-zero", "missing-state"}) {
        TempDir temp; Seed(temp.path);
        { ChainDB db; CHECK(db.init(temp.path) == Status::Ok); Initial(db);
          rocksdb::WriteBatch b; Companion(db, b, true);
          CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {H(31)}, funding, b) == Status::Ok);
          CHECK(db.writeBatch(token, std::move(b), true) == Status::Ok); }
        auto records = Inspect(temp.path);
        auto key = [](char type, uint8_t id) {
            const auto h = H(id); return std::string("O1") + type + std::string(reinterpret_cast<const char*>(h.data), 32);
        };
        auto names = legacy; names.push_back(shielded);
        { Raw raw(temp.path, names);
          const std::string m(mode);
          if (m == "missing-state") CHECK(raw.db->Delete({}, raw.cf(shielded), "O1S").ok());
          else if (m == "state-tail") raw.put(shielded, "O1S", records[shielded]["O1S"] + "x");
          else if (m == "state-length") {
              auto bytes = records[shielded]["O1S"]; bytes.replace(88, 4, std::string(4, '\xff'));
              raw.put(shielded, "O1S", bytes);
          } else if (m == "state-version") {
              auto bytes = records[shielded]["O1S"]; bytes[3] = '2'; raw.put(shielded, "O1S", bytes);
          } else if (m == "undo-tail") raw.put(shielded, key('U', 10), records[shielded][key('U', 10)] + "x");
          else if (m == "nullifier-owner") {
              const auto owner = H(99); raw.put(shielded, key('N', 31), std::string(reinterpret_cast<const char*>(owner.data), 32));
          } else if (m == "nullifier-length") raw.put(shielded, key('N', 31), "");
          else if (m == "anchor-zero") raw.put(shielded, key('A', 90), std::string(8, '\0'));
        }
        const auto before = Inspect(temp.path); ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
        rocksdb::WriteBatch b; Companion(db, b, false); const auto saved = b.Data();
        if (std::string(mode) == "missing-state")
            CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {}, funding, b) == Status::Corruption);
        else CHECK(db.stageOrchardDisconnect(token, State(10), b) == Status::Corruption);
        CHECK(b.Data() == saved); db.close(); CHECK(Inspect(temp.path) == before);
    }
}
struct WalFailureEnv : rocksdb::EnvWrapper {
    std::atomic<bool> armed{false};
    std::atomic<unsigned> failures{0};
    WalFailureEnv() : EnvWrapper(rocksdb::Env::Default()) {}
    struct File : rocksdb::WritableFileWrapper {
        std::unique_ptr<rocksdb::WritableFile> owned;
        WalFailureEnv& env;
        File(std::unique_ptr<rocksdb::WritableFile> file, WalFailureEnv& e)
            : WritableFileWrapper(file.get()), owned(std::move(file)), env(e) {}
        rocksdb::Status Append(const rocksdb::Slice& bytes) override {
            if (env.armed) { ++env.failures; return rocksdb::Status::IOError("test WAL append refused"); }
            return owned->Append(bytes);
        }
        rocksdb::Status Append(const rocksdb::Slice& bytes, const rocksdb::DataVerificationInfo& info) override {
            if (env.armed) { ++env.failures; return rocksdb::Status::IOError("test WAL append refused"); }
            return owned->Append(bytes, info);
        }
    };
    rocksdb::Status NewWritableFile(const std::string& path,
        std::unique_ptr<rocksdb::WritableFile>* out, const rocksdb::EnvOptions& options) override {
        auto status = target()->NewWritableFile(path, out, options);
        if (status.ok() && std::filesystem::path(path).extension() == ".log")
            *out = std::make_unique<File>(std::move(*out), *this);
        return status;
    }
};
void WriteFailure() {
    TempDir temp; Seed(temp.path); WalFailureEnv env;
    {
        ChainDB db; db.setEnvForTesting(&env); CHECK(db.init(temp.path) == Status::Ok); Initial(db);
        rocksdb::WriteBatch b; Companion(db, b, true);
        CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {H(31)}, funding, b) == Status::Ok);
        env.armed = true;
        const auto status = db.writeBatch(token, std::move(b), true);
        env.armed = false;
        CHECK(status != Status::Ok); CHECK(env.failures > 0); CheckOld(db);
    }
    ChainDB db; CHECK(db.init(temp.path) == Status::Ok); CheckOld(db);
}
// Exit a separate generated-store writer without destructors, before or after
// the synchronous outer commit. This is process-loss recovery, not power loss.
void ProcessBoundary(bool commit) {
    TempDir temp; Seed(temp.path);
    { ChainDB db; CHECK(db.init(temp.path) == Status::Ok); Initial(db); }
    const auto pid = fork(); CHECK(pid >= 0);
    if (pid == 0) {
        try {
            ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
            rocksdb::WriteBatch b; Companion(db, b, true);
            CHECK(db.stageOrchardConnect(token, std::nullopt, State(10), {H(31)}, funding, b) == Status::Ok);
            if (commit) CHECK(db.writeBatch(token, std::move(b), true) == Status::Ok);
            _Exit(0);
        } catch (...) { _Exit(2); }
    }
    int status = 0; CHECK(waitpid(pid, &status, 0) == pid); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
    if (commit) CheckNew(db); else CheckOld(db);
}
int main() {
    try {
        TurnstileArithmetic(); TurnstileStorage(); LayoutRefusal(); Rejects(); RoundTrip(); BranchReplacement(); CorruptUndo(); CorruptRecords(); WriteFailure(); ProcessBoundary(false); ProcessBoundary(true);
        std::cout << "Orchard storage: layout, rejected/stale transitions, atomic companion writes, repeated anchors, exact undo, corruption and process-loss boundaries passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

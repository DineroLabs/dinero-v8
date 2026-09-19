// Corrupt keys are injected only into generated temporary RocksDB stores.
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include <rocksdb/db.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdlib>
#include <unistd.h>

using dinero::ChainDB;
using dinero::ChainWriteToken;
using dinero::Status;
using Records = std::map<std::string, std::map<std::string, std::string>>;

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
#define CHECK(c) do { if (!(c)) throw Failure(std::string(__func__) + ": " + #c); } while (false)
void Setup(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        auto pattern = (std::filesystem::temp_directory_path() / "dinero_nullifier_scan_XXXXXX").string();
        const auto* dir = mkdtemp(pattern.data());
        Setup(dir != nullptr, "mkdtemp");
        path = dir;
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

struct RawStore {
    std::unique_ptr<rocksdb::DB> db;
    std::vector<std::string> names;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    explicit RawStore(const std::filesystem::path& path, bool writable = false) {
        rocksdb::Options options;
        Setup(rocksdb::DB::ListColumnFamilies(options, path.string(), &names).ok(), "list CFs");
        std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
        for (const auto& name : names) descriptors.emplace_back(name, options);
        rocksdb::DB* raw = nullptr;
        auto status = writable
            ? rocksdb::DB::Open(options, path.string(), descriptors, &handles, &raw)
            : rocksdb::DB::OpenForReadOnly(options, path.string(), descriptors, &handles, &raw);
        db.reset(raw);
        Setup(status.ok(), "raw open");
    }
    ~RawStore() { for (auto* handle : handles) db->DestroyColumnFamilyHandle(handle); }
    rocksdb::ColumnFamilyHandle* handle(const std::string& name) {
        for (size_t i = 0; i < names.size(); ++i) if (names[i] == name) return handles[i];
        throw std::runtime_error("missing CF");
    }
};

Records Inspect(const std::filesystem::path& path) {
    RawStore raw(path);
    Records result;
    for (size_t i = 0; i < raw.names.size(); ++i) {
        auto& rows = result[raw.names[i]];
        std::unique_ptr<rocksdb::Iterator> it(raw.db->NewIterator(rocksdb::ReadOptions(), raw.handles[i]));
        for (it->SeekToFirst(); it->Valid(); it->Next()) rows[it->key().ToString()] = it->value().ToString();
        Setup(it->status().ok(), "raw scan");
    }
    return result;
}

std::array<uint8_t, 32> Nullifier(uint8_t byte) {
    std::array<uint8_t, 32> nf{}; nf.fill(byte); return nf;
}

std::string Key(uint32_t height, size_t nullifier_bytes) {
    std::string key(5, '\0'); key[0] = 'N';
    for (unsigned i = 0; i < 4; ++i) key[1 + i] = static_cast<char>(height >> (24 - 8 * i));
    key.append(nullifier_bytes, '\x42');
    return key;
}

void Seed(const std::filesystem::path& path, const std::string& malformed = {}) {
    const auto token = ChainWriteToken::CreateForTesting();
    {
        ChainDB db;
        Setup(db.init(path) == Status::Ok, "initialize fixture");
        Setup(db.putShieldedNullifier(token, 1, Nullifier(0x11).data()) == Status::Ok, "seed h1");
        Setup(db.putShieldedNullifier(token, 3, Nullifier(0x33).data()) == Status::Ok, "seed h3");
        Setup(db.putUtreexoMeta(token, "shielded_frontier", "preserved-frontier") == Status::Ok, "seed frontier");
        Setup(db.putUtreexoCheckpointWithChecksum(token, 1, {1, 2, 3}) == Status::Ok, "seed checkpoint");
    }
    RawStore raw(path, true);
    rocksdb::WriteOptions opts; opts.sync = true;
    // Unrelated namespaces immediately before and after N must terminate/avoid
    // a scan normally regardless of their key lengths.
    for (const auto& key : {std::string("M"), std::string("O"), std::string(37, 'O')}) {
        Setup(raw.db->Put(opts, raw.handle("utreexo"), key, "unrelated").ok(), "seed neighbors");
    }
    if (!malformed.empty()) {
        Setup(raw.db->Put(opts, raw.handle("utreexo"), malformed, "preserve-corrupt-bytes").ok(), "inject malformed key");
    }
}

enum class Operation { Count, Visit, DeleteAll, DeleteAbove, BatchAll, BatchAbove };

void CorruptCase(const std::string& key, Operation operation) {
    TempDir temp;
    Seed(temp.path, key);
    auto expected = Inspect(temp.path);
    const auto token = ChainWriteToken::CreateForTesting();
    {
        ChainDB db; Setup(db.init(temp.path) == Status::Ok, "open corrupt fixture");
        if (operation == Operation::Count) {
            CHECK(db.countShieldedNullifiers().status() == Status::Corruption);
        } else if (operation == Operation::Visit) {
            std::vector<uint32_t> visited;
            CHECK(db.forEachShieldedNullifier([&](uint32_t height, const uint8_t*) {
                visited.push_back(height); return true;
            }) == Status::Corruption);
            // In particular the malformed middle key must not be skipped.
            if (key == Key(2, 31) || key == Key(2, 33)) CHECK(visited == std::vector<uint32_t>{1});
        } else if (operation == Operation::DeleteAll || operation == Operation::DeleteAbove) {
            const auto result = operation == Operation::DeleteAll
                ? db.deleteAllShieldedNullifiers(token)
                : db.deleteShieldedNullifiersAboveHeight(token, 0);
            CHECK(result.status() == Status::Corruption);
        } else {
            rocksdb::WriteBatch batch;
            Setup(db.putUtreexoMeta(token, "caller_before_savepoint", "keep", &batch) == Status::Ok, "stage caller base");
            const auto base = batch.Data();
            batch.SetSavePoint();
            Setup(db.putUtreexoMeta(token, "caller_after_savepoint", "keep-too", &batch) == Status::Ok, "stage caller tail");
            const auto before = batch.Data();
            const auto result = operation == Operation::BatchAll
                ? db.deleteAllShieldedNullifiers(token, &batch)
                : db.deleteShieldedNullifiersAboveHeight(token, 0, &batch);
            CHECK(result.status() == Status::Corruption);
            CHECK(batch.Data() == before);
            // The callee must preserve the caller's savepoint stack as well as
            // its serialized batch contents, even after staging valid deletes.
            CHECK(batch.RollbackToSavePoint().ok());
            CHECK(batch.Data() == base);
            CHECK(db.writeBatch(token, std::move(batch), true) == Status::Ok);
            expected["utreexo"]["Mcaller_before_savepoint"] = "keep";
        }
    }
    // Closing/reopening and comparing every CF also proves no deletion of the
    // bad key, valid nullifiers, checkpoints, or neighboring metadata occurred.
    CHECK(Inspect(temp.path) == expected);
}

void ValidCase(Operation operation) {
    TempDir temp; Seed(temp.path);
    auto expected = Inspect(temp.path);
    const auto token = ChainWriteToken::CreateForTesting();
    {
        ChainDB db; Setup(db.init(temp.path) == Status::Ok, "open valid fixture");
        CHECK(db.countShieldedNullifiers().ok());
        CHECK(db.countShieldedNullifiers().value() == 2);
        std::vector<uint32_t> heights;
        CHECK(db.forEachShieldedNullifier([&](uint32_t height, const uint8_t* nf) {
            heights.push_back(height);
            CHECK(std::equal(nf, nf + 32, Nullifier(height == 1 ? 0x11 : 0x33).begin()));
            return true;
        }) == Status::Ok);
        CHECK((heights == std::vector<uint32_t>{1, 3}));
        if (operation != Operation::Count) {
            const bool all = operation == Operation::DeleteAll || operation == Operation::BatchAll;
            const bool batched = operation == Operation::BatchAll || operation == Operation::BatchAbove;
            rocksdb::WriteBatch batch;
            Setup(db.putUtreexoMeta(token, "caller", "keep", &batch) == Status::Ok, "stage caller");
            const auto before = batch.Data();
            batch.SetSavePoint();
            auto result = all ? db.deleteAllShieldedNullifiers(token, batched ? &batch : nullptr)
                              : db.deleteShieldedNullifiersAboveHeight(token, 1, batched ? &batch : nullptr);
            CHECK(result.ok() && result.value() == (all ? 2U : 1U));
            if (batched) {
                CHECK(db.countShieldedNullifiers().value() == 2); // not committed yet
                // Successful calls must also leave the caller's savepoint on top.
                CHECK(batch.RollbackToSavePoint().ok());
                CHECK(batch.Data() == before);
                result = all ? db.deleteAllShieldedNullifiers(token, &batch)
                             : db.deleteShieldedNullifiersAboveHeight(token, 1, &batch);
                CHECK(result.ok() && result.value() == (all ? 2U : 1U));
                CHECK(db.writeBatch(token, std::move(batch), true) == Status::Ok);
                expected["utreexo"]["Mcaller"] = "keep";
            }
            auto& rows = expected["utreexo"];
            for (auto it = rows.begin(); it != rows.end();) {
                if (it->first[0] == 'N' && (all || it->first[4] == 3)) it = rows.erase(it);
                else ++it;
            }
            CHECK(db.countShieldedNullifiers().value() == (all ? 0U : 1U));
        }
    }
    CHECK(Inspect(temp.path) == expected);
}

struct Case { std::string name; std::function<void()> run; };
std::vector<Case> Cases() {
    std::vector<Case> cases;
    const std::vector<std::pair<std::string, Operation>> operations{
        {"count", Operation::Count}, {"visit", Operation::Visit},
        {"delete_all", Operation::DeleteAll}, {"delete_above", Operation::DeleteAbove},
        {"batch_all", Operation::BatchAll}, {"batch_above", Operation::BatchAbove}};
    const std::vector<std::pair<std::string, std::string>> malformed{
        {"bare_prefix", "N"}, {"partial_height", std::string("N\0", 2)},
        {"before_first", Key(1, 0)}, {"short_middle", Key(2, 31)},
        {"long_middle", Key(2, 33)}, {"after_last", Key(4, 33)}};
    for (const auto& [name, key] : malformed) for (const auto& [op_name, op] : operations) {
        // The cutoff API deliberately scans only its requested suffix; a
        // malformed key sorting below that suffix is outside its contract.
        if (key.size() < 5 && (op == Operation::DeleteAbove || op == Operation::BatchAbove)) continue;
        cases.push_back({name + "_" + op_name, [key, op] { CorruptCase(key, op); }});
    }
    for (auto [name, op] : operations) {
        if (op != Operation::Visit) cases.push_back({"valid_" + name, [op] { ValidCase(op); }});
    }
    cases.push_back({"early_visitor_stop", [] {
        TempDir temp; Seed(temp.path, Key(2, 31));
        ChainDB db; Setup(db.init(temp.path) == Status::Ok, "open early-stop fixture");
        unsigned visits = 0;
        CHECK(db.forEachShieldedNullifier([&](uint32_t height, const uint8_t*) {
            CHECK(height == 1); ++visits; return false;
        }) == Status::Ok);
        CHECK(visits == 1); // early stop never claims to validate the suffix
    }});
    cases.push_back({"max_height_noop", [] {
        TempDir temp; Seed(temp.path, Key(2, 31));
        auto expected = Inspect(temp.path);
        const auto token = ChainWriteToken::CreateForTesting();
        {
            ChainDB db; Setup(db.init(temp.path) == Status::Ok, "open no-op fixture");
            rocksdb::WriteBatch batch;
            const auto before = batch.Data();
            for (auto* wb : {static_cast<rocksdb::WriteBatch*>(nullptr), &batch}) {
                auto result = db.deleteShieldedNullifiersAboveHeight(token, UINT32_MAX, wb);
                CHECK(result.ok() && result.value() == 0);
                CHECK(batch.Data() == before);
            }
        }
        CHECK(Inspect(temp.path) == expected);
    }});
    return cases;
}

int main(int argc, char** argv) {
    const auto cases = Cases();
    if (argc == 2 && std::string(argv[1]) == "--list") {
        for (const auto& c : cases) std::cout << c.name << '\n';
        return 0;
    }
    unsigned passed = 0, failed = 0;
    for (const auto& c : cases) {
        if (argc == 2 && c.name != argv[1]) continue;
        try { c.run(); ++passed; std::cout << "PASS " << c.name << '\n'; }
        catch (const Failure& e) { ++failed; std::cerr << "FAIL " << c.name << ": " << e.what() << '\n'; }
        catch (const std::exception& e) { std::cerr << "SETUP ERROR " << c.name << ": " << e.what() << '\n'; return 2; }
    }
    if (passed + failed == 0 || argc > 2) return 2;
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}

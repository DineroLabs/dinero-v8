#pragma once
// Generated storage fixtures only; never a migration tool.
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include <rocksdb/db.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace shielded_store_fixture {
using dinero::ChainDB;
using dinero::ChainWriteToken;
using dinero::Status;
using Rows = std::map<std::string, std::map<std::string, std::string>>;
const std::vector<std::string> legacy{
    "default", "meta", "blocks", "headers", "height", "txindex", "utxo", "utreexo", "prebase_coins"};
constexpr const char* shielded = "shielded_state_v1";
constexpr const char* ready = "shielded-state-v1:READY";
const std::vector<std::string> reserved{
    "shielded_frontier", "shielded_anchor_history", "shielded_anchor_history_migrated_v1"};

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
#define CHECK(c) do { if (!(c)) throw Failure(std::string(__func__) + ": " + #c); } while (false)
template <typename T> T RequiredValue(dinero::StatusOr<T> value) {
    CHECK(value.ok());
    return value.value();
}
inline void Setup(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        auto pattern = (std::filesystem::temp_directory_path() / "dinero_shielded_store_XXXXXX").string();
        const char* created = mkdtemp(pattern.data());
        Setup(created, "mkdtemp"); path = created;
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
struct Raw {
    std::unique_ptr<rocksdb::DB> db;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    Raw(const std::filesystem::path& path, std::vector<std::string> names = {}) {
        rocksdb::Options opts;
        const bool create = !names.empty();
        opts.create_if_missing = opts.create_missing_column_families = create;
        if (!create) Setup(rocksdb::DB::ListColumnFamilies(opts, path.string(), &names).ok(), "list CFs");
        std::vector<rocksdb::ColumnFamilyDescriptor> desc;
        for (const auto& name : names) desc.emplace_back(name, opts);
        rocksdb::DB* p = nullptr;
        auto status = create ? rocksdb::DB::Open(opts, path.string(), desc, &handles, &p)
                            : rocksdb::DB::OpenForReadOnly(opts, path.string(), desc, &handles, &p);
        db.reset(p); Setup(status.ok(), "raw open");
    }
    ~Raw() { for (auto* h : handles) db->DestroyColumnFamilyHandle(h); }
    rocksdb::ColumnFamilyHandle* cf(const std::string& name) {
        for (auto* h : handles) if (h->GetName() == name) return h;
        throw std::runtime_error("missing raw CF");
    }
    void put(const std::string& cf_name, const std::string& key, const std::string& value) {
        rocksdb::WriteOptions opts; opts.sync = true;
        Setup(db->Put(opts, cf(cf_name), key, value).ok(), "seed row");
    }
    Rows rows() {
        Rows out;
        for (auto* h : handles) {
            auto& entries = out[h->GetName()];
            std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(rocksdb::ReadOptions(), h));
            for (it->SeekToFirst(); it->Valid(); it->Next()) entries[it->key().ToString()] = it->value().ToString();
            Setup(it->status().ok(), "raw iterator");
        }
        return out;
    }
};
inline Rows Inspect(const std::filesystem::path& path) { return Raw(path).rows(); }
inline std::string NullifierKey(unsigned height) {
    std::string key(37, '\0'); key[0] = 'N'; key[4] = static_cast<char>(height);
    std::fill(key.begin() + 5, key.end(), static_cast<char>(height)); return key;
}

inline void Seed(const std::filesystem::path& path, bool separated = true,
          std::optional<std::string> marker = ready, bool permuted = false,
          const std::string& missing = {}, bool leftover = false,
          bool add_checkpoint_cf = false) {
    auto names = legacy;
    if (separated) names.push_back(shielded);
    if (add_checkpoint_cf) names.push_back("utreexo_checkpoints_v1");
    if (permuted) std::swap(names[7], names[9]);
    if (missing == "prebase_coins") names.erase(std::find(names.begin(), names.end(), missing));
    Raw raw(path, names);
    const uint32_t version = 4;
    raw.put("meta", "schema_version", std::string(reinterpret_cast<const char*>(&version), 4));
    if (marker) raw.put("meta", "storage_layout_v1", *marker);
    if (missing != "shielded_tip") raw.put("meta", "shielded_tip", std::string(84, '\0'));
    const auto domain = separated ? shielded : "utreexo";
    for (const auto& key : reserved) {
        if (missing != key) raw.put(domain, "M" + key, key == reserved[2] ? "1" : "opaque-storage-fixture:" + key);
    }
    raw.put(domain, NullifierKey(1), ""); raw.put(domain, NullifierKey(3), "");
    raw.put("utreexo", std::string("U\0\0\0\1", 5), "checkpoint");
    raw.put("utreexo", std::string("C\0\0\0\1", 5), "checksum");
    raw.put("utreexo", "Sreplay", "replay"); raw.put("utreexo", "Ttransition", "transition");
    raw.put("utreexo", "Mshielded_future_extension", "unrelated-unmoved");
    raw.put("default", "UD:history", "retained-delta");
    raw.put("meta", "forest_tip", std::string(68, '\0'));
    if (leftover) raw.put("utreexo", "Mshielded_frontier", "stale-copy");
}

} // namespace shielded_store_fixture

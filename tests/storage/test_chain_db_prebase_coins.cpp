#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <rocksdb/db.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

using dinero::ChainDB;
using dinero::ChainWriteToken;
using dinero::Coin;
using dinero::Status;
using dinero::uint256;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL: " << msg << " (" << #cond << ") at line "    \
                      << __LINE__ << std::endl;                              \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

namespace {

std::filesystem::path TempDir() {
    auto dir = std::filesystem::temp_directory_path() /
               ("dinero_prebase_coins_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    return dir;
}

uint256 Hash(uint8_t byte) {
    uint256 value;
    std::memset(value.data, 0, sizeof(value.data));
    value.data[0] = byte;
    return value;
}

Coin MakeCoin(uint64_t amount, int height, bool coinbase) {
    Coin coin;
    coin.amount = amount;
    coin.script_pubkey = "5121";
    coin.height = height;
    coin.coinbase = coinbase;
    coin.is_confidential = true;
    coin.commitment = {0xAA, 0xBB, 0xCC};
    return coin;
}

void CreateLegacyEightCfDatabase(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    rocksdb::Options options;
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
    const std::vector<std::string> names{
        rocksdb::kDefaultColumnFamilyName, "meta", "blocks", "headers",
        "height", "txindex", "utxo", "utreexo"};
    for (const auto& name : names) {
        descriptors.emplace_back(name, rocksdb::ColumnFamilyOptions());
    }
    rocksdb::DB* raw_db = nullptr;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    CHECK(rocksdb::DB::Open(options, dir.string(), descriptors, &handles, &raw_db).ok(),
          "create legacy eight-CF database");
    for (auto* handle : handles) delete handle;
    delete raw_db;
}

}  // namespace

int main() {
    const auto dir = TempDir();
    CreateLegacyEightCfDatabase(dir);

    const auto base_a = Hash(0xA0);
    const auto base_b = Hash(0xB0);
    const auto tx_a = Hash(0x01);
    const auto tx_b = Hash(0x02);
    const ChainWriteToken token = ChainWriteToken::CreateForTesting();

    {
        ChainDB db;
        CHECK(db.init(dir) == Status::Ok,
              "append prebase_coins CF to an existing eight-CF database");

        std::vector<ChainDB::PreBaseCoinRecord> first{
            {tx_a, 3, MakeCoin(123456, 40, true)},
        };
        CHECK(db.replacePreBaseCoins(token, base_a, 50, first) == Status::Ok,
              "persist first frozen snapshot set");

        const auto loaded = db.getPreBaseCoin(tx_a, 3);
        CHECK(loaded.ok(), "read frozen coin");
        CHECK(loaded.value().amount == 123456, "amount round-trip");
        CHECK(loaded.value().script_pubkey == "5121", "script round-trip");
        CHECK(loaded.value().height == 40, "height round-trip");
        CHECK(loaded.value().coinbase, "coinbase round-trip");
        CHECK(loaded.value().is_confidential, "confidential flag round-trip");
        CHECK(loaded.value().commitment == std::vector<uint8_t>({0xAA, 0xBB, 0xCC}),
              "commitment round-trip");

        // Normal UTXO lifecycle must not mutate the frozen copy.
        CHECK(db.putCoin(token, tx_a, 3, first[0].coin) == Status::Ok,
              "write ordinary UTXO copy");
        CHECK(db.deleteCoin(token, tx_a, 3) == Status::Ok,
              "delete ordinary UTXO copy");
        CHECK(db.getPreBaseCoin(tx_a, 3).ok(),
              "ordinary spend cannot delete frozen record");

        // Invalid replacement is rejected before its WriteBatch commits, so
        // the previous complete set remains authoritative.
        std::vector<ChainDB::PreBaseCoinRecord> invalid{
            {tx_b, 0, MakeCoin(999, 51, false)},
        };
        CHECK(db.replacePreBaseCoins(token, base_b, 50, invalid) == Status::Invalid,
              "reject coin created above snapshot base");
        CHECK(db.getPreBaseCoin(tx_a, 3).ok(),
              "invalid replacement preserves old complete set");
        const auto old_marker = db.getPreBaseCoinSetBase();
        CHECK(old_marker.ok() && old_marker.value().first == base_a &&
                  old_marker.value().second == 50,
              "invalid replacement preserves old marker");

        std::vector<ChainDB::PreBaseCoinRecord> second{
            {tx_b, 7, MakeCoin(654321, 49, false)},
        };
        CHECK(db.replacePreBaseCoins(token, base_b, 50, second) == Status::Ok,
              "atomically replace frozen snapshot set");
        CHECK(!db.getPreBaseCoin(tx_a, 3).ok(), "old snapshot row removed");
        CHECK(db.getPreBaseCoin(tx_b, 7).ok(), "new snapshot row present");
    }

    {
        ChainDB reopened;
        CHECK(reopened.init(dir) == Status::Ok, "reopen migrated database");
        CHECK(reopened.getPreBaseCoin(tx_b, 7).ok(),
              "frozen row survives restart");
        const auto marker = reopened.getPreBaseCoinSetBase();
        CHECK(marker.ok() && marker.value().first == base_b &&
                  marker.value().second == 50,
              "snapshot marker survives restart");
    }

    std::filesystem::remove_all(dir);
    std::cout << "test_chain_db_prebase_coins: PASS" << std::endl;
    return 0;
}

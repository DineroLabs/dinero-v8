// The approved ten-family reader contract, using generated stores only.
#include "shielded_store_fixture.h"
using namespace shielded_store_fixture;

void OpenCase(bool separated, const std::optional<std::string>& marker, bool accept,
              bool permuted = false, const std::string& missing = {}, bool leftover = false,
              bool extra_cf = false) {
    TempDir temp; Seed(temp.path, separated, marker, permuted, missing, leftover, extra_cf);
    const auto before = Inspect(temp.path);
    {
        ChainDB db;
        const auto status = db.init(temp.path);
        CHECK((status == Status::Ok) == accept);
        if (accept) {
            auto count = db.countShieldedNullifiers(); CHECK(count.ok() && count.value() == 2);
            std::vector<uint32_t> heights;
            CHECK(db.forEachShieldedNullifier([&](uint32_t h, const uint8_t* nf) {
                CHECK(nf[0] == h); heights.push_back(h); return true;
            }) == Status::Ok);
            CHECK((heights == std::vector<uint32_t>{1, 3}));
        } else {
            // Failed opening must not retain usable handles or accept writes.
            CHECK(db.countShieldedNullifiers().status() == Status::Internal);
            const auto token = ChainWriteToken::CreateForTesting();
            uint8_t nf[32]{};
            CHECK(db.putShieldedNullifier(token, 4, nf) == Status::Internal);
        }
    }
    CHECK(Inspect(temp.path) == before);
}

void RecoveryAndWrites() {
    TempDir temp; Seed(temp.path);
    auto expected = Inspect(temp.path);
    const auto token = ChainWriteToken::CreateForTesting();
    {
        ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
        uint8_t nf[32]; std::fill(nf, nf + 32, 4);
        rocksdb::WriteBatch batch;
        CHECK(db.putShieldedNullifier(token, 4, nf, &batch) == Status::Ok);
        ChainDB::ShieldedTipMarker marker; marker.height = 4; marker.nullifier_count = 3;
        CHECK(db.putShieldedTipMarker(token, marker, &batch) == Status::Ok);
        CHECK(db.countShieldedNullifiers().value() == 2);
        CHECK(db.getShieldedTipMarker().value().height == 0);
        CHECK(db.writeBatch(token, std::move(batch), true) == Status::Ok);
        CHECK(db.countShieldedNullifiers().value() == 3);
        CHECK(db.getShieldedTipMarker().value().height == 4);
        auto removed = db.deleteShieldedNullifiersAboveHeight(token, 1);
        CHECK(removed.ok() && removed.value() == 2);
        CHECK(db.countShieldedNullifiers().value() == 1);
        CHECK(db.wipeAllUtreexoCheckpoints() == Status::Ok);
        CHECK(db.countShieldedNullifiers().value() == 1);
    }
    auto actual = Inspect(temp.path);
    CHECK(actual[shielded].at(NullifierKey(1)).empty());
    CHECK(actual[shielded].count(NullifierKey(3)) == 0);
    CHECK(actual[shielded].count(NullifierKey(4)) == 0);
    expected[shielded].erase(NullifierKey(3));
    expected["utreexo"].erase(std::string("U\0\0\0\1", 5));
    expected["utreexo"].erase(std::string("C\0\0\0\1", 5));
    expected["meta"].erase("forest_tip");
    expected["meta"]["shielded_tip"] = actual["meta"].at("shielded_tip");
    CHECK(actual == expected);
    ChainDB restarted; CHECK(restarted.init(temp.path) == Status::Ok);
    CHECK(restarted.countShieldedNullifiers().value() == 1);
}

void GenericRefusal(const std::string& key, bool write) {
    TempDir temp; Seed(temp.path, false, std::nullopt);
    const auto before = Inspect(temp.path);
    {
        ChainDB db; Setup(db.init(temp.path) == Status::Ok, "open legacy fixture");
        if (write) {
            const auto token = ChainWriteToken::CreateForTesting();
            rocksdb::WriteBatch batch;
            const auto bytes = batch.Data();
            CHECK(db.putUtreexoMeta(token, key, "wrong-domain", &batch) == Status::Invalid);
            CHECK(batch.Data() == bytes);
            CHECK(db.putUtreexoMeta(token, key, "wrong-domain") == Status::Invalid);
        } else CHECK(db.getUtreexoMeta(key).status() == Status::Invalid);
    }
    CHECK(Inspect(temp.path) == before);
}

void TypedRecords(bool separated, bool commit) {
    TempDir temp; Seed(temp.path, separated, separated ? std::optional<std::string>(ready) : std::nullopt);
    auto expected = Inspect(temp.path);
    const auto domain = separated ? shielded : "utreexo";
    const std::vector<ChainDB::ShieldedStateRecord> records{
        ChainDB::ShieldedStateRecord::Frontier, ChainDB::ShieldedStateRecord::AnchorHistory,
        ChainDB::ShieldedStateRecord::LegacyAnchorImportMarker};
    const auto token = ChainWriteToken::CreateForTesting();
    {
        ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
        CHECK(db.hasSeparatedShieldedState() == separated);
        rocksdb::WriteBatch batch;
        for (size_t i = 0; i < records.size(); ++i) {
            CHECK(RequiredValue(db.getShieldedState(records[i])) == expected[domain].at("M" + reserved[i]));
            const std::string bytes = std::string("new\0opaque", 10) + reserved[i];
            CHECK(db.putShieldedState(token, records[i], bytes, &batch) == Status::Ok);
            CHECK(RequiredValue(db.getShieldedState(records[i])) == expected[domain].at("M" + reserved[i]));
            if (commit) expected[domain]["M" + reserved[i]] = bytes;
        }
        if (commit) CHECK(db.writeBatch(token, std::move(batch), true) == Status::Ok);
        for (size_t i = 0; i < records.size(); ++i)
            CHECK(RequiredValue(db.getShieldedState(records[i])) == expected[domain].at("M" + reserved[i]));
    }
    CHECK(Inspect(temp.path) == expected);
    ChainDB reopened; CHECK(reopened.init(temp.path) == Status::Ok);
    for (size_t i = 0; i < records.size(); ++i)
        CHECK(RequiredValue(reopened.getShieldedState(records[i])) == expected[domain].at("M" + reserved[i]));
}

void InvalidTypedRecord(bool separated) {
    TempDir temp; Seed(temp.path, separated, separated ? std::optional<std::string>(ready) : std::nullopt);
    const auto before = Inspect(temp.path);
    {
        ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
        const auto token = ChainWriteToken::CreateForTesting();
        const auto invalid = static_cast<ChainDB::ShieldedStateRecord>(12345);
        rocksdb::WriteBatch batch;
        CHECK(db.putUtreexoMeta(token, "unrelated", "caller-data", &batch) == Status::Ok);
        const auto staged = batch.Data();
        CHECK(db.putShieldedState(token, invalid, "invalid", &batch) == Status::Invalid);
        CHECK(batch.Data() == staged);
        CHECK(db.putShieldedState(token, invalid, "invalid") == Status::Invalid);
        CHECK(db.getShieldedState(invalid).status() == Status::Invalid);
    }
    CHECK(Inspect(temp.path) == before);
}

void OptionalMarkerWrite(bool separated) {
    TempDir temp; Seed(temp.path, separated, separated ? std::optional<std::string>(ready) : std::nullopt,
                       false, reserved[2]);
    auto expected = Inspect(temp.path);
    {
        ChainDB db; CHECK(db.init(temp.path) == Status::Ok);
        const auto record = ChainDB::ShieldedStateRecord::LegacyAnchorImportMarker;
        CHECK(db.getShieldedState(record).status() == Status::NotFound);
        CHECK(db.putShieldedState(ChainWriteToken::CreateForTesting(), record, "1") == Status::Ok);
        CHECK(db.getShieldedState(record).value() == "1");
    }
    expected[separated ? shielded : "utreexo"]["M" + reserved[2]] = "1";
    CHECK(Inspect(temp.path) == expected);
}

struct Case { std::string name; std::function<void()> run; };
std::vector<Case> Cases() {
    std::vector<Case> cases{
        {"legacy9", [] { OpenCase(false, std::nullopt, true); }},
        {"ready10", [] { OpenCase(true, ready, true); }},
        {"permuted10", [] { OpenCase(true, ready, true, true); }},
        {"no_layout_marker", [] { OpenCase(true, std::nullopt, false); }},
        {"missing_cf", [] { OpenCase(false, ready, false); }},
        {"missing_prebase", [] { OpenCase(true, ready, false, false, "prebase_coins"); }},
        {"old_exploration_layout", [] { OpenCase(true, "separated-v1:READY", false, false, {}, false, true); }},
        {"leftover_source", [] { OpenCase(true, ready, false, false, {}, true); }},
        {"scoped_checkpoint_recovery_and_writes", RecoveryAndWrites},
    };
    for (const auto& state : {"PREPARING", "MOVING", "VERIFYING", "", "READY-with-trailing-bytes"}) {
        cases.push_back({"reject_" + std::string(state), [state] { OpenCase(true, "shielded-state-v1:" + std::string(state), false); }});
    }
    for (const auto& key : {"shielded_frontier", "shielded_anchor_history", "shielded_tip"})
        cases.push_back({"missing_" + std::string(key), [key] { OpenCase(true, ready, false, false, key); }});
    // Optional historical import marker must remain optional; never invent it.
    cases.push_back({"optional_import_marker", [] { OpenCase(true, ready, true, false, reserved[2]); }});
    for (const auto& key : reserved) for (bool write : {false, true})
        cases.push_back({"reserved_" + key + (write ? "_write" : "_read"), [key, write] { GenericRefusal(key, write); }});
    for (bool separated : {false, true}) {
        const std::string layout = separated ? "ready_" : "legacy_";
        for (bool commit : {false, true})
            cases.push_back({layout + (commit ? "commit_typed_records" : "abort_typed_records"),
                             [separated, commit] { TypedRecords(separated, commit); }});
        cases.push_back({layout + "invalid_typed_record", [separated] { InvalidTypedRecord(separated); }});
        cases.push_back({layout + "optional_marker_write", [separated] { OptionalMarkerWrite(separated); }});
    }
    return cases;
}
int main(int argc, char** argv) {
    const auto cases = Cases();
    if (argc == 2 && std::string(argv[1]) == "--list") {
        for (const auto& c : cases) std::cout << c.name << '\n'; return 0;
    }
    unsigned passed = 0, failed = 0;
    for (const auto& c : cases) {
        if (argc == 2 && c.name != argv[1]) continue;
        try { c.run(); ++passed; std::cout << "PASS " << c.name << '\n'; }
        catch (const Failure& e) { ++failed; std::cerr << "FAIL " << c.name << ": " << e.what() << '\n'; }
        catch (const std::exception& e) { std::cerr << "SETUP ERROR " << c.name << ": " << e.what() << '\n'; return 2; }
    }
    if (passed + failed == 0 || argc > 2) return 2;
    std::cout << passed << " passed, " << failed << " failed\n"; return failed ? 1 : 0;
}

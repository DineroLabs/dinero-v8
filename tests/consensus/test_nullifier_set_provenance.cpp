// Copyright (c) 2026 Dinero Labs.
//
// Provenance is the discriminator that decides whether a sqlite nullifier
// database's rows may ever be treated as authoritative. Getting it wrong in
// either direction is unrecoverable, so these tests are consensus-shaped: they
// assert on the STAMP, which is durable, not only on the in-memory
// classification, which is recomputed on the next open.
//
// The defect these lock out: both facts Open() depends on were read with
// queries that answered 0 on failure. A populated, unstamped legacy database
// that hit SQLITE_BUSY during Open therefore looked EMPTY, took the
// "fresh cache" branch, and was stamped -- permanently demoting the only
// authoritative nullifier set on that node. A transient lock cost the user
// their shielded spend history.

#include <gtest/gtest.h>

#include <cstdio>
#include <optional>
#include <string>

#include <sqlite3.h>

#include "consensus/shielded/nullifier_set.h"

using namespace dinero::consensus::shielded;
using OD = NullifierSet::OpenDecision;

namespace {

// Read the stamp with an INDEPENDENT connection. The stamp is the irreversible
// artifact, so it must be observed without going through the object under test.
int RawUserVersion(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt* st = nullptr;
    int v = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return v;
}

void Exec(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

}  // namespace

// ── the rule, exhaustively ────────────────────────────────────────────────
//
// Stated as a pure function so the states that only arise under sqlite failure
// are reachable at all. Same idiom as PreservedFailureFlags in
// consensus/block_status_generation.h.

TEST(NullifierSetProvenance, UnreadableUserVersionDecidesNothing) {
    EXPECT_EQ(NullifierSet::DecideProvenance(std::nullopt, 5), OD::Indeterminate);
    EXPECT_EQ(NullifierSet::DecideProvenance(std::nullopt, 0), OD::Indeterminate);
    EXPECT_EQ(NullifierSet::DecideProvenance(std::nullopt, std::nullopt),
              OD::Indeterminate);
}

// THE case. An unstamped file whose COUNT(*) failed used to read as empty and
// get stamped.
TEST(NullifierSetProvenance, UnreadableCountIsNeverTreatedAsEmpty) {
    EXPECT_EQ(NullifierSet::DecideProvenance(0, std::nullopt), OD::Indeterminate)
        << "an unreadable count must not be read as an empty database";
}

TEST(NullifierSetProvenance, StampedFilesAreCacheWhateverTheCountSays) {
    EXPECT_EQ(NullifierSet::DecideProvenance(1, 0), OD::AlreadyCache);
    EXPECT_EQ(NullifierSet::DecideProvenance(1, 9), OD::AlreadyCache);
    // Once stamped, the count cannot change the answer, so an unreadable count
    // is not an obstacle here.
    EXPECT_EQ(NullifierSet::DecideProvenance(1, std::nullopt), OD::AlreadyCache);
}

TEST(NullifierSetProvenance, UnstampedFilesSplitOnTheCount) {
    EXPECT_EQ(NullifierSet::DecideProvenance(0, 5), OD::LegacyCandidate);
    EXPECT_EQ(NullifierSet::DecideProvenance(0, 0), OD::StampFresh);
}

// ── real sqlite failures are reported as failures, not as zero ────────────

TEST(NullifierSetProvenance, FailedCountReportsFailureNotZero) {
    const std::string path = "/tmp/dinero_test_nfprov_dropped.sqlite";
    std::remove(path.c_str());
    NullifierSet set;
    ASSERT_EQ(set.Open(path), NullifierSet::OpenResult::Ok);
    Hash h{};
    h.fill(7);
    ASSERT_TRUE(set.Insert(h, 7));
    ASSERT_EQ(set.TryCount().value_or(999), 1u);

    // A genuine prepare failure on an OPEN handle.
    Exec(path, "DROP TABLE nullifiers");

    EXPECT_FALSE(set.TryCount().has_value())
        << "a failed COUNT(*) must report failure";
    EXPECT_EQ(set.Size(), 0u)
        << "Size() still flattens it to 0 — which is exactly why nothing "
           "may branch on Size()";
    std::remove(path.c_str());
}

TEST(NullifierSetProvenance, NeverOpenedSetReadsAsUnknownNotEmpty) {
    NullifierSet never;
    EXPECT_FALSE(never.TryCount().has_value());
    EXPECT_FALSE(never.TryReadUserVersion().has_value());
}

// ── end to end: the stamp ─────────────────────────────────────────────────

TEST(NullifierSetProvenance, PopulatedLegacyDatabaseIsNotDemoted) {
    const std::string path = "/tmp/dinero_test_nfprov_legacy.sqlite";
    std::remove(path.c_str());
    {
        NullifierSet seed;
        ASSERT_EQ(seed.Open(path), NullifierSet::OpenResult::Ok);
        for (uint8_t i = 1; i <= 3; ++i) {
            Hash h{};
            h.fill(i);
            ASSERT_TRUE(seed.Insert(h, i));
        }
    }
    // Undo the stamp so the fixture is a genuine pre-authority file.
    Exec(path, "PRAGMA user_version = 0");
    ASSERT_EQ(RawUserVersion(path), 0);

    NullifierSet set;
    ASSERT_EQ(set.Open(path), NullifierSet::OpenResult::Ok);
    EXPECT_EQ(set.GetProvenance(), NullifierSet::Provenance::LegacyCandidate);
    EXPECT_EQ(RawUserVersion(path), 0)
        << "opening a legacy database must not stamp it";
    std::remove(path.c_str());
}

TEST(NullifierSetProvenance, EmptyUnstampedDatabaseIsStamped) {
    const std::string path = "/tmp/dinero_test_nfprov_fresh.sqlite";
    std::remove(path.c_str());
    NullifierSet set;
    ASSERT_EQ(set.Open(path), NullifierSet::OpenResult::Ok);
    // Cache, not FreshCache: the stamp is already durable by the time Open
    // returns, so FreshCache is only the transient pre-stamp classification.
    EXPECT_EQ(set.GetProvenance(), NullifierSet::Provenance::Cache);
    EXPECT_EQ(RawUserVersion(path), NullifierSet::kCacheSchemaVersion);
    std::remove(path.c_str());
}

// An unreadable database must never be stamped, by whichever path rejects it.
//
// NOTE ON REACH: sqlite validates rootpages while PARSING the schema, so a
// corrupted table is rejected during CREATE TABLE IF NOT EXISTS and Open
// returns SchemaError before it ever reaches the count. The state "schema
// fine, count fails" could not be synthesized from outside the process, so
// the Indeterminate branch itself is pinned by the pure-function tests above.
// What this pins is the property that matters and IS reachable.
TEST(NullifierSetProvenance, UnreadableDatabaseIsNeverStamped) {
    const std::string path = "/tmp/dinero_test_nfprov_corrupt.sqlite";
    std::remove(path.c_str());
    Exec(path,
         "CREATE TABLE nullifiers (nullifier BLOB PRIMARY KEY NOT NULL, "
         "block_height INTEGER NOT NULL);"
         "CREATE INDEX idx_nullifiers_height ON nullifiers(block_height);"
         "INSERT INTO nullifiers VALUES(x'0102',7);"
         "PRAGMA user_version = 0;");
    Exec(path,
         "PRAGMA writable_schema=ON;"
         "UPDATE sqlite_master SET rootpage=99999 "
         "  WHERE name='nullifiers' AND type='table';");
    ASSERT_EQ(RawUserVersion(path), 0);

    NullifierSet set;
    EXPECT_NE(set.Open(path), NullifierSet::OpenResult::Ok);
    EXPECT_NE(set.GetProvenance(), NullifierSet::Provenance::Cache);
    EXPECT_EQ(RawUserVersion(path), 0)
        << "an unreadable database must not be stamped";
    std::remove(path.c_str());
}

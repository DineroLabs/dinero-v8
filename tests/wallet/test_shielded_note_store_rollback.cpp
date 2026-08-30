#include "wallet/shielded_note_store.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace sh = dinero::consensus::shielded;
using dinero::wallet::ShieldedNoteStore;
using dinero::wallet::NoteKeyScheme;

sh::Hash HashWithByte(uint8_t value) {
    sh::Hash hash{};
    hash.fill(value);
    return hash;
}

class ShieldedNoteStoreRollbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("dinero-shielded-rollback-" + std::to_string(nonce) + ".sqlite");
        ASSERT_EQ(store_.Open(path_.string()), ShieldedNoteStore::OpenResult::Ok);
    }

    void TearDown() override {
        store_.Close();
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    ShieldedNoteStore store_;
    std::filesystem::path path_;
};

TEST_F(ShieldedNoteStoreRollbackTest, RemovesOnlyPendingWalletMutation) {
    const auto pending_spend_key = HashWithByte(1);
    const auto confirmed_spend_key = HashWithByte(2);
    const auto pending_spend_commitment = HashWithByte(3);
    const auto confirmed_spend_commitment = HashWithByte(4);
    const auto pending_output_commitment = HashWithByte(5);
    const auto unrelated_pending_commitment = HashWithByte(6);

    ASSERT_TRUE(store_.AddNote(50, pending_spend_key, HashWithByte(11),
                               HashWithByte(12), pending_spend_commitment, 0, 10));
    ASSERT_TRUE(store_.AddNote(60, confirmed_spend_key, HashWithByte(13),
                               HashWithByte(14), confirmed_spend_commitment, 1, 10));
    const auto pending_nullifier = sh::ComputeNullifier(pending_spend_key, 0);
    const auto confirmed_nullifier = sh::ComputeNullifier(confirmed_spend_key, 1);
    ASSERT_TRUE(store_.MarkSpentByNullifier(pending_nullifier, 0));
    ASSERT_TRUE(store_.MarkSpentByNullifier(confirmed_nullifier, 20));
    ASSERT_TRUE(store_.AddPendingNote(40, HashWithByte(21), HashWithByte(22),
                                      HashWithByte(23), pending_output_commitment, 0));
    ASSERT_TRUE(store_.AddPendingNote(30, HashWithByte(24), HashWithByte(25),
                                      HashWithByte(26), unrelated_pending_commitment, 0));

    ASSERT_TRUE(store_.RollbackPendingTransaction(
        {pending_nullifier, confirmed_nullifier},
        {pending_output_commitment, confirmed_spend_commitment}));

    const auto notes = store_.ListAll();
    ASSERT_EQ(notes.size(), 3U);
    const auto pending_input = store_.GetByLeafIndex(0);
    const auto confirmed_input = store_.GetByLeafIndex(1);
    ASSERT_TRUE(pending_input.has_value());
    EXPECT_FALSE(pending_input->spent);
    EXPECT_EQ(pending_input->spent_height, 0U);
    ASSERT_TRUE(confirmed_input.has_value());
    EXPECT_TRUE(confirmed_input->spent);
    EXPECT_EQ(confirmed_input->spent_height, 20U);

    EXPECT_EQ(std::count_if(notes.begin(), notes.end(), [&](const auto& note) {
                  return note.commitment == pending_output_commitment;
              }), 0);
    EXPECT_EQ(std::count_if(notes.begin(), notes.end(), [&](const auto& note) {
                  return note.commitment == unrelated_pending_commitment;
              }), 1);
}

TEST_F(ShieldedNoteStoreRollbackTest, IsIdempotentForPartialPersistence) {
    const auto spend_key = HashWithByte(31);
    const auto spend_commitment = HashWithByte(32);
    const auto absent_output = HashWithByte(33);
    ASSERT_TRUE(store_.AddNote(75, spend_key, HashWithByte(34), HashWithByte(35),
                               spend_commitment, 4, 12));
    const auto nullifier = sh::ComputeNullifier(spend_key, 4);
    ASSERT_TRUE(store_.MarkSpentByNullifier(nullifier, 0));

    ASSERT_TRUE(store_.RollbackPendingTransaction({nullifier}, {absent_output}));
    ASSERT_TRUE(store_.RollbackPendingTransaction({nullifier}, {absent_output}));

    const auto note = store_.GetByLeafIndex(4);
    ASSERT_TRUE(note.has_value());
    EXPECT_FALSE(note->spent);
    EXPECT_EQ(note->spent_height, 0U);
}


// ── Spend-authority: the note's key convention must survive persistence ──
//
// key_scheme is fixed when a note is CREATED and decides which circuit can
// spend it. If it did not round-trip, an auth note would be reloaded as legacy
// and become silently unspendable.

TEST_F(ShieldedNoteStoreRollbackTest, KeySchemeDefaultsToLegacy) {
    ASSERT_TRUE(store_.AddPendingNote(1000, HashWithByte(0x11), HashWithByte(0x12),
                                      HashWithByte(0x13), HashWithByte(0x14), 5));
    const auto notes = store_.ListAll();
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].key_scheme, NoteKeyScheme::LegacySenderKey)
        << "an unspecified scheme must read back as legacy, which is what it is";
}

TEST_F(ShieldedNoteStoreRollbackTest, KeySchemeRoundTripsAuth) {
    const auto auth_d = HashWithByte(0x2d);
    ASSERT_TRUE(store_.AddPendingNote(2000, HashWithByte(0x21), HashWithByte(0x22),
                                      HashWithByte(0x23), HashWithByte(0x24), 6,
                                      NoteKeyScheme::Auth, auth_d));
    ASSERT_TRUE(store_.AddPendingNote(3000, HashWithByte(0x31), HashWithByte(0x32),
                                      HashWithByte(0x33), HashWithByte(0x34), 7,
                                      NoteKeyScheme::LegacySenderKey));
    const auto notes = store_.ListAll();
    ASSERT_EQ(notes.size(), 2u);
    // Both schemes coexist and are distinguished per row.
    EXPECT_EQ(notes[0].key_scheme, NoteKeyScheme::Auth);
    EXPECT_EQ(notes[0].d, auth_d);
    EXPECT_EQ(notes[1].key_scheme, NoteKeyScheme::LegacySenderKey);
    EXPECT_EQ(notes[1].d, sh::Hash{});
}

// A wallet written before key_scheme existed must open, gain the column, and
// report its pre-existing notes as legacy. CREATE TABLE IF NOT EXISTS is a
// no-op on an existing table, so without the ADD COLUMN migration every read
// path referencing key_scheme fails and the wallet cannot open at all.
TEST_F(ShieldedNoteStoreRollbackTest, PreSpendAuthDatabaseGainsKeySchemeColumn) {
    // Build a wallet DB with the OLD schema: no key_scheme column.
    store_.Close();
    std::error_code ec;
    std::filesystem::remove(path_, ec);

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path_.string().c_str(), &raw), SQLITE_OK);
    const char* legacy_ddl =
        "CREATE TABLE shielded_notes ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  value_una INTEGER NOT NULL,"
        "  secret_key BLOB NOT NULL,"
        "  public_key BLOB NOT NULL,"
        "  randomness BLOB NOT NULL,"
        "  commitment BLOB NOT NULL UNIQUE,"
        "  leaf_index INTEGER,"
        "  nullifier BLOB,"
        "  confirmed INTEGER NOT NULL DEFAULT 0,"
        "  spent INTEGER NOT NULL DEFAULT 0,"
        "  created_height INTEGER NOT NULL DEFAULT 0,"
        "  confirmed_height INTEGER NOT NULL DEFAULT 0,"
        "  spent_height INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE shielded_tree_leaves ("
        "  leaf_index INTEGER PRIMARY KEY NOT NULL,"
        "  commitment BLOB NOT NULL,"
        "  created_height INTEGER NOT NULL"
        ");"
        "INSERT INTO shielded_notes "
        "  (value_una, secret_key, public_key, randomness, commitment,"
        "   leaf_index, confirmed, spent, created_height) "
        "VALUES (4242, x'01', x'02', x'03', x'04', 0, 1, 0, 9);";
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(raw, legacy_ddl, nullptr, nullptr, &err), SQLITE_OK)
        << (err ? err : "");
    sqlite3_close(raw);

    // Opening must migrate rather than fail.
    ASSERT_EQ(store_.Open(path_.string()), ShieldedNoteStore::OpenResult::Ok)
        << "a pre-spend-auth wallet must still open";

    const auto notes = store_.ListAll();
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].value_una, 4242u) << "pre-existing row must survive intact";
    EXPECT_EQ(notes[0].key_scheme, NoteKeyScheme::LegacySenderKey);
    EXPECT_EQ(notes[0].d, sh::Hash{});

    // Idempotent: re-opening must not attempt ADD COLUMN twice.
    store_.Close();
    EXPECT_EQ(store_.Open(path_.string()), ShieldedNoteStore::OpenResult::Ok)
        << "migration must be idempotent across restarts";
}

}  // namespace

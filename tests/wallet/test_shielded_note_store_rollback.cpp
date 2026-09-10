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
using dinero::wallet::OutgoingShieldedNote;

sh::Hash HashWithByte(uint8_t value) {
    sh::Hash hash{};
    hash.fill(value);
    return hash;
}

OutgoingShieldedNote OutgoingNoteWithByte(uint8_t value, bool confirmed) {
    OutgoingShieldedNote note;
    note.commitment = HashWithByte(value);
    note.recipient_address_payload.fill(static_cast<uint8_t>(value + 1));
    note.value_una = 1000 + value;
    note.memo.fill(static_cast<uint8_t>(value + 2));
    note.txid = std::string(64, confirmed ? 'c' : 'p');
    note.confirmed = confirmed;
    note.created_height = 40;
    note.confirmed_height = confirmed ? 50 : 0;
    return note;
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
    ASSERT_TRUE(store_.AddPendingNote(2000, HashWithByte(0x21), HashWithByte(0x25), HashWithByte(0x22),
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

TEST_F(ShieldedNoteStoreRollbackTest, RescanStyleConfirmedAuthRoundTrips) {
    const auto auth_d = HashWithByte(0x4d);
    ASSERT_TRUE(store_.AddNote(4000, HashWithByte(0x41), HashWithByte(0x45), HashWithByte(0x42),
                               HashWithByte(0x43), HashWithByte(0x44), 17, 90,
                               NoteKeyScheme::Auth, auth_d));
    store_.Close();
    ASSERT_EQ(store_.Open(path_.string()), ShieldedNoteStore::OpenResult::Ok);

    const auto note = store_.GetByLeafIndex(17);
    ASSERT_TRUE(note.has_value());
    EXPECT_TRUE(note->confirmed);
    EXPECT_EQ(note->confirmed_height, 90u);
    EXPECT_EQ(note->key_scheme, NoteKeyScheme::Auth)
        << "a rescan-restored auth note must not become legacy after restart";
    EXPECT_EQ(note->d, auth_d);
}

TEST_F(ShieldedNoteStoreRollbackTest, AuthSpendSecretNeverReachesDatabase) {
    const auto spend = HashWithByte(0x61);
    const auto nfk = HashWithByte(0x62);
    const auto pending = HashWithByte(0x63);
    ASSERT_TRUE(store_.AddPendingNote(1000, spend, nfk, HashWithByte(0x64),
                                      HashWithByte(0x65), pending, 10,
                                      NoteKeyScheme::Auth, HashWithByte(0x66)));
    ASSERT_TRUE(store_.AddNote(2000, spend, nfk, HashWithByte(0x67),
                               HashWithByte(0x68), HashWithByte(0x69), 1, 11,
                               NoteKeyScheme::Auth, HashWithByte(0x6a)));
    // Check raw SQLite, not just a reader that could hide persisted secrets.
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path_.string().c_str(), &raw), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw,
        "SELECT COUNT(*) FROM shielded_notes WHERE key_scheme = 1 "
        "AND secret_key = zeroblob(32) AND nullifier_key = ?", -1,
        &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_blob(stmt, 1, nfk.data(), nfk.size(), SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);
    sqlite3_close(raw);

    ASSERT_TRUE(store_.ConfirmNote(pending, 0, 12));
    store_.Close();
    ASSERT_EQ(store_.Open(path_.string()), ShieldedNoteStore::OpenResult::Ok);
    for (const auto& note : store_.ListUnspent()) {
        EXPECT_EQ(note.secret_key, sh::Hash{});
        EXPECT_EQ(note.nullifier_key, nfk);
        EXPECT_EQ(note.nullifier, sh::ComputeNullifier(nfk, note.leaf_index));
    }
}

TEST_F(ShieldedNoteStoreRollbackTest, LegacyOverloadsRejectAuthScheme) {
    // Otherwise the legacy overload would persist s as the nullifier key,
    // bypassing the Auth secret_key suppression through a different column.
    EXPECT_FALSE(store_.AddPendingNote(1000, HashWithByte(0x71),
        HashWithByte(0x72), HashWithByte(0x73), HashWithByte(0x74), 10,
        NoteKeyScheme::Auth));
    EXPECT_FALSE(store_.AddNote(1000, HashWithByte(0x71),
        HashWithByte(0x72), HashWithByte(0x73), HashWithByte(0x74), 0, 10,
        NoteKeyScheme::Auth));
    EXPECT_TRUE(store_.ListAll().empty());
}

TEST_F(ShieldedNoteStoreRollbackTest, ReopenRetiresDevelopmentAuthSpendCache) {
    const auto nfk = HashWithByte(0x92);
    ASSERT_TRUE(store_.AddNote(1000, HashWithByte(0x91), nfk,
        HashWithByte(0x93), HashWithByte(0x94), HashWithByte(0x95), 0, 10,
        NoteKeyScheme::Auth));
    store_.Close();
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path_.string().c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw,
        "UPDATE shielded_notes SET secret_key = randomblob(32)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    ASSERT_EQ(store_.Open(path_.string()), ShieldedNoteStore::OpenResult::Ok);
    const auto note = store_.GetByLeafIndex(0);
    ASSERT_TRUE(note);
    EXPECT_EQ(note->secret_key, sh::Hash{});
    EXPECT_EQ(note->nullifier_key, nfk);
    EXPECT_EQ(note->nullifier, sh::ComputeNullifier(nfk, 0));
}

TEST_F(ShieldedNoteStoreRollbackTest, MissingAuthNullifierKeyCannotUseSpendKey) {
    ASSERT_TRUE(store_.AddPendingNote(1000, HashWithByte(0xa1),
        HashWithByte(0xa2), HashWithByte(0xa3), HashWithByte(0xa4),
        HashWithByte(0xa5), 10, NoteKeyScheme::Auth));
    store_.Close();
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path_.string().c_str(), &raw), SQLITE_OK);
    // Match the nullable column added by preexisting database migrations.
    ASSERT_EQ(sqlite3_exec(raw,
        "ALTER TABLE shielded_notes DROP COLUMN nullifier_key;"
        "ALTER TABLE shielded_notes ADD COLUMN nullifier_key BLOB;",
        nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    EXPECT_EQ(store_.Open(path_.string()), ShieldedNoteStore::OpenResult::SchemaError);
}

TEST_F(ShieldedNoteStoreRollbackTest, OutgoingRecoverySurvivesRestartAndConfirmation) {
    auto provisional = OutgoingNoteWithByte(0x51, false);
    ASSERT_TRUE(store_.UpsertOutgoingNote(provisional));
    store_.Close();
    ASSERT_EQ(store_.Open(path_.string()), ShieldedNoteStore::OpenResult::Ok);
    auto notes = store_.ListOutgoingNotes();
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_FALSE(notes[0].confirmed);
    EXPECT_EQ(notes[0].recipient_address_payload,
              provisional.recipient_address_payload);

    auto confirmed = provisional;
    confirmed.confirmed = true;
    confirmed.confirmed_height = 77;
    confirmed.txid = std::string(64, 'a');
    ASSERT_TRUE(store_.UpsertOutgoingNote(confirmed));
    notes = store_.ListOutgoingNotes();
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_TRUE(notes[0].confirmed);
    EXPECT_EQ(notes[0].confirmed_height, 77u);
    EXPECT_EQ(notes[0].txid, confirmed.txid);
}

TEST_F(ShieldedNoteStoreRollbackTest, ProvisionalObservationCannotDowngradeConfirmed) {
    auto confirmed = OutgoingNoteWithByte(0x61, true);
    ASSERT_TRUE(store_.UpsertOutgoingNote(confirmed));
    auto stale = confirmed;
    stale.confirmed = false;
    stale.confirmed_height = 0;
    stale.txid = std::string(64, 'x');
    stale.value_una++;
    ASSERT_TRUE(store_.UpsertOutgoingNote(stale));

    const auto notes = store_.ListOutgoingNotes();
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_TRUE(notes[0].confirmed);
    EXPECT_EQ(notes[0].confirmed_height, confirmed.confirmed_height);
    EXPECT_EQ(notes[0].txid, confirmed.txid);
    EXPECT_EQ(notes[0].value_una, confirmed.value_una);
}

TEST_F(ShieldedNoteStoreRollbackTest, RejectionRollsBackOnlyProvisionalOutgoing) {
    const auto provisional = OutgoingNoteWithByte(0x71, false);
    const auto confirmed = OutgoingNoteWithByte(0x72, true);
    ASSERT_TRUE(store_.UpsertOutgoingNote(provisional));
    ASSERT_TRUE(store_.UpsertOutgoingNote(confirmed));

    ASSERT_TRUE(store_.RollbackPendingTransaction(
        {}, {provisional.commitment, confirmed.commitment}));
    const auto notes = store_.ListOutgoingNotes();
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].commitment, confirmed.commitment);
    EXPECT_TRUE(notes[0].confirmed);
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
        "VALUES (4242, zeroblob(32), zeroblob(32), zeroblob(32), "
        "        zeroblob(32), 0, 1, 0, 9);";
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

    // The outgoing-history table is additive and must appear when an older
    // wallet first opens; otherwise sender recovery would work only for newly
    // created wallet files.
    const auto outgoing = OutgoingNoteWithByte(0x7a, true);
    ASSERT_TRUE(store_.UpsertOutgoingNote(outgoing));
    ASSERT_EQ(store_.ListOutgoingNotes().size(), 1u);

    // Idempotent: re-opening must not attempt ADD COLUMN twice.
    store_.Close();
    EXPECT_EQ(store_.Open(path_.string()), ShieldedNoteStore::OpenResult::Ok)
        << "migration must be idempotent across restarts";
}

TEST_F(ShieldedNoteStoreRollbackTest, MalformedNullifierKeyFailsClosedOnOpen) {
    ASSERT_TRUE(store_.AddPendingNote(9000, HashWithByte(0x81),
                                      HashWithByte(0x82), HashWithByte(0x83),
                                      HashWithByte(0x84), HashWithByte(0x85), 8,
                                      NoteKeyScheme::Auth,
                                      HashWithByte(0x86)));
    store_.Close();

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path_.string().c_str(), &raw), SQLITE_OK);
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(raw,
                           "UPDATE shielded_notes SET nullifier_key = x'01';",
                           nullptr, nullptr, &err),
              SQLITE_OK)
        << (err ? err : "");
    sqlite3_close(raw);

    EXPECT_EQ(store_.Open(path_.string()),
              ShieldedNoteStore::OpenResult::SchemaError)
        << "a truncated nullifier key must never decode as the all-zero key";
}

TEST_F(ShieldedNoteStoreRollbackTest, UnknownAuthoritySchemeFailsClosedOnOpen) {
    ASSERT_TRUE(store_.AddPendingNote(9000, HashWithByte(0x91),
                                      HashWithByte(0x92), HashWithByte(0x93),
                                      HashWithByte(0x94), HashWithByte(0x95), 8,
                                      NoteKeyScheme::Auth,
                                      HashWithByte(0x96)));
    store_.Close();

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path_.string().c_str(), &raw), SQLITE_OK);
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(raw,
                           "UPDATE shielded_notes SET key_scheme = 3;",
                           nullptr, nullptr, &err),
              SQLITE_OK)
        << (err ? err : "");
    sqlite3_close(raw);

    EXPECT_EQ(store_.Open(path_.string()),
              ShieldedNoteStore::OpenResult::SchemaError)
        << "unknown authority schemes must not be reinterpreted as legacy";
}

}  // namespace

#include "wallet/private_covenant_descriptor.h"

namespace {
dinero::wallet::PrivateCovenantDescriptor TestPrivateDescriptor() {
    std::array<uint8_t,64> seed{}; seed[0]=17;
    const auto keys=dinero::wallet::shielded::DeriveShieldedAccount(seed.data(),seed.size(),0);
    const auto address=dinero::wallet::shielded::DeriveDiversifiedAddress(keys,0,"rdins");
    dinero::wallet::PrivateCovenantDescriptor d;
    d.seed[0]=37; d.minimum_height=120; d.fee_una=1000;
    d.outputs={{address.payload,40000},{address.payload,20000}};
    return d;
}
TEST(PrivateCovenantDescriptor, CanonicalRecoveryAndTamperRejection) {
    using namespace dinero::wallet;
    auto d=TestPrivateDescriptor();
    auto memo=EncodePrivateCovenantDescriptor(d);
    auto recovered=DecodePrivateCovenantDescriptor(memo);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(PrivateCovenantFundingValue(*recovered),61000);
    EXPECT_EQ(EncodePrivateCovenantDescriptor(*recovered),memo);
    EXPECT_EQ(PrivateCovenantDescriptorRoot(*recovered),PrivateCovenantDescriptorRoot(d));
    memo.back()=1; EXPECT_FALSE(DecodePrivateCovenantDescriptor(memo));
    memo=EncodePrivateCovenantDescriptor(d); memo[8]=3;
    EXPECT_FALSE(DecodePrivateCovenantDescriptor(memo));
    d.outputs[0].value_una=INT64_MAX;
    EXPECT_THROW(EncodePrivateCovenantDescriptor(d),std::invalid_argument);
}
TEST(PrivateCovenantDescriptor, ExactCiphertextAndDomainSeparation) {
    using namespace dinero::wallet;
    auto d=TestPrivateDescriptor();
    const auto a=DerivePrivateCovenantOutputs(d), b=DerivePrivateCovenantOutputs(d);
    ASSERT_EQ(a.size(),2);
    EXPECT_EQ(a[0].output.encrypted_note,b[0].output.encrypted_note);
    EXPECT_EQ(a[0].output.commitment,b[0].output.commitment);
    EXPECT_NE(a[0].rcm,a[0].esk);
    EXPECT_NE(a[0].esk,a[1].esk);
    const auto root=PrivateCovenantDescriptorRoot(d);
    std::swap(d.outputs[0],d.outputs[1]); EXPECT_NE(root,PrivateCovenantDescriptorRoot(d));
    d.seed[0]++; EXPECT_NE(a[0].esk,DerivePrivateCovenantOutputs(d)[0].esk);
}
TEST_F(ShieldedNoteStoreRollbackTest, PrivateDescriptorSurvivesRestartWithoutSpendSecret) {
    using namespace dinero::wallet;
    const auto memo=EncodePrivateCovenantDescriptor(TestPrivateDescriptor());
    ASSERT_TRUE(store_.AddNote(61000,HashWithByte(1),HashWithByte(2),HashWithByte(3),
        HashWithByte(4),HashWithByte(5),0,10,NoteKeyScheme::PrivateCovenant,{},memo));
    EXPECT_FALSE(store_.AddNote(60000,HashWithByte(1),HashWithByte(2),HashWithByte(3),
        HashWithByte(4),HashWithByte(6),1,10,NoteKeyScheme::PrivateCovenant,{},memo));
    store_.Close(); ASSERT_EQ(store_.Open(path_.string()),ShieldedNoteStore::OpenResult::Ok);
    const auto note=store_.GetByLeafIndex(0); ASSERT_TRUE(note);
    EXPECT_EQ(note->key_scheme,NoteKeyScheme::PrivateCovenant);
    EXPECT_EQ(note->covenant_memo,memo);
    EXPECT_EQ(note->secret_key,sh::Hash{});
}
}

#include "consensus/shielded/shielded_serialization.h"
TEST(PrivateCovenantDescriptor, PolicySurvivesWireCanonicalizationAcrossSeeds) {
    using namespace dinero::wallet;
    for(uint8_t seed=1;seed<=32;++seed) {
        auto descriptor=TestPrivateDescriptor(); descriptor.seed[0]=seed;
        const auto material=DerivePrivateCovenantOutputs(descriptor);
        sh::ShieldedBundle bundle;
        for(const auto& output:material) bundle.outputs.push_back(output.output);
        sh::ShieldedBundle decoded;
        ASSERT_EQ(sh::DeserializeShieldedBundle(sh::SerializeShieldedBundle(bundle),&decoded),sh::BundleDecodeError::Ok);
        EXPECT_EQ(PrivateCovenantDescriptorRoot(descriptor),sh::PrivateCovenantOutputRoot(decoded.outputs))
            << "descriptor seed " << int(seed) << " must bind the actual wire output order";
    }
}

TEST_F(ShieldedNoteStoreRollbackTest, InvalidPrivateDescriptorFailsClosedOnOpen) {
    using namespace dinero::wallet;
    const auto memo=EncodePrivateCovenantDescriptor(TestPrivateDescriptor());
    ASSERT_TRUE(store_.AddNote(61000,HashWithByte(1),HashWithByte(2),HashWithByte(3),
        HashWithByte(4),HashWithByte(5),0,10,NoteKeyScheme::PrivateCovenant,{},memo));
    store_.Close();
    sqlite3* raw=nullptr;
    ASSERT_EQ(sqlite3_open(path_.string().c_str(),&raw),SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw,"UPDATE shielded_notes SET covenant_memo=zeroblob(512)",nullptr,nullptr,nullptr),SQLITE_OK);
    sqlite3_close(raw);
    EXPECT_EQ(store_.Open(path_.string()),ShieldedNoteStore::OpenResult::SchemaError);
}

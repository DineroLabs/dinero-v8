#include "wallet/shielded_note_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace sh = dinero::consensus::shielded;
using dinero::wallet::ShieldedNoteStore;

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

} // namespace

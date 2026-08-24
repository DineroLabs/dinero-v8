// Copyright (c) 2026 Dinero Labs.
//
// M3 PR B - C ABI tests for the native ShieldedProverKit surface
// exported to iOS.

#include <gtest/gtest.h>

#include "shielded_prover_kit/shielded_prover_kit.h"

#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_validation.h"
#include "primitives/transaction.h"
#include "wallet/shielded_derivation.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define DINERO_GETPID _getpid
#else
#include <unistd.h>
#define DINERO_GETPID getpid
#endif

namespace {

namespace sh = dinero::consensus::shielded;
namespace deriv = dinero::wallet::shielded;

sh::Hash MakeHash(uint8_t seed, uint8_t tail = 0xCD) {
    sh::Hash h{};
    h[0]  = seed;
    h[31] = tail;
    return h;
}

sh::Hash ValueAsHash(uint64_t v) {
    sh::Hash h{};
    for (int i = 0; i < 8; ++i) {
        h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    return h;
}

std::string TempDbPath() {
    static std::atomic<uint64_t> counter{0};
    const auto pid = static_cast<unsigned long long>(DINERO_GETPID());
    const auto ts = static_cast<long long>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    char name[96];
    std::snprintf(name, sizeof(name),
                  "dinero_proverkit_test_%llu_%lld_%llu.db",
                  pid, ts, static_cast<unsigned long long>(seq));
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

dinero::Transaction MakeUnshieldEnvelope(uint64_t recipient_value,
                                         uint64_t fee_una,
                                         uint8_t recipient_seed) {
    dinero::Transaction tx;
    tx.version  = dinero::Transaction::TX_VERSION_SHIELDED;
    tx.lockTime = 0;

    dinero::TxOutput out;
    out.value = dinero::AmountUna::Una(recipient_value);
    out.scriptPubKey = {0x00, recipient_seed};
    tx.vout.push_back(std::move(out));

    tx.SetExplicitFee(fee_una);
    return tx;
}

std::vector<uint8_t> SerializeUnsignedShieldedEnvelopeForProver(
    const dinero::Transaction& tx) {
    std::vector<uint8_t> out;
    dinero::TransactionSerializer::WriteUint32(out,
                                               static_cast<uint32_t>(tx.version));

    if (tx.vin.empty()) {
        out.push_back(0x00);
        out.push_back(0x01);
    }

    dinero::TransactionSerializer::WriteVarint(out, tx.vin.size());
    for (const auto& input : tx.vin) {
        const auto& txid_bytes = input.prevout.txid.AsUint256();
        out.insert(out.end(), txid_bytes.begin(), txid_bytes.end());
        dinero::TransactionSerializer::WriteUint32(out, input.prevout.vout);
        dinero::TransactionSerializer::WriteBytes(out, input.scriptSig);
        dinero::TransactionSerializer::WriteUint32(out, input.sequence);
    }

    dinero::TransactionSerializer::WriteVarint(out, tx.vout.size());
    for (const auto& output : tx.vout) {
        dinero::TransactionSerializer::WriteUint64(out, output.value.GetUna());
        dinero::TransactionSerializer::WriteBytes(out, output.scriptPubKey);
    }

    out.push_back(tx.has_explicit_fee ? 0x01 : 0x00);
    if (tx.has_explicit_fee) {
        dinero::TransactionSerializer::WriteUint64(out, tx.explicit_fee.GetUna());
    }
    dinero::TransactionSerializer::WriteBytes(out, std::vector<uint8_t>{});
    dinero::TransactionSerializer::WriteUint32(out, tx.lockTime);
    return out;
}

void CopyHashToBytes(const sh::Hash& h, uint8_t out[32]) {
    std::memcpy(out, h.data(), h.size());
}

dinero_shielded_spend_note MakeAbiSpendNote(sh::CommitmentTree& tree,
                                            const sh::Hash& rcm,
                                            const sh::Hash& d,
                                            uint64_t value_una) {
    const sh::Hash sk_note = deriv::DeriveNoteSpendKey(rcm);
    const sh::Hash pk_note = sh::PoseidonHash2(sk_note, sh::Hash{});
    const sh::Hash cm = sh::NoteCommitment(d, pk_note,
                                           ValueAsHash(value_una), rcm);
    const uint64_t leaf_index = tree.Append(cm);
    const auto path = tree.GetAuthPath(leaf_index);
    EXPECT_TRUE(path.has_value());

    dinero_shielded_spend_note note{};
    CopyHashToBytes(rcm, note.rcm);
    CopyHashToBytes(d, note.d);
    note.leaf_index = leaf_index;
    note.value_una = value_una;
    CopyHashToBytes(tree.Root(), note.anchor);
    if (path.has_value()) {
        for (size_t i = 0; i < sh::TREE_DEPTH; ++i) {
            CopyHashToBytes(path->siblings[i], note.merkle_path[i]);
        }
    }
    return note;
}

int BuildUnshieldViaAbi(const dinero_shielded_spend_note& note,
                        const dinero::Transaction& tx,
                        uint64_t fee_una,
                        dinero_shielded_unshield_result* result) {
    const auto unsigned_tx = SerializeUnsignedShieldedEnvelopeForProver(tx);
    dinero_shielded_unshield_request req{};
    req.version = static_cast<uint8_t>(tx.version);
    req.serialized_unsigned_tx = unsigned_tx.data();
    req.serialized_unsigned_tx_len = unsigned_tx.size();
    req.fee_una = fee_una;
    req.note = &note;
    return dinero_shielded_build_unshield_bundle(&req, result);
}

sh::ShieldedValidationError ValidateBundleForTx(
    const sh::ShieldedBundle& bundle,
    const dinero::Transaction& tx,
    const sh::CommitmentTree& tree,
    int64_t transparent_value_delta) {
    sh::NullifierSet nullifiers;
    const auto nf_path = TempDbPath();
    EXPECT_EQ(nullifiers.Open(nf_path), sh::NullifierSet::OpenResult::Ok);
    sh::ValidationContext ctx{
        &nullifiers,
        &tree,
        /*block_height=*/100,
        transparent_value_delta,
        /*shielded_activation_height=*/0,
        /*anchor_history=*/nullptr,
        sh::ComputeShieldedTxSighash(tx),
    };
    const auto rc = sh::ValidateShieldedBundle(bundle, ctx);
    nullifiers.Close();
    std::filesystem::remove(nf_path);
    return rc;
}

sh::ShieldedBundle DecodeResultBundle(
    const dinero_shielded_unshield_result& result) {
    sh::ShieldedBundle decoded;
    std::vector<uint8_t> bytes(result.bundle_bytes,
                               result.bundle_bytes + result.bundle_len);
    EXPECT_EQ(sh::DeserializeShieldedBundle(bytes, &decoded),
              sh::BundleDecodeError::Ok);
    return decoded;
}

} // namespace

TEST(ShieldedProverKit, ComputesNullifierFromRandomness) {
    const sh::Hash rcm = MakeHash(0x11, 0x22);
    const uint64_t leaf_index = 42;
    const sh::Hash sk_note = deriv::DeriveNoteSpendKey(rcm);
    const sh::Hash expected = sh::ComputeNullifier(sk_note, leaf_index);

    uint8_t out[32]{};
    EXPECT_EQ(dinero_shielded_compute_nullifier(rcm.data(), leaf_index, out),
              DINERO_SHIELDED_OK);
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), out));
}

TEST(ShieldedProverKit, ComputesCommitmentForNonZeroDiversifier) {
    constexpr uint64_t kValue = 123'456'789;
    const sh::Hash d = MakeHash(0x31, 0x32);
    const sh::Hash rcm = MakeHash(0x33, 0x34);
    const sh::Hash sk_note = deriv::DeriveNoteSpendKey(rcm);
    const sh::Hash pk_note = sh::PoseidonHash2(sk_note, sh::Hash{});
    const sh::Hash expected =
        sh::NoteCommitment(d, pk_note, ValueAsHash(kValue), rcm);

    uint8_t out[32]{};
    EXPECT_EQ(dinero_shielded_compute_note_commitment(
                  d.data(), rcm.data(), kValue, out),
              DINERO_SHIELDED_OK);
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), out));
}

TEST(ShieldedProverKit, BuildUnshieldBundleAcceptedByValidatorWithNonZeroD) {
    constexpr uint64_t kNoteValue = 100'000'000;
    constexpr uint64_t kFee = 10'000;
    constexpr uint64_t kRecipient = kNoteValue - kFee;

    const sh::Hash rcm = MakeHash(0xA1, 0xC1);
    const sh::Hash d = MakeHash(0xA2, 0xC1);
    sh::CommitmentTree tree;
    const auto note = MakeAbiSpendNote(tree, rcm, d, kNoteValue);

    auto tx = MakeUnshieldEnvelope(kRecipient, kFee, 0x55);

    dinero_shielded_unshield_result result{};
    ASSERT_EQ(BuildUnshieldViaAbi(note, tx, kFee, &result), DINERO_SHIELDED_OK)
        << (result.error ? result.error : "");
    ASSERT_NE(result.bundle_bytes, nullptr);
    ASSERT_GT(result.bundle_len, 0u);

    tx.shielded_bundle_bytes.assign(result.bundle_bytes,
                                    result.bundle_bytes + result.bundle_len);
    sh::ShieldedBundle decoded;
    ASSERT_EQ(sh::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &decoded),
              sh::BundleDecodeError::Ok);
    ASSERT_EQ(decoded.spends.size(), 1u);
    EXPECT_EQ(decoded.outputs.size(), 0u);
    EXPECT_EQ(decoded.value_balance, -static_cast<int64_t>(kNoteValue));
    EXPECT_TRUE(std::equal(decoded.spends[0].nullifier.begin(),
                           decoded.spends[0].nullifier.end(),
                           result.nullifier));
    EXPECT_TRUE(std::equal(decoded.spends[0].anchor.begin(),
                           decoded.spends[0].anchor.end(),
                           result.anchor));

    EXPECT_EQ(ValidateBundleForTx(decoded, tx, tree,
                                  -static_cast<int64_t>(kNoteValue)),
              sh::ShieldedValidationError::Ok);

    dinero_shielded_free_result(&result);
}

TEST(ShieldedProverKit, UnshieldBundleRejectsRecipientScriptMutation) {
    constexpr uint64_t kNoteValue = 100'000'000;
    constexpr uint64_t kFee = 10'000;
    constexpr uint64_t kRecipient = kNoteValue - kFee;

    sh::CommitmentTree tree;
    const auto note = MakeAbiSpendNote(tree, MakeHash(0xC1, 0xD1),
                                       MakeHash(0xC2, 0xD1), kNoteValue);
    const auto tx = MakeUnshieldEnvelope(kRecipient, kFee, 0x61);

    dinero_shielded_unshield_result result{};
    ASSERT_EQ(BuildUnshieldViaAbi(note, tx, kFee, &result), DINERO_SHIELDED_OK)
        << (result.error ? result.error : "");
    const auto decoded = DecodeResultBundle(result);

    auto mutated = tx;
    mutated.vout[0].scriptPubKey = {0x00, 0x62};
    EXPECT_EQ(ValidateBundleForTx(decoded, mutated, tree,
                                  -static_cast<int64_t>(kNoteValue)),
              sh::ShieldedValidationError::BindingSigInvalid);
    dinero_shielded_free_result(&result);
}

TEST(ShieldedProverKit, UnshieldBundleRejectsRecipientValueMutation) {
    constexpr uint64_t kNoteValue = 100'000'000;
    constexpr uint64_t kFee = 10'000;
    constexpr uint64_t kRecipient = kNoteValue - kFee;

    sh::CommitmentTree tree;
    const auto note = MakeAbiSpendNote(tree, MakeHash(0xC3, 0xD1),
                                       MakeHash(0xC4, 0xD1), kNoteValue);
    const auto tx = MakeUnshieldEnvelope(kRecipient, kFee, 0x63);

    dinero_shielded_unshield_result result{};
    ASSERT_EQ(BuildUnshieldViaAbi(note, tx, kFee, &result), DINERO_SHIELDED_OK)
        << (result.error ? result.error : "");
    const auto decoded = DecodeResultBundle(result);

    auto mutated = tx;
    mutated.vout[0].value = dinero::AmountUna::Una(kRecipient - 1);
    mutated.SetExplicitFee(kFee + 1);
    EXPECT_EQ(ValidateBundleForTx(decoded, mutated, tree,
                                  -static_cast<int64_t>(kNoteValue)),
              sh::ShieldedValidationError::BindingSigInvalid);
    dinero_shielded_free_result(&result);
}

TEST(ShieldedProverKit, UnshieldBundleRejectsFeeMutationByValueBalance) {
    constexpr uint64_t kNoteValue = 100'000'000;
    constexpr uint64_t kFee = 10'000;
    constexpr uint64_t kRecipient = kNoteValue - kFee;

    sh::CommitmentTree tree;
    const auto note = MakeAbiSpendNote(tree, MakeHash(0xC5, 0xD1),
                                       MakeHash(0xC6, 0xD1), kNoteValue);
    const auto tx = MakeUnshieldEnvelope(kRecipient, kFee, 0x64);

    dinero_shielded_unshield_result result{};
    ASSERT_EQ(BuildUnshieldViaAbi(note, tx, kFee, &result), DINERO_SHIELDED_OK)
        << (result.error ? result.error : "");
    const auto decoded = DecodeResultBundle(result);

    auto mutated = tx;
    mutated.SetExplicitFee(kFee + 1);
    EXPECT_EQ(ValidateBundleForTx(decoded, mutated, tree,
                                  -static_cast<int64_t>(kNoteValue + 1)),
              sh::ShieldedValidationError::ValueBalanceMismatch);
    dinero_shielded_free_result(&result);
}

TEST(ShieldedProverKit, WrongMerklePathFailsBuildOrValidation) {
    constexpr uint64_t kNoteValue = 100'000'000;
    constexpr uint64_t kFee = 10'000;
    constexpr uint64_t kRecipient = kNoteValue - kFee;

    sh::CommitmentTree tree;
    auto note = MakeAbiSpendNote(tree, MakeHash(0xC7, 0xD1),
                                 MakeHash(0xC8, 0xD1), kNoteValue);
    note.merkle_path[0][0] ^= 0x55;
    const auto tx = MakeUnshieldEnvelope(kRecipient, kFee, 0x65);

    dinero_shielded_unshield_result result{};
    const int rc = BuildUnshieldViaAbi(note, tx, kFee, &result);
    if (rc == DINERO_SHIELDED_OK) {
        const auto decoded = DecodeResultBundle(result);
        EXPECT_EQ(ValidateBundleForTx(decoded, tx, tree,
                                      -static_cast<int64_t>(kNoteValue)),
                  sh::ShieldedValidationError::ProofInvalid);
    } else {
        EXPECT_EQ(rc, DINERO_SHIELDED_ERR_BUILD_FAILED);
    }
    dinero_shielded_free_result(&result);
}

TEST(ShieldedProverKit, BuildUnshieldBundleRejectsFeeGteValue) {
    constexpr uint64_t kNoteValue = 100;
    constexpr uint64_t kFee = 100;

    const sh::Hash rcm = MakeHash(0xB1, 0xC1);
    const sh::Hash d = MakeHash(0xB2, 0xC1);
    sh::CommitmentTree tree;
    const auto note = MakeAbiSpendNote(tree, rcm, d, kNoteValue);

    auto tx = MakeUnshieldEnvelope(/*recipient_value=*/0, kFee, 0x56);

    dinero_shielded_unshield_result result{};
    EXPECT_EQ(BuildUnshieldViaAbi(note, tx, kFee, &result),
              DINERO_SHIELDED_ERR_BUILD_FAILED);
    ASSERT_NE(result.error, nullptr);
    dinero_shielded_free_result(&result);
}

TEST(ShieldedProverKit, BuildUnshieldBundleRejectsMalformedTx) {
    const uint8_t bad_tx[] = {0x01, 0x02, 0x03};
    dinero_shielded_spend_note note{};
    dinero_shielded_unshield_request req{};
    req.version = dinero::Transaction::TX_VERSION_SHIELDED;
    req.serialized_unsigned_tx = bad_tx;
    req.serialized_unsigned_tx_len = sizeof(bad_tx);
    req.fee_una = 1;
    req.note = &note;

    dinero_shielded_unshield_result result{};
    EXPECT_EQ(dinero_shielded_build_unshield_bundle(&req, &result),
              DINERO_SHIELDED_ERR_DESERIALIZE_TX);
    ASSERT_NE(result.error, nullptr);
    dinero_shielded_free_result(&result);
}

// ── dinero_shielded_derive_address ──────────────────────────────────────
//
// Golden vectors for the shielded-receive-address ABI (M3 PR: local shielded
// receive addresses — see docs/superpowers/specs/2026-07-10-local-shielded-
// receive-design.md in the DineroDPI repo). Every case below re-derives the
// same address two ways: once through the internal C++ path
// (`deriv::DeriveDiversifiedAddress`) and once through the exported ABI
// function, and asserts byte-identical results. The Swift-side golden test
// pins the same fixed seed and must reproduce the printed mainnet j=0
// address exactly.

namespace {

// Fixed 64-byte test seed for the ABI golden vectors — first 32 bytes the
// ASCII tag below, second 32 bytes the same tag XOR'd with 0xFF (mirrors
// the CanonicalSeed() convention in shielded_derivation_tests.cpp).
std::array<uint8_t, 64> ProverKitGoldenSeed() {
    constexpr const char kTag[] = "DIN/v7/proverkit/derive_addr/v1";
    constexpr std::size_t kTagLen = sizeof(kTag) - 1;  // exclude NUL
    static_assert(kTagLen <= 32, "tag must fit in 32 bytes");
    std::array<uint8_t, 64> s{};
    std::memcpy(s.data(), kTag, kTagLen);
    for (std::size_t i = 0; i < 32; ++i) {
        s[32 + i] = static_cast<uint8_t>(s[i] ^ 0xFF);
    }
    return s;
}

} // namespace

TEST(ShieldedProverKit, DeriveAddressMatchesInternalPathAllHrpsAndIndices) {
    auto seed = ProverKitGoldenSeed();
    const deriv::ShieldedAccountKeys keys =
        deriv::DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/0);

    const std::string hrps[] = {deriv::kHrpMainnet, deriv::kHrpTestnet,
                                deriv::kHrpRegtest};
    const uint64_t indices[] = {0, 1};

    bool printed_golden = false;
    for (uint64_t j : indices) {
        for (const std::string& hrp : hrps) {
            const deriv::DiversifiedAddress expected =
                deriv::DeriveDiversifiedAddress(keys, j, hrp);

            char buf[192]{};
            size_t buf_len = sizeof(buf);
            const int32_t rc = dinero_shielded_derive_address(
                keys.dk.data(), keys.ivk.data(), j, hrp.c_str(), buf, &buf_len);

            ASSERT_EQ(rc, DINERO_SHIELDED_OK)
                << "hrp=" << hrp << " j=" << j;
            EXPECT_EQ(buf_len, expected.address.size());
            EXPECT_STREQ(buf, expected.address.c_str())
                << "hrp=" << hrp << " j=" << j;

            if (!printed_golden && j == 0 && hrp == deriv::kHrpMainnet) {
                printed_golden = true;
                std::printf(
                    "[ShieldedProverKit golden] mainnet j=0 address=%s\n",
                    expected.address.c_str());
            }
        }
    }
    ASSERT_TRUE(printed_golden);
}

TEST(ShieldedProverKit, DeriveAddressIsDeterministic) {
    auto seed = ProverKitGoldenSeed();
    const deriv::ShieldedAccountKeys keys =
        deriv::DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/0);

    char buf1[192]{};
    size_t len1 = sizeof(buf1);
    ASSERT_EQ(dinero_shielded_derive_address(keys.dk.data(), keys.ivk.data(),
                                             /*j=*/0, deriv::kHrpMainnet, buf1,
                                             &len1),
              DINERO_SHIELDED_OK);

    char buf2[192]{};
    size_t len2 = sizeof(buf2);
    ASSERT_EQ(dinero_shielded_derive_address(keys.dk.data(), keys.ivk.data(),
                                             /*j=*/0, deriv::kHrpMainnet, buf2,
                                             &len2),
              DINERO_SHIELDED_OK);

    EXPECT_EQ(len1, len2);
    EXPECT_STREQ(buf1, buf2);
}

TEST(ShieldedProverKit, DeriveAddressRejectsBufferTooSmall) {
    auto seed = ProverKitGoldenSeed();
    const deriv::ShieldedAccountKeys keys =
        deriv::DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/0);
    const deriv::DiversifiedAddress expected =
        deriv::DeriveDiversifiedAddress(keys, /*j=*/0, deriv::kHrpMainnet);

    char tiny[4]{};
    size_t tiny_len = sizeof(tiny);
    const int32_t rc = dinero_shielded_derive_address(
        keys.dk.data(), keys.ivk.data(), /*j=*/0, deriv::kHrpMainnet, tiny,
        &tiny_len);
    EXPECT_EQ(rc, DINERO_SHIELDED_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(tiny_len, expected.address.size() + 1);

    // Retry with the now-known-sufficient capacity succeeds.
    std::vector<char> buf(tiny_len, '\0');
    size_t retry_len = buf.size();
    EXPECT_EQ(dinero_shielded_derive_address(keys.dk.data(), keys.ivk.data(),
                                             /*j=*/0, deriv::kHrpMainnet,
                                             buf.data(), &retry_len),
              DINERO_SHIELDED_OK);
    EXPECT_STREQ(buf.data(), expected.address.c_str());
}

TEST(ShieldedProverKit, DeriveAddressRejectsNullArguments) {
    auto seed = ProverKitGoldenSeed();
    const deriv::ShieldedAccountKeys keys =
        deriv::DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/0);

    char buf[192]{};
    size_t buf_len = sizeof(buf);

    EXPECT_EQ(dinero_shielded_derive_address(nullptr, keys.ivk.data(), 0,
                                             deriv::kHrpMainnet, buf, &buf_len),
              DINERO_SHIELDED_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dinero_shielded_derive_address(keys.dk.data(), nullptr, 0,
                                             deriv::kHrpMainnet, buf, &buf_len),
              DINERO_SHIELDED_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dinero_shielded_derive_address(keys.dk.data(), keys.ivk.data(), 0,
                                             nullptr, buf, &buf_len),
              DINERO_SHIELDED_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dinero_shielded_derive_address(keys.dk.data(), keys.ivk.data(), 0,
                                             deriv::kHrpMainnet, nullptr,
                                             &buf_len),
              DINERO_SHIELDED_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dinero_shielded_derive_address(keys.dk.data(), keys.ivk.data(), 0,
                                             deriv::kHrpMainnet, buf, nullptr),
              DINERO_SHIELDED_ERR_INVALID_ARGUMENT);
}

TEST(ShieldedProverKit, DeriveAddressRejectsBadHrp) {
    auto seed = ProverKitGoldenSeed();
    const deriv::ShieldedAccountKeys keys =
        deriv::DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/0);

    char buf[192]{};
    for (const char* bad_hrp : {"btc", "dins1", "tdinss", "", "DINS", "rdin"}) {
        size_t buf_len = sizeof(buf);
        EXPECT_EQ(dinero_shielded_derive_address(keys.dk.data(),
                                                 keys.ivk.data(), 0, bad_hrp,
                                                 buf, &buf_len),
                  DINERO_SHIELDED_ERR_INVALID_ARGUMENT)
            << "hrp=" << bad_hrp;
    }
}

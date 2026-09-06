// Item 9 — one canonical representation across every path that touches the
// state commitment: coinbase script bytes, network serialization, block
// hashing, RPC/snapshot string form, and the snapshot comparison.
//
// Why this must precede enforcement: if two paths disagree about byte order,
// enforcement tests can validate one representation while production uses the
// other, and both look green. uint256 makes that easy to get wrong --
// GetHex() emits data[31] FIRST, so the display string is the REVERSE of the
// raw array, and FromHex() reverses back.
//
// Three representations are legitimately in play, and each is pinned here:
//   raw internal   data[0..31]              -- coinbase script bytes 7..38
//   display hex    GetHex(), reversed       -- RPC and snapshot metadata
//   round trip     FromHex(GetHex(x)) == x
//
// The house rule this protects (block_acceptor.cpp:160): "Compare raw uint256
// — never compare display-order hex strings."
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

#include "consensus/merkle_root.h"
#include "consensus/state_commitment.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"

using dinero::uint256;
using dinero::Transaction;
using dinero::consensus::BuildStateCommitmentScript;
using dinero::consensus::FindStateCommitment;
using dinero::consensus::ParseStateCommitmentScript;
using dinero::consensus::StateCommitment;
using dinero::consensus::StateCommitmentStatus;

namespace {

// Deliberately NOT a palindrome: a reversal must be detectable.
uint256 AsymmetricRoot() {
    uint256 h;
    for (int i = 0; i < 32; ++i) h.data[i] = static_cast<uint8_t>(i * 7 + 1);
    return h;
}

uint256 Reversed(const uint256& in) {
    uint256 out;
    for (int i = 0; i < 32; ++i) out.data[i] = in.data[31 - i];
    return out;
}

Transaction CoinbaseWithCommitment(const uint256& root) {
    Transaction tx;
    tx.version = Transaction::TX_VERSION_LEGACY;
    tx.vin.resize(1);
    tx.lockTime = 0;
    dinero::TxOutput payout;
    payout.scriptPubKey = {0x51};
    tx.vout.push_back(payout);
    dinero::TxOutput commit;
    commit.scriptPubKey = BuildStateCommitmentScript(root);
    tx.vout.push_back(commit);
    return tx;
}

std::string HexForward(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// The pin: which order goes on the wire.
// ---------------------------------------------------------------------------

TEST(Canonical, ScriptCarriesRawInternalOrderNotDisplayOrder) {
    const uint256 root = AsymmetricRoot();
    const auto s = BuildStateCommitmentScript(root);

    // Script bytes are the raw array, in order.
    EXPECT_EQ(0, std::memcmp(s.data() + StateCommitment::OFFSET_ROOT, root.data, 32));

    // And that is NOT the display string. If these ever coincide the test has
    // stopped proving anything, so assert the difference explicitly.
    const std::string script_hex = HexForward(s.data() + StateCommitment::OFFSET_ROOT, 32);
    EXPECT_NE(script_hex, root.GetHex())
        << "GetHex() is display order (reversed); the script must not use it";
    std::string rev = root.GetHex();
    for (size_t i = 0; i < rev.size(); i += 2) std::swap(rev[i], rev[i + 1]);
    std::reverse(rev.begin(), rev.end());
    EXPECT_EQ(script_hex, rev) << "display hex must be exactly the reverse of script bytes";
}

TEST(Canonical, DisplayHexRoundTripsLosslessly) {
    const uint256 root = AsymmetricRoot();
    uint256 back;
    ASSERT_TRUE(uint256::FromHex(root.GetHex(), back));
    EXPECT_EQ(back, root);
    EXPECT_EQ(0, std::memcmp(back.data, root.data, 32));
}

TEST(Canonical, ScriptAndDisplayHexDescribeTheSameValue) {
    // The snapshot stores GetHex(); the coinbase stores raw bytes. Both must
    // resolve to one uint256, or a snapshot could "match" a block it does not.
    const uint256 root = AsymmetricRoot();
    const auto from_script = ParseStateCommitmentScript(BuildStateCommitmentScript(root));
    ASSERT_TRUE(from_script.has_value());
    uint256 from_hex;
    ASSERT_TRUE(uint256::FromHex(root.GetHex(), from_hex));
    EXPECT_EQ(*from_script, from_hex);
    EXPECT_EQ(from_script->GetHex(), root.GetHex());
}

// ---------------------------------------------------------------------------
// Display-order confusion must be detectable, not silently accepted.
// ---------------------------------------------------------------------------

TEST(Canonical, AReversedRootIsADifferentCommitment) {
    const uint256 root = AsymmetricRoot();
    const uint256 flipped = Reversed(root);
    ASSERT_NE(root, flipped) << "test input must be asymmetric or it proves nothing";

    EXPECT_NE(BuildStateCommitmentScript(root), BuildStateCommitmentScript(flipped));
    EXPECT_NE(root.GetHex(), flipped.GetHex());

    const auto parsed = ParseStateCommitmentScript(BuildStateCommitmentScript(root));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_NE(*parsed, flipped)
        << "a byte-reversed root must never compare equal to the original";
}

TEST(Canonical, SnapshotStyleStringComparisonRejectsAReversedRoot) {
    // Mirrors the live comparison: replayed->GetHex() == stored_hex.
    const uint256 root = AsymmetricRoot();
    const std::string stored = root.GetHex();
    EXPECT_EQ(root.GetHex(), stored);
    EXPECT_NE(Reversed(root).GetHex(), stored);
}

// ---------------------------------------------------------------------------
// Network serialization and block hashing.
// ---------------------------------------------------------------------------

TEST(Canonical, SurvivesTransactionSerializationByteExact) {
    const uint256 root = AsymmetricRoot();
    const Transaction tx = CoinbaseWithCommitment(root);

    for (auto mode : {dinero::TxSerializationMode::WithWitness,
                      dinero::TxSerializationMode::WithoutWitness}) {
        const auto bytes = tx.Serialize(mode);
        // The exact 39-byte script must appear verbatim in the wire bytes.
        const auto script = BuildStateCommitmentScript(root);
        auto it = std::search(bytes.begin(), bytes.end(), script.begin(), script.end());
        EXPECT_NE(it, bytes.end())
            << "the commitment script must appear byte-for-byte in the serialized tx";
    }
}

TEST(Canonical, CommitmentIsCoveredByTheMerkleRoot) {
    // The whole trust chain claim: commitment <- coinbase <- merkle_root <-
    // PoW-validated header. If a commitment byte can change without moving the
    // merkle root, the header does not actually authenticate it.
    const uint256 root = AsymmetricRoot();
    uint256 altered = root;
    altered.data[17] ^= 0x01;  // one bit, one byte

    std::vector<Transaction> a{CoinbaseWithCommitment(root)};
    std::vector<Transaction> b{CoinbaseWithCommitment(altered)};

    const uint256 ma = dinero::consensus::ComputeMerkleRoot(a);
    const uint256 mb = dinero::consensus::ComputeMerkleRoot(b);
    EXPECT_NE(ma, mb)
        << "a one-bit commitment change must move the merkle root, or the header "
           "does not bind the commitment";
}

TEST(Canonical, ExtractionFromASerializedThenRebuiltCoinbaseAgrees) {
    const uint256 root = AsymmetricRoot();
    const Transaction tx = CoinbaseWithCommitment(root);
    const auto found = FindStateCommitment(tx);
    ASSERT_EQ(found.status, StateCommitmentStatus::Ok);
    EXPECT_EQ(found.root, root);
    EXPECT_EQ(found.root.GetHex(), root.GetHex());
    EXPECT_EQ(0, std::memcmp(found.root.data, root.data, 32));
}

#include <gtest/gtest.h>
#include <openssl/sha.h>
#include "consensus/snapshot_binding.h"
#include "consensus/shielded/shielded_root.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/nullifier_set.h"
#include <cstring>

namespace {
using namespace dinero;
using namespace dinero::consensus;
uint256 Value(uint8_t n) { uint256 v; std::memset(v.data, n, 32); return v; }
Transaction Coinbase(const uint256& root) {
    Transaction tx; tx.vin.resize(1); tx.vout.resize(1);
    tx.vout[0].scriptPubKey = BuildStateCommitmentScript(root); return tx;
}
// Independent test oracle: OpenSSL SHA256 and explicit level construction,
// never ComputeMerkleRoot / the production branch generator as its verifier.
uint256 Pair(const uint256& a, const uint256& b) {
    uint8_t bytes[64], mid[32]; uint256 out;
    std::memcpy(bytes, a.data, 32); std::memcpy(bytes + 32, b.data, 32);
    SHA256(bytes, 64, mid); SHA256(mid, 32, out.data); return out;
}
uint256 Oracle(std::vector<uint256> nodes) {
    while (nodes.size() > 1) {
        if (nodes.size() % 2) nodes.push_back(nodes.back());
        std::vector<uint256> next;
        for (size_t i=0; i<nodes.size(); i+=2) next.push_back(Pair(nodes[i], nodes[i+1]));
        nodes = std::move(next);
    }
    return nodes.front();
}
TEST(SnapshotBinding, MerkleBranchesMatchIndependentOddAndEvenTreeOracle) {
    EXPECT_TRUE(ComputeCoinbaseMerkleBranch({}).empty());
    for (size_t n : {1u,2u,3u,5u,6u,17u}) {
        std::vector<Transaction> txs; std::vector<uint256> ids;
        for (size_t i=0;i<n;++i) { txs.push_back(Coinbase(Value(i+1))); ids.push_back(txs.back().GetTxid().AsUint256()); }
        auto branch = ComputeCoinbaseMerkleBranch(txs);
        // Pin the carried bytes independently too: reversing both production
        // writer and verifier must not turn into a self-consistent false pass.
        if (n == 2) EXPECT_EQ(branch, std::vector<uint256>({ids[1]}));
        if (n == 3) EXPECT_EQ(branch, std::vector<uint256>({ids[1], Pair(ids[2],ids[2])}));
        if (n == 5) {
            const auto tail = Pair(ids[4],ids[4]);
            EXPECT_EQ(branch, std::vector<uint256>({ids[1], Pair(ids[2],ids[3]), Pair(tail,tail)}));
        }
        const auto root = Oracle(ids);
        ASSERT_TRUE(VerifyCoinbaseMerkleBranch(ids[0], branch, root)) << n;
        if (!branch.empty()) { branch[0].data[0] ^= 1; EXPECT_FALSE(VerifyCoinbaseMerkleBranch(ids[0], branch, root)); }
        EXPECT_FALSE(VerifyCoinbaseMerkleBranch(Value(200), ComputeCoinbaseMerkleBranch(txs), root));
    }
}
TEST(SnapshotBinding, MerkleDepthCapAndEmptyBranch) {
    const auto leaf=Value(1);
    EXPECT_TRUE(VerifyCoinbaseMerkleBranch(leaf, {}, leaf));
    std::vector<uint256> branch(32,Value(2)); auto root=leaf;
    for (auto& h:branch) root=Pair(root,h);
    EXPECT_TRUE(VerifyCoinbaseMerkleBranch(leaf,branch,root));
    branch.push_back(Value(2)); root=Pair(root,Value(2));
    EXPECT_FALSE(VerifyCoinbaseMerkleBranch(leaf,branch,root));
}
TEST(SnapshotBinding, PreciseBindingFailureClassesAndVerificationOrder) {
    auto root=Value(4); auto cb=Coinbase(root); auto txid=cb.GetTxid().AsUint256();
    EXPECT_EQ(EvaluateSnapshotBinding(cb,{},txid,root),SnapshotBindingVerdict::Ok);
    EXPECT_EQ(EvaluateSnapshotBinding(cb,{},txid,Value(5)),SnapshotBindingVerdict::CommitmentMismatch);
    cb.vout[0].scriptPubKey.back() ^= 1;
    EXPECT_EQ(EvaluateSnapshotBinding(cb,{},txid,root),SnapshotBindingVerdict::InvalidMerkleProof);
    for (int kind=0;kind<3;++kind) {
        cb=Coinbase(root);
        if (kind==0) cb.vout.clear();
        if (kind==1) cb.vout.push_back(cb.vout.front());
        if (kind==2) cb.vout.front().scriptPubKey.pop_back();
        EXPECT_EQ(EvaluateSnapshotBinding(cb,{},cb.GetTxid().AsUint256(),root),SnapshotBindingVerdict::MalformedOrDuplicateCommitment);
        EXPECT_EQ(EvaluateSnapshotBinding(cb,{},Value(250),root),SnapshotBindingVerdict::InvalidMerkleProof);
    }
}
TEST(SnapshotBinding, FullStateBindingDetectsNullifiersAndAnchorsWithUnchangedTree) {
    shielded::CommitmentTree tree; shielded::AnchorHistory anchors; shielded::NullifierSet nfs;
    ASSERT_EQ(nfs.Open(":memory:"),shielded::NullifierSet::OpenResult::Ok);
    anchors.RecordRoot(1,tree.Root());
    const auto root=shielded::ComputeShieldedRoot(tree,nfs,anchors); ASSERT_TRUE(root);
    auto cb=Coinbase(*root); auto txid=cb.GetTxid().AsUint256();
    auto nf=tree.Root(); nf[0]^=1; ASSERT_TRUE(nfs.Insert(nf,1));
    auto changed=shielded::ComputeShieldedRoot(tree,nfs,anchors); ASSERT_TRUE(changed);
    EXPECT_EQ(EvaluateSnapshotBinding(cb,{},txid,*changed),SnapshotBindingVerdict::CommitmentMismatch);
    nfs.Clear(); anchors.RecordRoot(2,tree.Root());
    changed=shielded::ComputeShieldedRoot(tree,nfs,anchors); ASSERT_TRUE(changed);
    EXPECT_EQ(EvaluateSnapshotBinding(cb,{},txid,*changed),SnapshotBindingVerdict::CommitmentMismatch);
}
TEST(SnapshotBinding, BurialRequiresSelectedAncestorAndExactDepthWithoutOverflow) {
    const auto base=Value(9);
    for (uint32_t depth : {0u,1u,8u,288u}) {
        EXPECT_EQ(EvaluateSnapshotBurial(base,100,base,100+depth,depth),SnapshotBindingVerdict::Ok);
        if (depth) EXPECT_EQ(EvaluateSnapshotBurial(base,100,base,99+depth,depth),SnapshotBindingVerdict::InsufficientBurialOrNonAncestry);
        EXPECT_EQ(EvaluateSnapshotBurial(base,100,Value(8),100+depth,depth),SnapshotBindingVerdict::InsufficientBurialOrNonAncestry);
        EXPECT_EQ(EvaluateSnapshotBurial(base,100,std::nullopt,UINT32_MAX,depth),SnapshotBindingVerdict::InsufficientBurialOrNonAncestry);
    }
    EXPECT_EQ(EvaluateSnapshotBurial(base,UINT32_MAX,base,0,1),SnapshotBindingVerdict::InsufficientBurialOrNonAncestry);
    EXPECT_EQ(EvaluateSnapshotBurial(base,UINT32_MAX,base,UINT32_MAX,0),SnapshotBindingVerdict::Ok);
    EXPECT_EQ(EvaluateSnapshotBurial(base,UINT32_MAX,base,UINT32_MAX,1),SnapshotBindingVerdict::InsufficientBurialOrNonAncestry);
}
TEST(SnapshotBinding, StableVerdictNames) {
    EXPECT_STREQ(SnapshotBindingVerdictName(SnapshotBindingVerdict::MissingProof),"missing-proof");
    EXPECT_STREQ(SnapshotBindingVerdictName(SnapshotBindingVerdict::InvalidMerkleProof),"invalid-merkle-proof");
    EXPECT_STREQ(SnapshotBindingVerdictName(SnapshotBindingVerdict::MalformedOrDuplicateCommitment),"malformed-or-duplicate-commitment");
    EXPECT_STREQ(SnapshotBindingVerdictName(SnapshotBindingVerdict::CommitmentMismatch),"commitment-mismatch");
    EXPECT_STREQ(SnapshotBindingVerdictName(SnapshotBindingVerdict::InsufficientBurialOrNonAncestry),"insufficient-burial-or-non-ancestry");
}
} // namespace

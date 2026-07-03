#include <gtest/gtest.h>

#include <unordered_map>

#include "consensus/utxo_set_digest.h"
#include "consensus/consensus_utxo_set.h"   // OutPoint, UTXOEntry
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"        // TxId

namespace dinero::consensus {

namespace {
UTXOEntry MakeEntry(uint64_t value, uint32_t height, bool coinbase,
                    std::initializer_list<uint8_t> script) {
    UTXOEntry e{};
    e.value.v = value;
    e.height = height;
    e.isCoinbase = coinbase;
    e.scriptPubKey.assign(script);
    return e;
}

// Construct an OutPoint with txid_byte in data[0], rest zero.
// ExportSnapshot reads outpoint.txid.AsUint256().data — we build
// via TxId(uint256) so the raw bytes match exactly.
OutPoint MakeOutpoint(uint8_t txid_byte, uint32_t vout) {
    OutPoint op{};
    uint256 txid_hash;   // zero-initialized by uint256 default ctor
    txid_hash.data[0] = txid_byte;
    op.txid = TxId(txid_hash);
    op.vout = vout;
    return op;
}
}  // namespace

// The digest must be a pure function of CONTENT, independent of map order.
TEST(UtxoSetDigest, OrderIndependent) {
    std::unordered_map<OutPoint, UTXOEntry> a;
    a.emplace(MakeOutpoint(0x01, 0), MakeEntry(5000, 10, true,  {0x51}));
    a.emplace(MakeOutpoint(0x02, 1), MakeEntry(2500, 11, false, {0x52, 0x53}));

    std::unordered_map<OutPoint, UTXOEntry> b;
    b.emplace(MakeOutpoint(0x02, 1), MakeEntry(2500, 11, false, {0x52, 0x53}));
    b.emplace(MakeOutpoint(0x01, 0), MakeEntry(5000, 10, true,  {0x51}));

    EXPECT_EQ(ComputeUtxoRecordsDigest(a).GetHex(),
              ComputeUtxoRecordsDigest(b).GetHex());
}

// Any field change must change the digest.
TEST(UtxoSetDigest, FieldSensitive) {
    std::unordered_map<OutPoint, UTXOEntry> base;
    base.emplace(MakeOutpoint(0x01, 0), MakeEntry(5000, 10, true, {0x51}));
    const std::string d0 = ComputeUtxoRecordsDigest(base).GetHex();

    auto flip = [&](UTXOEntry e) {
        std::unordered_map<OutPoint, UTXOEntry> m;
        m.emplace(MakeOutpoint(0x01, 0), std::move(e));
        return ComputeUtxoRecordsDigest(m).GetHex();
    };
    EXPECT_NE(d0, flip(MakeEntry(5001, 10, true,  {0x51})));   // value
    EXPECT_NE(d0, flip(MakeEntry(5000, 11, true,  {0x51})));   // height
    EXPECT_NE(d0, flip(MakeEntry(5000, 10, false, {0x51})));   // coinbase
    EXPECT_NE(d0, flip(MakeEntry(5000, 10, true,  {0x52})));   // script
}

// The streaming accumulator over per-record bytes must equal the whole-set
// digest when fed the same records in sorted order (this is what LoadSnapshot
// uses while importing).
TEST(UtxoSetDigest, StreamingMatchesWholeSet) {
    std::unordered_map<OutPoint, UTXOEntry> m;
    m.emplace(MakeOutpoint(0x01, 0), MakeEntry(5000, 10, true,  {0x51}));
    m.emplace(MakeOutpoint(0x02, 1), MakeEntry(2500, 11, false, {0x52, 0x53}));
    m.emplace(MakeOutpoint(0x02, 0), MakeEntry(7500, 12, false, {}));

    StreamingUtxoDigest stream;
    // Feed in sorted-outpoint order, same as snapshot file order.
    std::vector<OutPoint> sorted;
    for (const auto& [op, _] : m) sorted.push_back(op);
    std::sort(sorted.begin(), sorted.end());
    for (const auto& op : sorted) {
        stream.AddRecord(op, m.at(op));
    }
    EXPECT_EQ(stream.Finalize().GetHex(), ComputeUtxoRecordsDigest(m).GetHex());
}

// Golden byte layout: the serializer must produce EXACTLY the snapshot-file
// record encoding (txid32 | vout4 | value8 | script_len4 | script | height4 |
// coinbase1, little-endian integers). If this test breaks, the snapshot file
// format changed and the commitment design must be revisited.
TEST(UtxoSetDigest, GoldenRecordBytes) {
    const auto op = MakeOutpoint(0xAB, 7);
    const auto e  = MakeEntry(0x1122334455667788ULL, 0x000000FF, true, {0xDE, 0xAD});
    const std::vector<uint8_t> bytes = SerializeUtxoRecord(op, e);
    ASSERT_EQ(bytes.size(), 32u + 4 + 8 + 4 + 2 + 4 + 1);
    EXPECT_EQ(bytes[0],  0xAB);   // txid[0]
    EXPECT_EQ(bytes[32], 0x07);   // vout LE low byte
    EXPECT_EQ(bytes[36], 0x88);   // value LE low byte (0x...7788 -> 0x88 first)
    EXPECT_EQ(bytes[44], 0x02);   // script_len LE low byte
    EXPECT_EQ(bytes[48], 0xDE);   // script[0]
    EXPECT_EQ(bytes[50], 0xFF);   // height LE low byte (0x000000FF -> 0xFF first)
    EXPECT_EQ(bytes[54], 0x01);   // coinbase
}

// ─────────────────────────────────────────────────────────────────────────
// #281: SerializeUtxoRecord omits UTXOEntry.is_confidential/commitment, so the
// snapshot content commitment does not bind them. On v7 that is safe only
// because those fields are consensus-forced empty (transparent-only chain).
// UtxoRecordIsSnapshotSafe turns that "always empty" assumption into an
// enforced, fail-closed invariant the exporter checks per UTXO: the moment a
// confidential UTXO could exist without the record encoding having been
// extended to bind these fields, export must refuse rather than silently
// produce an unbound snapshot.
// ─────────────────────────────────────────────────────────────────────────

TEST(UtxoSnapshotGuard, TransparentUtxoIsSafe) {
    UTXOEntry e = MakeEntry(5000, 10, false, {0x51});
    // MakeEntry leaves is_confidential=false and commitment empty.
    EXPECT_TRUE(UtxoRecordIsSnapshotSafe(e));
}

TEST(UtxoSnapshotGuard, ConfidentialFlagIsRejected) {
    UTXOEntry e = MakeEntry(5000, 10, false, {0x51});
    e.is_confidential = true;
    EXPECT_FALSE(UtxoRecordIsSnapshotSafe(e))
        << "a confidential UTXO must not be silently exported unbound (#281)";
}

TEST(UtxoSnapshotGuard, NonEmptyCommitmentIsRejected) {
    UTXOEntry e = MakeEntry(5000, 10, false, {0x51});
    // Even with the flag unset, a stray commitment payload would be dropped by
    // SerializeUtxoRecord — reject it rather than export an unbound record.
    e.commitment = {0x08, 0x01, 0x02, 0x03};
    EXPECT_FALSE(UtxoRecordIsSnapshotSafe(e))
        << "a non-empty commitment must not be silently dropped from the digest (#281)";
}

}  // namespace dinero::consensus

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

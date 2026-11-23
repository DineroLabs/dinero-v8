#include "wallet/swap/btc_merkle_proof.h"

#include "crypto/sha256.h"
#include "wallet/swap/btc_raw_tx.h"

namespace dinero::swap {
namespace {

Bytes32 HashPair(const Bytes32& left, const Bytes32& right) {
    Bytes32 once{}, twice{};
    crypto::CSHA256().Write(left.data(), left.size()).Write(right.data(), right.size()).Finalize(once.data());
    crypto::CSHA256().Write(once.data(), once.size()).Finalize(twice.data());
    return twice;
}

// Reads a transaction a source supplied, or says why it cannot stand as a leaf.
std::optional<std::string> Parse(const std::vector<uint8_t>& raw, BtcRawTx& out) {
    auto tx = ParseBtcRawTx(raw);
    if (!tx) return "not a transaction";
    if (tx->stripped_size == 64) return "a 64-byte transaction cannot be told from an inner tree node";
    out = std::move(*tx);
    return std::nullopt;
}

}  // namespace

std::optional<Bytes32> BtcMerkleRootFromProof(const Bytes32& txid, const BtcMerkleProof& proof) {
    if (proof.siblings.size() > kBtcMaxMerkleDepth) return std::nullopt;
    if (static_cast<uint64_t>(proof.position) >> proof.siblings.size() != 0) return std::nullopt;
    Bytes32 node = txid;
    uint32_t position = proof.position;
    for (const Bytes32& sibling : proof.siblings) {
        node = (position & 1) ? HashPair(sibling, node) : HashPair(node, sibling);
        position >>= 1;
    }
    return node;
}

std::optional<std::string> BtcCheckTxInclusion(const BtcTxInclusion& inclusion, const BtcBlockHeader& header) {
    BtcRawTx tx, coinbase;
    if (const auto why = Parse(inclusion.raw_tx, tx)) return "transaction: " + *why;
    if (const auto why = Parse(inclusion.raw_coinbase_tx, coinbase)) return "coinbase: " + *why;
    if (!coinbase.IsCoinbase()) return "coinbase: not a coinbase transaction";
    if (inclusion.coinbase_proof.position != 0) return "coinbase: not the block's first transaction";
    if (inclusion.proof.siblings.size() != inclusion.coinbase_proof.siblings.size()) {
        return "proof depth differs from the coinbase's";
    }
    const auto coinbase_root = BtcMerkleRootFromProof(coinbase.txid, inclusion.coinbase_proof);
    if (!coinbase_root) return "coinbase: malformed proof";
    if (*coinbase_root != header.merkle_root) return "coinbase: proof does not reach the block's Merkle root";
    const auto root = BtcMerkleRootFromProof(tx.txid, inclusion.proof);
    if (!root) return "malformed proof";
    if (*root != header.merkle_root) return "proof does not reach the block's Merkle root";
    return std::nullopt;
}

}  // namespace dinero::swap

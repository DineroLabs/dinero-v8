#pragma once
// Proof that a Bitcoin transaction is in a block
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3.2).
//
// A server says "transaction T is in block B". The node believes it only if
// T's hash, combined with the sibling hashes the server supplies, gives the
// Merkle root in B's header, and B is in the node's own header chain
// (btc_header_chain.h). The transaction is hashed here, from its bytes.
//
// Two known weaknesses of Bitcoin's Merkle tree are closed:
//   - A 64-byte transaction is indistinguishable from an inner node (two
//     32-byte hashes), so it could be passed off as one, or an inner node as a
//     transaction. A transaction whose serialization without witness is 64
//     bytes is refused.
//   - The tree does not commit to its depth. The block's first transaction (the
//     coinbase) is proven alongside, and both proofs must have the same number
//     of steps: a proof that goes one level too deep is refused.
//
// Pure: no I/O.

#include "wallet/swap/btc_header_chain.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dinero::swap {

// No Bitcoin block can hold 2^20 transactions (a megabyte of 60-byte ones is
// under 2^15); a longer proof is not a proof.
inline constexpr size_t kBtcMaxMerkleDepth = 20;

struct BtcMerkleProof {
    uint32_t position{};             // index of the transaction in the block
    std::vector<Bytes32> siblings;   // from the leaf's level up; wire byte order
};

// The root reached from `txid` (wire byte order) along `proof`; nullopt if the
// proof is too deep or its position does not fit its depth.
std::optional<Bytes32> BtcMerkleRootFromProof(const Bytes32& txid, const BtcMerkleProof& proof);

struct BtcTxInclusion {
    std::vector<uint8_t> raw_tx;           // the transaction, any serialization
    BtcMerkleProof proof;
    std::vector<uint8_t> raw_coinbase_tx;  // the block's first transaction
    BtcMerkleProof coinbase_proof;         // position 0
};

// nullopt: `raw_tx` is in the block with this header. Otherwise why not.
std::optional<std::string> BtcCheckTxInclusion(const BtcTxInclusion& inclusion, const BtcBlockHeader& header);

}  // namespace dinero::swap

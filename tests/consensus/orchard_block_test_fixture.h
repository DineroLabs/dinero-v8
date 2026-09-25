#pragma once
#include "orchard_state_test_fixture.h"
#include "primitives/orchard_block_reader.h"
#include "consensus/merkle_root.h"
#include "consensus/witness_commitment.h"

// Honest proof fixtures inside a syntactically valid block with real identity
// commitments. No mined PoW, Utreexo proof or full block validity is claimed.
static OrchardBlockCandidate Candidate(const OrchardBlockContext& context,
    std::span<const VerifiedOrchardAuthorizations> authorizations,
    const std::vector<Transaction>& extra = {}, uint32_t nonce = 0) {
    Require(authorizations.size()+extra.size()<252);
    Transaction coinbase;coinbase.version=2;
    TxInput input;input.prevout.vout=UINT32_MAX;input.scriptSig={1,1};coinbase.vin={input};
    coinbase.vout.emplace_back(AmountUna::Una(1),Bytes{0x51});
    std::vector<Bytes> wires;
    std::vector<WTxId> wids{WTxId(uint256())};
    for(const auto& auth:authorizations) {
        const auto& wire=auth.Orchard().CanonicalBytes();
        wires.push_back(wire);
        wids.push_back(ParsedTransaction::DecodeExact(wire,TransactionReadMode::StagedOrchard).GetWtxid());
    }
    for(const auto& tx:extra) {wires.push_back(tx.Serialize(TxSerializationMode::WithWitness));wids.push_back(tx.GetWtxid());}
    coinbase.vout.emplace_back(AmountUna::Zero(),consensus::BuildWitnessCommitmentFromRoot(
        consensus::ComputeWitnessMerkleRootFromIds(wids)));
    wires.insert(wires.begin(),coinbase.Serialize(TxSerializationMode::WithWitness));
    std::vector<TxId> ids;
    for(const auto& wire:wires)ids.push_back(ParsedTransaction::DecodeExact(wire,TransactionReadMode::StagedOrchard).GetTxid());
    BlockHeader header{};header.version=1;header.prev_block_hash=context.parent_hash;
    header.timestamp=context.height;header.nonce=nonce;header.merkle_root=consensus::ComputeTransactionMerkleRoot(ids);
    const auto prefix=header.SerializeForHash();Bytes bytes(prefix.begin(),prefix.end());bytes.push_back(wires.size());
    for(const auto& wire:wires)bytes.insert(bytes.end(),wire.begin(),wire.end());bytes.push_back(0);
    return OrchardBlockCandidate::DecodeExact(bytes);
}

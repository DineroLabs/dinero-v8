#include "primitives/orchard_block_reader.h"
#include "consensus/limits.h"
#include "consensus/merkle_root.h"
#include "consensus/witness_commitment.h"
#include <algorithm>
#include <stdexcept>

namespace dinero {
namespace {
[[noreturn]] void Invalid() { throw std::invalid_argument("invalid staged Orchard block framing"); }
uint64_t Count(std::span<const uint8_t> bytes, size_t& offset) {
    if (offset == bytes.size()) Invalid();
    const auto marker = bytes[offset++];
    if (marker < 253) return marker;
    const size_t width = marker == 253 ? 2 : marker == 254 ? 4 : 8;
    if (width > bytes.size() - offset) Invalid();
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= uint64_t(bytes[offset++]) << (8 * i);
    if ((width == 2 && value < 253) || (width == 4 && value <= 65535) ||
        (width == 8 && value <= UINT32_MAX)) Invalid();
    return value;
}
} // namespace
OrchardBlockCandidate OrchardBlockCandidate::DecodeExact(std::span<const uint8_t> bytes) {
    if (bytes.size() < 130 || bytes.size() > consensus::MAX_BLOCK_WEIGHT) Invalid();
    const auto header = BlockHeader::Deserialize(bytes.data(), 128);
    if (!header) Invalid();
    size_t offset = 128;
    const auto count = Count(bytes, offset);
    // Every historical transaction is at least ten bytes; Orchard is larger.
    // Bound count before reserving, independently of any transaction decoder.
    if (count == 0 || count > (bytes.size() - offset) / 10) Invalid();
    std::vector<ParsedTransaction> transactions;
    transactions.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        const auto [tx, used] = ParsedTransaction::DecodePrefix(bytes.subspan(offset), TransactionReadMode::StagedOrchard);
        if (used == 0 || used > bytes.size() - offset) Invalid();
        transactions.push_back(tx); offset += used;
    }
    // The new candidate format has one explicit suffix flag. This does not
    // retroactively require one in historical blocks, or tighten their parser.
    if (offset == bytes.size()) Invalid();
    const auto flag = bytes[offset++];
    std::optional<consensus::BlockUtreexoData> proof;
    if (flag == 0) {
        if (offset != bytes.size()) Invalid();
    } else if (flag == 1) {
        const std::vector<uint8_t> encoded(bytes.begin() + offset, bytes.end());
        proof = consensus::BlockUtreexoData::deserialize(encoded);
        // The existing decoder can return an empty object on malformed data;
        // never interpret that as successful new-format decoding. Serialization
        // also rejects ignored trailing bytes and noncanonical proof encodings.
        if (proof->serialize() != encoded) Invalid();
    } else Invalid();
    return OrchardBlockCandidate(*header, std::move(transactions), std::move(proof), {bytes.begin(), bytes.end()});
}
bool OrchardBlockCandidate::MatchesTransactionRoot() const {
    std::vector<TxId> ids; ids.reserve(transactions_.size());
    for (const auto& tx : transactions_) ids.push_back(tx.GetTxid());
    bool mutated = false;
    const auto root = consensus::ComputeTransactionMerkleRoot(ids, &mutated);
    return !mutated && root == header_.merkle_root;
}
uint256 OrchardBlockCandidate::WitnessRoot(bool* mutated) const {
    std::vector<WTxId> ids; ids.reserve(transactions_.size());
    for (const auto& tx : transactions_) ids.push_back(tx.GetWtxid());
    return consensus::ComputeWitnessMerkleRootFromIds(ids, mutated);
}
bool OrchardBlockCandidate::CheckIdentityCommitments(bool require_witness_commitment, std::string& error) const {
    if (transactions_.empty() || transactions_[0].IsOrchard() || !transactions_[0].Historical().IsCoinbase()) {
        error = "missing-historical-coinbase"; return false;
    }
    if (!MatchesTransactionRoot()) { error = "bad-transaction-root-or-duplicate"; return false; }
    const auto& coinbase = transactions_[0].Historical();
    bool mutated = false;
    const auto root = WitnessRoot(&mutated);
    if (!consensus::ValidateWitnessCommitmentRoot(coinbase, root, mutated, error)) return false;
    const bool has_witness = std::any_of(transactions_.begin(), transactions_.end(), [](const auto& tx) {
        if (!tx.IsOrchard()) return tx.Historical().HasWitness();
        return std::any_of(tx.Orchard().Inputs().begin(), tx.Orchard().Inputs().end(),
            [](const auto& input) { return !input.witness.empty(); });
    });
    if (require_witness_commitment && has_witness && !consensus::FindWitnessCommitmentIndex(coinbase)) {
        error = "missing-witness-commitment"; return false;
    }
    return true;
}
} // namespace dinero

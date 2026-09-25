#pragma once
#include "primitives/block.h"
#include "primitives/transaction_reader.h"

namespace dinero {
// Post-activation candidate codec only. No implicit conversion to the legacy
// Block type and no live parser/admission switch. Historical replay retains
// Block::Deserialize and its historical accepted encodings.
// Parsing authenticates neither PoW, scripts, proofs, nor state transitions.
class OrchardBlockCandidate {
public:
    [[nodiscard]] static OrchardBlockCandidate DecodeExact(std::span<const uint8_t>);
    const BlockHeader& Header() const noexcept { return header_; }
    const auto& Transactions() const noexcept { return transactions_; }
    const auto& Utreexo() const noexcept { return utreexo_; }
    const auto& WireBytes() const noexcept { return bytes_; }
    size_t BaseSize() const noexcept { return base_size_; }
    size_t Weight() const noexcept { return 3*base_size_+bytes_.size(); }
    // Size/weight only; sigops and Orchard proof-work budgets are separate.
    bool CheckSizeLimits(std::string& error) const;
    // These compare transaction identities, NOT transaction validity. The
    // Orchard proof and ciphertext are inside txid; transparent witnesses need
    // the separate DINW commitment check in the eventual block validator.
    bool MatchesTransactionRoot() const;
    uint256 WitnessRoot(bool* mutated = nullptr) const;
    // Validate both identity commitments. The selected-height caller decides
    // when DINW presence is mandatory; recognized commitments always validate.
    bool CheckIdentityCommitments(bool require_witness_commitment, std::string& error) const;
private:
    OrchardBlockCandidate(BlockHeader header, std::vector<ParsedTransaction> txs,
        std::optional<consensus::BlockUtreexoData> proof, std::vector<uint8_t> bytes,
        size_t base_size, bool transaction_sizes_valid)
        : header_(header), transactions_(std::move(txs)), utreexo_(std::move(proof)), bytes_(std::move(bytes)),
          base_size_(base_size), transaction_sizes_valid_(transaction_sizes_valid) {}
    const BlockHeader header_;
    const std::vector<ParsedTransaction> transactions_;
    const std::optional<consensus::BlockUtreexoData> utreexo_;
    const std::vector<uint8_t> bytes_;
    const size_t base_size_;
    const bool transaction_sizes_valid_;
};
} // namespace dinero

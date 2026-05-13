/**
 * Phase G.3.2: Structural Validation Implementation
 *
 * Pure structural validation - deserialize and check internal consistency.
 * NO script execution, NO UTXO access, NO consensus context.
 */

#include "../../include/p2p/structural_validator.h"

#include "consensus/limits.h"
#include "consensus/merkle_root.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <limits>
#include <optional>
#include <unordered_set>

namespace dinero {
namespace p2p {

namespace {

bool ReadCompactSize(const uint8_t* data, size_t len, size_t& offset, uint64_t& out) {
    if (!data || offset >= len) {
        return false;
    }

    const uint8_t first = data[offset++];
    if (first < 0xfd) {
        out = first;
        return true;
    }

    if (first == 0xfd) {
        if (offset + 2 > len) return false;
        out = static_cast<uint64_t>(data[offset]) |
              (static_cast<uint64_t>(data[offset + 1]) << 8);
        offset += 2;
        return true;
    }

    if (first == 0xfe) {
        if (offset + 4 > len) return false;
        out = static_cast<uint64_t>(data[offset]) |
              (static_cast<uint64_t>(data[offset + 1]) << 8) |
              (static_cast<uint64_t>(data[offset + 2]) << 16) |
              (static_cast<uint64_t>(data[offset + 3]) << 24);
        offset += 4;
        return true;
    }

    if (offset + 8 > len) return false;
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= (static_cast<uint64_t>(data[offset + i]) << (8 * i));
    }
    offset += 8;
    out = value;
    return true;
}

size_t CompactSizeLength(uint64_t value) {
    if (value < 0xfd) return 1;
    if (value <= 0xffff) return 3;
    if (value <= 0xffffffffULL) return 5;
    return 9;
}

size_t CountNonCoinbaseSpentInputs(const std::vector<dinero::Transaction>& transactions) {
    size_t spent_inputs = 0;
    for (size_t i = 1; i < transactions.size(); ++i) {
        if (spent_inputs > std::numeric_limits<size_t>::max() - transactions[i].vin.size()) {
            return std::numeric_limits<size_t>::max();
        }
        spent_inputs += transactions[i].vin.size();
    }
    return spent_inputs;
}

} // namespace

//=============================================================================
// Block Structural Validation
//=============================================================================

StructuralValidationResult StructuralValidator::validateBlock(const std::vector<uint8_t>& raw) {
    if (raw.empty()) {
        return StructuralValidationResult::Fail("Block payload is empty");
    }

    if (raw.size() > dinero::consensus::MAX_BLOCK_WEIGHT) {
        return StructuralValidationResult::Fail("Block exceeds maximum serialized size/weight limit");
    }

    if (raw.size() < MIN_BLOCK_HEADER_SIZE) {
        return StructuralValidationResult::Fail("Block header incomplete (< 128 bytes)");
    }

    auto header_opt = dinero::BlockHeader::Deserialize(raw.data(), raw.size());
    if (!header_opt.has_value()) {
        return StructuralValidationResult::Fail("Block header deserialization failed");
    }

    const auto& header = *header_opt;
    if (!header.IsReservedValid()) {
        return StructuralValidationResult::Fail("Block header reserved bytes must be zero");
    }

    size_t offset = MIN_BLOCK_HEADER_SIZE;
    uint64_t tx_count = 0;
    if (!ReadCompactSize(raw.data(), raw.size(), offset, tx_count)) {
        return StructuralValidationResult::Fail("Block transaction count deserialization failed");
    }
    if (tx_count == 0) {
        return StructuralValidationResult::Fail("Block must contain at least one transaction");
    }
    constexpr size_t MIN_SERIALIZED_TX_BYTES = 8;  // version + vin count + vout count + locktime
    const size_t remaining_bytes = raw.size() - offset;
    if (tx_count > remaining_bytes / MIN_SERIALIZED_TX_BYTES) {
        return StructuralValidationResult::Fail("Block transaction count is impossible for payload size");
    }

    std::vector<dinero::Transaction> transactions;
    for (uint64_t i = 0; i < tx_count; ++i) {
        if (offset >= raw.size()) {
            return StructuralValidationResult::Fail("Block truncated while parsing transactions");
        }

        std::vector<uint8_t> remaining(raw.begin() + static_cast<std::ptrdiff_t>(offset), raw.end());
        dinero::Transaction tx;
        size_t consumed = 0;
        if (!dinero::TransactionSerializer::Deserialize(tx, remaining, consumed) || consumed == 0) {
            return StructuralValidationResult::Fail("Transaction deserialization failed inside block");
        }
        if (offset + consumed > raw.size()) {
            return StructuralValidationResult::Fail("Transaction extends past end of block payload");
        }

        auto tx_result = validateParsedTx(tx);
        if (!tx_result.ok) {
            return tx_result;
        }

        transactions.push_back(std::move(tx));
        offset += consumed;
    }

    if (!transactions.front().IsCoinbase()) {
        return StructuralValidationResult::Fail("First block transaction must be coinbase");
    }
    for (size_t i = 1; i < transactions.size(); ++i) {
        if (transactions[i].IsCoinbase()) {
            return StructuralValidationResult::Fail("Only the first block transaction may be coinbase");
        }
    }

    if (dinero::consensus::ComputeMerkleRoot(transactions) != header.merkle_root) {
        return StructuralValidationResult::Fail("Block merkle root does not match serialized transactions");
    }

    if (offset < raw.size()) {
        const uint8_t flag = raw[offset++];
        if (flag == 0x00) {
            if (offset != raw.size()) {
                return StructuralValidationResult::Fail("Trailing bytes after empty Utreexo flag");
            }
        } else if (flag == 0x01) {
            try {
                std::vector<uint8_t> utreexo_bytes(raw.begin() + static_cast<std::ptrdiff_t>(offset), raw.end());
                const auto utreexo = dinero::consensus::BlockUtreexoData::deserialize(utreexo_bytes);
                if (utreexo.serialize().size() != utreexo_bytes.size()) {
                    return StructuralValidationResult::Fail("Trailing bytes after Utreexo payload");
                }
                if (utreexo.accumulator_root_before.size() != 32) {
                    return StructuralValidationResult::Fail("Block Utreexo root-before field must be 32 bytes");
                }
                if (!utreexo.spend_proof.isValid()) {
                    return StructuralValidationResult::Fail("Block Utreexo proof is structurally invalid");
                }
                if (utreexo.spend_proof.targets.size() > dinero::consensus::MAX_PROOF_TARGETS) {
                    return StructuralValidationResult::Fail("Block Utreexo proof has too many targets");
                }
                if (utreexo.spend_proof.proof_hashes.size() > dinero::consensus::MAX_PROOF_HASHES) {
                    return StructuralValidationResult::Fail("Block Utreexo proof has too many hashes");
                }

                const size_t spent_inputs = CountNonCoinbaseSpentInputs(transactions);
                if (spent_inputs == std::numeric_limits<size_t>::max()) {
                    return StructuralValidationResult::Fail("Block spend input count overflowed");
                }
                if (utreexo.spent_outputs.size() != spent_inputs) {
                    return StructuralValidationResult::Fail("Block Utreexo spent-output count does not match spent inputs");
                }
                if (utreexo.spend_proof.targets.size() != spent_inputs) {
                    return StructuralValidationResult::Fail("Block Utreexo proof target count does not match spent inputs");
                }
            } catch (const std::exception&) {
                return StructuralValidationResult::Fail("Block Utreexo payload deserialization failed");
            }
        } else {
            return StructuralValidationResult::Fail("Invalid block Utreexo flag");
        }
    }

    return StructuralValidationResult::Ok();
}

//=============================================================================
// Transaction Structural Validation
//=============================================================================

StructuralValidationResult StructuralValidator::validateTx(const std::vector<uint8_t>& raw) {
    if (raw.empty()) {
        return StructuralValidationResult::Fail("Transaction payload is empty");
    }

    if (raw.size() > dinero::consensus::MAX_TX_SIZE) {
        return StructuralValidationResult::Fail("Transaction exceeds maximum size");
    }

    if (raw.size() < 4) {
        return StructuralValidationResult::Fail("Transaction too small (< 4 bytes)");
    }

    dinero::Transaction tx;
    size_t consumed = 0;
    if (!dinero::TransactionSerializer::Deserialize(tx, raw, consumed)) {
        return StructuralValidationResult::Fail("Transaction deserialization failed");
    }
    if (consumed != raw.size()) {
        return StructuralValidationResult::Fail("Transaction has trailing or truncated bytes");
    }

    return validateParsedTx(tx);
}

StructuralValidationResult StructuralValidator::validateParsedTx(const dinero::Transaction& tx) const {
    if (tx.vin.empty()) {
        return StructuralValidationResult::Fail("Transaction must have at least one input");
    }

    if (tx.vout.empty()) {
        return StructuralValidationResult::Fail("Transaction must have at least one output");
    }
    if (tx.GetWeight() == 0 || tx.GetWeight() > dinero::consensus::MAX_TX_WEIGHT) {
        return StructuralValidationResult::Fail("Transaction weight exceeds limit");
    }

    std::unordered_set<dinero::TxOutPoint> seen_inputs;
    for (const auto& input : tx.vin) {
        if (!tx.IsCoinbase() &&
            input.prevout.txid.IsNull() &&
            input.prevout.vout == 0xffffffffU) {
            return StructuralValidationResult::Fail("Non-coinbase transaction has null prevout");
        }
        if (input.scriptSig.size() > dinero::consensus::MAX_SCRIPT_SIZE) {
            return StructuralValidationResult::Fail("Transaction input script exceeds maximum size");
        }
        for (const auto& witness_item : input.witness) {
            if (witness_item.size() > dinero::consensus::MAX_SCRIPT_SIZE) {
                return StructuralValidationResult::Fail("Transaction witness item exceeds maximum size");
            }
        }

        if (!seen_inputs.insert(input.prevout).second) {
            return StructuralValidationResult::Fail("Transaction has duplicate inputs");
        }
    }

    for (const auto& output : tx.vout) {
        if (output.scriptPubKey.size() > dinero::consensus::MAX_SCRIPT_SIZE) {
            return StructuralValidationResult::Fail("Transaction output script exceeds maximum size");
        }
    }

    return StructuralValidationResult::Ok();
}

} // namespace p2p
} // namespace dinero

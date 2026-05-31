#include "mining/block_template.h"

#include "common/logger.h"
#include "common/sha256d.h"
#include "consensus/freeze.h"
#include "consensus/subsidy.h"

#include <cstdlib>
#include <unordered_set>

static_assert(
    dinero::consensus::CONSENSUS_VERSION_MAJOR >= 1,
    "block_template.cpp requires dinero_consensus library (subsidy rules live there)"
);

namespace dinero {
namespace mining {

uint64_t BlockTemplateBuilder::getBlockSubsidy(uint32_t height) {
    return dinero::ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
}

std::string BlockTemplateBuilder::calculateMerkleRoot(const std::vector<Transaction>& transactions) {
    std::string merkle_root;
    std::vector<std::string> merkle_branches;

    if (!buildMerkleTree(transactions, merkle_root, merkle_branches)) {
        return std::string(64, '0');
    }

    return merkle_root;
}

bool BlockTemplateBuilder::buildMerkleTree(
    const std::vector<Transaction>& transactions,
    std::string& merkle_root,
    std::vector<std::string>& merkle_branches
) {
    merkle_branches.clear();

    if (transactions.empty()) {
        merkle_root = std::string(64, '0');
        return true;
    }

    std::vector<std::string> hashes;
    std::unordered_set<std::string> seen_txids;

    for (const auto& tx : transactions) {
        std::string txid = tx.GetTxid().AsUint256().GetHex();

        if (seen_txids.count(txid) > 0) {
            dinero::g_logger.error("[BlockTemplate] Duplicate txid detected: " + txid);
            return false;
        }

        seen_txids.insert(txid);
        hashes.push_back(txid);
    }

    std::vector<std::string> current_level = hashes;
    size_t coinbase_index = 0;

    while (current_level.size() > 1) {
        std::vector<std::string> next_level;

        if (coinbase_index < current_level.size()) {
            size_t sibling_index = (coinbase_index % 2 == 0) ? coinbase_index + 1 : coinbase_index - 1;

            if (sibling_index < current_level.size()) {
                merkle_branches.push_back(current_level[sibling_index]);
            } else if (coinbase_index == current_level.size() - 1 && current_level.size() % 2 != 0) {
                merkle_branches.push_back(current_level[coinbase_index]);
            }
        }

        if (current_level.size() % 2 != 0) {
            current_level.push_back(current_level.back());
        }

        for (size_t i = 0; i < current_level.size(); i += 2) {
            std::vector<uint8_t> combined;
            combined.reserve(64);

            for (size_t j = 0; j < 64; j += 2) {
                uint8_t byte = static_cast<uint8_t>(
                    std::strtol(current_level[i].substr(j, 2).c_str(), nullptr, 16)
                );
                combined.push_back(byte);
            }

            for (size_t j = 0; j < 64; j += 2) {
                uint8_t byte = static_cast<uint8_t>(
                    std::strtol(current_level[i + 1].substr(j, 2).c_str(), nullptr, 16)
                );
                combined.push_back(byte);
            }

            next_level.push_back(Dinero::Common::double_sha256(combined));
        }

        coinbase_index /= 2;
        current_level = next_level;
    }

    merkle_root = current_level[0];
    return true;
}

} // namespace mining
} // namespace dinero

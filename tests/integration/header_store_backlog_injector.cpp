#include "consensus/chainparams.h"
#include "consensus/header_chain.h"
#include "consensus/header_store.h"
#include "primitives/block.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

void Usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " <header-store-dir> [--count N] [--network regtest|testnet|mainnet]\n"
        << "\n"
        << "Appends N synthetic header-only descendants to the persisted HeaderStore\n"
        << "without writing any matching block bodies to ChainDB/blk*.dat.\n";
}

bool ParsePositiveInt(const std::string& value, int& out) {
    try {
        size_t consumed = 0;
        int parsed = std::stoi(value, &consumed);
        if (consumed != value.size() || parsed <= 0) {
            return false;
        }
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool SelectNetwork(const std::string& network) {
    if (network == "mainnet" || network == "main") {
        dinero::SelectParams(dinero::Chain::MAINNET);
        return true;
    }
    if (network == "testnet" || network == "test") {
        dinero::SelectParams(dinero::Chain::TESTNET);
        return true;
    }
    if (network == "regtest") {
        dinero::SelectParams(dinero::Chain::REGTEST);
        return true;
    }
    return false;
}

dinero::BlockHeader BuildDescendantHeader(const dinero::consensus::HeaderIndexEntry& tip,
                                          uint32_t sequence) {
    dinero::BlockHeader header;
    header.version = std::max<uint32_t>(tip.header.version, 1);
    header.prev_block_hash = tip.hash;
    header.merkle_root = dinero::uint256();
    header.timestamp = tip.header.timestamp + sequence;
    header.difficulty = tip.header.difficulty == 0
        ? dinero::Params().pow_limit_bits
        : tip.header.difficulty;
    header.nonce = tip.header.nonce + sequence;
    header.utreexo_root = dinero::uint256();
    return header;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        Usage(argv[0]);
        return 1;
    }

    std::filesystem::path header_store_dir = argv[1];
    int backlog_count = 1;
    std::string network = "regtest";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--count") {
            if (i + 1 >= argc || !ParsePositiveInt(argv[++i], backlog_count)) {
                std::cerr << "Invalid value for --count\n";
                return 1;
            }
        } else if (arg == "--network") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --network\n";
                return 1;
            }
            network = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            Usage(argv[0]);
            return 1;
        }
    }

    if (!SelectNetwork(network)) {
        std::cerr << "Unsupported network: " << network << "\n";
        return 1;
    }

    dinero::consensus::HeaderStore store(header_store_dir.string());
    if (!store.Open()) {
        std::cerr << "Failed to open HeaderStore at " << header_store_dir << "\n";
        return 1;
    }

    dinero::consensus::HeaderChainSelector selector(&store);
    const auto* best = selector.GetBestHeader();
    if (!best) {
        std::cerr << "HeaderStore has no persisted tip to extend\n";
        return 1;
    }

    const uint32_t base_height = best->height;
    const std::string base_hash = best->hash.GetHex();

    for (int i = 0; i < backlog_count; ++i) {
        const auto* current_best = selector.GetBestHeader();
        if (!current_best) {
            std::cerr << "Header selector lost its best header while extending backlog\n";
            return 1;
        }

        const dinero::BlockHeader header = BuildDescendantHeader(*current_best, static_cast<uint32_t>(i + 1));
        if (!selector.AddHeader(header)) {
            std::cerr << "Failed to append synthetic descendant at offset " << (i + 1) << "\n";
            return 1;
        }
    }

    const auto* final_best = selector.GetBestHeader();
    if (!final_best) {
        std::cerr << "Header selector lost best tip after extension\n";
        return 1;
    }

    std::cout << "mode=header-backlog-injected\n";
    std::cout << "store=" << header_store_dir << "\n";
    std::cout << "network=" << network << "\n";
    std::cout << "count=" << backlog_count << "\n";
    std::cout << "base_height=" << base_height << "\n";
    std::cout << "base_hash=" << base_hash << "\n";
    std::cout << "final_height=" << final_best->height << "\n";
    std::cout << "final_hash=" << final_best->hash.GetHex() << "\n";
    return 0;
}

#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " <chaindb-dir> [--source-offset-back N] [--bitflip-latest] [--inspect]\n"
        << "\n"
        << "Rewrites the latest Utreexo checkpoint bytes with an older checkpoint\n"
        << "while preserving a valid checksum. This simulates a stale/contaminated\n"
        << "CSN checkpoint that should be caught by startup root verification.\n"
        << "\n"
        << "If --bitflip-latest is provided, mutate the latest checkpoint bytes in\n"
        << "place instead of sourcing an older checkpoint. This is useful when a\n"
        << "test datadir only has a single persisted checkpoint.\n"
        << "\n"
        << "If --inspect is provided, make no changes and print the persisted\n"
        << "checkpoint count and latest height. Empty sets are reported safely.\n";
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        Usage(argv[0]);
        return 1;
    }

    std::filesystem::path chain_db_dir = argv[1];
    int source_offset_back = 1;
    bool bitflip_latest = false;
    bool inspect = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--source-offset-back") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --source-offset-back\n";
                return 1;
            }
            if (!ParsePositiveInt(argv[++i], source_offset_back)) {
                std::cerr << "Invalid --source-offset-back value\n";
                return 1;
            }
        } else if (arg == "--bitflip-latest") {
            bitflip_latest = true;
        } else if (arg == "--inspect") {
            inspect = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            Usage(argv[0]);
            return 1;
        }
    }

    dinero::ChainDB chain_db;
    auto init_status = chain_db.init(chain_db_dir);
    if (init_status != dinero::Status::Ok) {
        std::cerr << "Failed to open ChainDB at " << chain_db_dir << "\n";
        return 1;
    }

    auto heights_result = chain_db.listUtreexoCheckpoints();
    if (heights_result.status() != dinero::Status::Ok) {
        std::cerr << "Failed to list Utreexo checkpoints\n";
        return 1;
    }

    auto heights = heights_result.value();
    if (inspect) {
        std::cout << "mode=inspect\n";
        std::cout << "chaindb=" << chain_db_dir << "\n";
        std::cout << "checkpoint_count=" << heights.size() << "\n";
        if (!heights.empty()) {
            std::cout << "latest_height=" << heights.back() << "\n";
        }
        return 0;
    }

    if (heights.empty()) {
        std::cerr << "No Utreexo checkpoints found\n";
        return 1;
    }

    const int latest_height = heights.back();
    auto latest_result = chain_db.getUtreexoCheckpoint(latest_height);
    if (latest_result.status() != dinero::Status::Ok) {
        std::cerr << "Failed to read latest checkpoint at height " << latest_height << "\n";
        return 1;
    }
    const auto latest_bytes = latest_result.value();

    if (bitflip_latest) {
        if (latest_bytes.empty()) {
            std::cerr << "Latest checkpoint is empty; cannot bitflip\n";
            return 1;
        }

        auto mutated_bytes = latest_bytes;
        mutated_bytes.back() ^= 0x01;

        const auto token = dinero::ChainWriteToken::CreateForTesting();
        auto put_status = chain_db.putUtreexoCheckpointWithChecksum(token, latest_height, mutated_bytes);
        if (put_status != dinero::Status::Ok) {
            std::cerr << "Failed to overwrite latest checkpoint at height " << latest_height << "\n";
            return 1;
        }

        auto verify_result = chain_db.getUtreexoCheckpoint(latest_height);
        if (verify_result.status() != dinero::Status::Ok || verify_result.value() != mutated_bytes) {
            std::cerr << "Verification failed after bitflipping latest checkpoint\n";
            return 1;
        }

        std::cout << "mode=bitflipped-latest-checkpoint\n";
        std::cout << "chaindb=" << chain_db_dir << "\n";
        std::cout << "latest_height=" << latest_height << "\n";
        std::cout << "latest_bytes=" << latest_bytes.size() << "\n";
        std::cout << "mutated_bytes=" << mutated_bytes.size() << "\n";
        return 0;
    }

    if (heights.size() < 2) {
        std::cerr << "Need at least 2 checkpoints to synthesize stale state\n";
        return 1;
    }

    int source_height = -1;
    std::vector<uint8_t> source_bytes;
    int selected_index = static_cast<int>(heights.size()) - 1 - source_offset_back;
    for (int idx = selected_index; idx >= 0; --idx) {
        auto source_result = chain_db.getUtreexoCheckpoint(heights[idx]);
        if (source_result.status() != dinero::Status::Ok) {
            continue;
        }
        if (source_result.value() != latest_bytes) {
            source_height = heights[idx];
            source_bytes = source_result.value();
            break;
        }
    }

    if (source_height < 0) {
        std::cerr << "Could not find a distinct older checkpoint to reuse\n";
        return 1;
    }

    const auto token = dinero::ChainWriteToken::CreateForTesting();
    auto put_status = chain_db.putUtreexoCheckpointWithChecksum(token, latest_height, source_bytes);
    if (put_status != dinero::Status::Ok) {
        std::cerr << "Failed to overwrite latest checkpoint at height " << latest_height << "\n";
        return 1;
    }

    auto verify_result = chain_db.getUtreexoCheckpoint(latest_height);
    if (verify_result.status() != dinero::Status::Ok || verify_result.value() != source_bytes) {
        std::cerr << "Verification failed after overwriting latest checkpoint\n";
        return 1;
    }

    std::cout << "mode=stale-latest-checkpoint\n";
    std::cout << "chaindb=" << chain_db_dir << "\n";
    std::cout << "latest_height=" << latest_height << "\n";
    std::cout << "source_height=" << source_height << "\n";
    std::cout << "latest_bytes=" << latest_bytes.size() << "\n";
    std::cout << "source_bytes=" << source_bytes.size() << "\n";
    return 0;
}

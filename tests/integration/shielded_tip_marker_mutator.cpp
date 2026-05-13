#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void Usage(const char* argv0) {
    std::cerr << "Usage:\n"
              << "  " << argv0 << " --datadir <path> --delete\n"
              << "  " << argv0 << " --datadir <path> --retarget-height <height>\n";
    std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
    std::string datadir;
    bool delete_marker = false;
    bool retarget = false;
    int retarget_height = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--datadir") {
            if (i + 1 >= argc) Usage(argv[0]);
            datadir = argv[++i];
        } else if (arg == "--delete") {
            delete_marker = true;
        } else if (arg == "--retarget-height") {
            if (i + 1 >= argc) Usage(argv[0]);
            retarget = true;
            retarget_height = std::stoi(argv[++i]);
        } else {
            Usage(argv[0]);
        }
    }

    if (datadir.empty()) {
        Usage(argv[0]);
    }
    if (delete_marker == retarget) {
        Usage(argv[0]);
    }

    dinero::ChainDB chain_db;
    const auto db_status = chain_db.init(fs::path(datadir) / "blockchain" / "chaindb");
    if (db_status != dinero::Status::Ok) {
        std::cerr << "failed to open ChainDB: " << dinero::StatusToString(db_status) << "\n";
        return 1;
    }

    dinero::ChainWriteToken token = dinero::ChainWriteToken::CreateForTesting();

    if (delete_marker) {
        const auto status = chain_db.deleteShieldedTipMarker(token);
        if (status != dinero::Status::Ok) {
            std::cerr << "failed to delete ShieldedTipMarker: "
                      << dinero::StatusToString(status) << "\n";
            return 1;
        }
        std::cout << "deleted ShieldedTipMarker\n";
        return 0;
    }

    auto marker_result = chain_db.getShieldedTipMarker();
    if (marker_result.status() != dinero::Status::Ok) {
        std::cerr << "failed to read ShieldedTipMarker: "
                  << dinero::StatusToString(marker_result.status()) << "\n";
        return 1;
    }

    auto hash_result = chain_db.getBlockHashByHeight(retarget_height);
    if (hash_result.status() != dinero::Status::Ok) {
        std::cerr << "failed to load block hash at height " << retarget_height << ": "
                  << dinero::StatusToString(hash_result.status()) << "\n";
        return 1;
    }

    auto marker = marker_result.value();
    marker.height = retarget_height;
    marker.block_hash = hash_result.value();
    const auto status = chain_db.putShieldedTipMarker(token, marker);
    if (status != dinero::Status::Ok) {
        std::cerr << "failed to retarget ShieldedTipMarker: "
                  << dinero::StatusToString(status) << "\n";
        return 1;
    }

    std::cout << "retargeted ShieldedTipMarker to height " << retarget_height
              << " hash=" << marker.block_hash.GetHex() << "\n";
    return 0;
}

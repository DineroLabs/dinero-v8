#pragma once

#include "common/status.h"
#include "primitives/uint256.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dinero {
class ChainDB;
class BlockStorage;
class ChainWriteToken;
}

namespace dinero::daemon {

enum class HeaderMetadataRecoveryStatus {
    AlreadyOk,
    Recoverable,
    Recovered,
    MissingHeightIndex,
    MissingFlatfileBlock,
    InvalidFlatfileBlock,
    MissingChainwork,
    WriteFailed
};

const char* HeaderMetadataRecoveryStatusName(HeaderMetadataRecoveryStatus status);

struct HeaderMetadataRecoveryEntry {
    uint32_t height{0};
    uint256 block_hash;
    HeaderMetadataRecoveryStatus status{HeaderMetadataRecoveryStatus::AlreadyOk};
    std::string reason;
};

struct HeaderMetadataRecoveryManifest {
    uint32_t window_start{0};
    uint32_t window_end{0};
    bool write{false};
    uint64_t scanned{0};
    uint64_t already_ok{0};
    uint64_t recoverable{0};
    uint64_t recovered{0};
    uint64_t failed{0};
    std::string final_status;
    std::vector<HeaderMetadataRecoveryEntry> entries;

    std::string ToJson() const;
};

struct HeaderMetadataRecoveryOptions {
    std::filesystem::path datadir;
    uint32_t window_start{0};
    uint32_t window_end{0};
    bool write{false};
    std::filesystem::path manifest_path_override;
    ChainDB* live_chain_db{nullptr};
    BlockStorage* live_block_storage{nullptr};
    const ChainWriteToken* write_token{nullptr};
};

StatusOr<HeaderMetadataRecoveryManifest>
RecoverMissingHeaderMetadataRange(const HeaderMetadataRecoveryOptions& opts);

}  // namespace dinero::daemon

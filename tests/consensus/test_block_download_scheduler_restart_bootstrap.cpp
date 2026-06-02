/**
 * Restart bootstrap regression for BlockDownloadScheduler.
 *
 * Proves that persisted headers present in HeaderChainSelector are enough to
 * restart stateless block fetching after app restart, even before any fresh
 * headers message triggers OnHeadersProcessed().
 */

#include "consensus/block_download_scheduler.h"
#include "consensus/block_lifecycle.h"
#include "consensus/chainparams.h"
#include "consensus/header_chain.h"
#include "daemon/services/chainstate_restart_import.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using dinero::BlockHeader;
using dinero::uint256;
namespace dcs = dinero::consensus;

namespace {

BlockHeader CreateTestHeader(
    const uint256& prev_hash,
    uint32_t time,
    uint32_t bits = 0x1d00ffff
) {
    BlockHeader header;
    header.version = 1;
    header.prev_block_hash = prev_hash;
    header.merkle_root = uint256();
    header.timestamp = time;
    header.difficulty = bits;
    header.nonce = 1;
    header.utreexo_root = uint256();
    return header;
}

void BuildLinearHeaders(
    dcs::HeaderChainSelector& selector,
    uint32_t count,
    std::vector<uint256>* hashes = nullptr,
    uint32_t base_time = 1'000'000
) {
    uint256 null_hash;
    null_hash.SetNull();

    BlockHeader genesis = CreateTestHeader(null_hash, base_time);
    if (!selector.AddHeader(genesis)) {
        throw std::runtime_error("failed to add genesis header");
    }

    if (hashes) {
        hashes->clear();
        hashes->push_back(genesis.GetHash());
    }

    uint256 prev = genesis.GetHash();
    for (uint32_t i = 1; i <= count; ++i) {
        BlockHeader next = CreateTestHeader(prev, base_time + i);
        if (!selector.AddHeader(next)) {
            throw std::runtime_error("failed to add linear header at height " + std::to_string(i));
        }
        prev = next.GetHash();
        if (hashes) {
            hashes->push_back(prev);
        }
    }
}

bool Require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "   ❌ " << message << std::endl;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    // RestorePersistedBlockIndexMetadata + chain-state interactions in the
    // second scenario reach into code paths that consult dinero::GetActiveChain(),
    // which throws if SelectParams() has never been called. Wire up chain
    // selection here so the test exercises the rehydration path with a
    // consistent chain answer. Use regtest: the synthetic headers carry fake PoW
    // (arbitrary bits, nonce=1) and header validation now enforces PoW on
    // mainnet/testnet — regtest skips PoW (matching block_acceptor), and this
    // test only needs *some* chain selected, not mainnet specifically.
    dinero::SelectParams(dinero::Chain::REGTEST);

    std::cout << "=== BlockDownloadScheduler Restart Bootstrap Regression ===" << std::endl;

    dcs::HeaderChainSelector selector;
    std::vector<uint256> hashes;
    try {
        BuildLinearHeaders(selector, 6, &hashes, 7'000'000);
    } catch (const std::exception& e) {
        std::cerr << "   ❌ failed to build persisted header chain: " << e.what() << std::endl;
        return 1;
    }

    const auto* best_header = selector.GetBestHeader();
    if (!Require(best_header != nullptr, "expected persisted best header")) {
        return 1;
    }
    if (!Require(best_header->height == 6, "expected best header height 6")) {
        return 1;
    }

    dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
    scheduler.SetLocalTipHeight(3);

    std::vector<uint256> requested_hashes;
    scheduler.SetSendGetDataCallback([&requested_hashes](const uint256& block_hash, uint32_t /*height*/) {
        requested_hashes.push_back(block_hash);
    });

    if (!Require(!scheduler.HeadersSynced(), "scheduler should start cold before bootstrap tick")) {
        return 1;
    }

    // Simulate app restart with persisted headers already loaded: no fresh
    // OnHeadersProcessed() call yet, only the periodic Tick().
    scheduler.Tick();

    if (!Require(scheduler.HeadersSynced(), "bootstrap tick should mark headers as processed")) {
        return 1;
    }
    if (!Require(!requested_hashes.empty(), "bootstrap tick should request at least one block")) {
        return 1;
    }
    if (!Require(scheduler.GetInFlightCount() == requested_hashes.size(),
                 "bootstrap tick should track every requested block as in-flight")) {
        return 1;
    }
    if (!Require(requested_hashes.front() == hashes[4], "bootstrap tick should request height 4 first")) {
        return 1;
    }

    std::cout << "   ✅ requested height 4 hash "
              << requested_hashes.front().GetHex().substr(0, 16)
              << "... after restart bootstrap (window=" << requested_hashes.size() << ")" << std::endl;

    std::cout << "\n=== Persisted Branch Metadata Restart Regression ===" << std::endl;

    const auto temp_db_path =
        std::filesystem::temp_directory_path() / "dinero_restart_branch_metadata_test";
    std::filesystem::remove_all(temp_db_path);

    dinero::ChainDB chain_db;
    if (!Require(chain_db.init(temp_db_path) == dinero::Status::Ok,
                 "expected ChainDB init for metadata regression")) {
        return 1;
    }

    const dinero::ChainWriteToken token = dinero::ChainWriteToken::CreateForTesting();
    BlockHeader stored_branch_header = CreateTestHeader(hashes[3], 7'000'100);
    const uint256 stored_branch_hash = stored_branch_header.GetHash();

    dinero::ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = hashes[3];
    metadata.height = 4;
    metadata.status_flags = dinero::BLOCK_VALID_CHAIN | dinero::BLOCK_HAVE_DATA;
    metadata.file_number = 1;
    metadata.data_pos = 128;
    metadata.data_size = 256;
    metadata.undo_file = 1;
    metadata.undo_pos = 512;
    metadata.undo_size = 64;

    if (!Require(chain_db.putHeaderMetadata(token, stored_branch_hash, metadata) == dinero::Status::Ok,
                 "expected persisted metadata write for stored better block")) {
        return 1;
    }

    dinero::CBlockIndex recreated_from_header(stored_branch_header, 4);
    recreated_from_header.status = dinero::BLOCK_VALID_HEADER;

    if (!Require(!dinero::IsEligibleForCandidacy(recreated_from_header.status),
                 "header-only recreated block should not be candidate-eligible before restore")) {
        return 1;
    }

    if (!Require(RestorePersistedBlockIndexMetadata(chain_db, stored_branch_hash, &recreated_from_header),
                 "restart import should rehydrate persisted metadata for stored branch block")) {
        return 1;
    }

    if (!Require((recreated_from_header.status & dinero::BLOCK_VALID_CHAIN) != 0,
                 "rehydrated block should recover BLOCK_VALID_CHAIN")) {
        return 1;
    }
    if (!Require((recreated_from_header.status & dinero::BLOCK_HAVE_DATA) != 0,
                 "rehydrated block should recover BLOCK_HAVE_DATA")) {
        return 1;
    }
    if (!Require(dinero::IsEligibleForCandidacy(recreated_from_header.status),
                 "rehydrated stored branch block should be candidate-eligible after restart")) {
        return 1;
    }
    if (!Require(recreated_from_header.data_size == metadata.data_size,
                 "rehydrated block should recover persisted disk positions")) {
        return 1;
    }

    // Close the DB before removing the directory. On Windows, RocksDB's
    // LOCK file is opened with non-shared write access and DeleteFile
    // fails with "file is being used by another process" if anything is
    // still holding it -- which raises std::filesystem::filesystem_error
    // out of remove_all and (uncaught) abort()s the test. Linux's
    // unlink-while-open silently succeeds, hiding the issue there.
    chain_db.close();
    std::error_code remove_ec;
    std::filesystem::remove_all(temp_db_path, remove_ec);
    if (remove_ec) {
        std::cerr << "   ⚠️  cleanup remove_all reported: " << remove_ec.message() << std::endl;
    }
    std::cout << "   ✅ restored persisted validity/data flags for stored better block after restart" << std::endl;
    return 0;
}

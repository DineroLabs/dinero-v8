/**
 * @file test_archival_block_reader.cpp
 * @brief Verifies archival reader distinguishes flatfile coverage from legacy ChainDB fallback
 */

#include "storage/archival_block_reader.h"
#include "storage/chain_write_token.h"
#include "consensus/block_lifecycle.h"
#include "daemon/genesis_init.hpp"
#include "consensus/chainparams.h"
#include "consensus/reindexer.h"
#include "primitives/block.h"
#include "primitives/amount.h"
#include <cstring>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace dinero;

#define TEST(name) void test_##name()

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "\nASSERT_TRUE failed: " #cond << std::endl; \
            std::abort(); \
        } \
    } while (0)

namespace {

Transaction MakeCoinbaseTx() {
    Transaction tx;
    tx.version = 2;

    TxInput in;
    in.prevout.txid = TxId(uint256());
    in.prevout.vout = 0xffffffff;
    in.scriptSig = {0x03, 0x01, 0x00, 0x00};
    in.sequence = 0xffffffff;
    tx.vin.push_back(in);

    TxOutput out;
    out.value = AmountUna::Una(50);
    out.scriptPubKey = {0x00, 0x14};
    out.scriptPubKey.insert(out.scriptPubKey.end(), 20, 0x11);
    tx.vout.push_back(out);

    tx.DetectWitnessVersion();
    return tx;
}

Block MakeBlock(uint32_t nonce) {
    Transaction coinbase = MakeCoinbaseTx();

    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.merkle_root = coinbase.GetTxid().AsUint256();
    block.header.utreexo_root = uint256();
    block.header.timestamp = 123456789 + nonce;
    block.header.difficulty = 0x207fffff;
    block.header.nonce = nonce;
    block.header.ZeroReserved();
    block.vtx.push_back(coinbase);
    block.utreexo.reset();
    return block;
}

Block MakeBlockWithPrev(const uint256& prev_hash, uint32_t nonce, uint32_t timestamp) {
    Block block = MakeBlock(nonce);
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = timestamp;
    block.header.difficulty = Params().genesis.nBits;
    return block;
}

std::filesystem::path MakeTempRoot() {
    auto root = std::filesystem::temp_directory_path() /
                ("din_archival_reader_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void PersistFlatfileIndexedBlock(ChainDB& chain_db,
                                 const ChainWriteToken& token,
                                 const Block& block,
                                 int32_t height,
                                 const arith_uint256& chainwork,
                                 const FilePosition& pos) {
    rocksdb::WriteBatch batch;
    const uint256 hash = block.GetHash();

    ASSERT_TRUE(chain_db.putHeader(token, hash, block.header, height, chainwork, &batch) == Status::Ok);

    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = block.header.prev_block_hash;
    metadata.height = height;
    metadata.chainwork = chainwork;
    metadata.status_flags = BLOCK_VALID_HEADER | BLOCK_HAVE_DATA;
    metadata.file_number = pos.file_number;
    metadata.data_pos = static_cast<uint32_t>(pos.offset);
    metadata.data_size = pos.size;
    ASSERT_TRUE(chain_db.putHeaderMetadata(token, hash, metadata, &batch) == Status::Ok);
    ASSERT_TRUE(chain_db.putHeightIndex(token, height, hash, &batch) == Status::Ok);
    ASSERT_TRUE(chain_db.setTip(token, hash, height, chainwork, &batch) == Status::Ok);
    ASSERT_TRUE(chain_db.writeBatch(token, std::move(batch), true) == Status::Ok);
}

}  // namespace

TEST(ArchivalBlockReader_FlatfileAndFallbackModes) {
    const auto root = MakeTempRoot();
    const auto chain_db_dir = root / "chaindb";
    std::filesystem::create_directories(chain_db_dir);

    ChainDB chain_db;
    ASSERT_TRUE(chain_db.init(chain_db_dir) == Status::Ok);

    BlockStorage block_storage;
    ASSERT_TRUE(block_storage.init(root) == Status::Ok);

    const ChainWriteToken token = ChainWriteToken::CreateForTesting();

    // Block 1: present in both flatfiles and legacy ChainDB shadow storage.
    const Block flatfile_block = MakeBlock(1);
    const uint256 flatfile_hash = flatfile_block.GetHash();
    auto flatfile_pos_result = block_storage.writeBlock(flatfile_hash, flatfile_block);
    ASSERT_TRUE(flatfile_pos_result.status() == Status::Ok);
    ASSERT_TRUE(chain_db.putBlock(token, flatfile_hash, flatfile_block) == Status::Ok);

    ChainDB::PersistedHeaderMetadata flatfile_metadata;
    flatfile_metadata.height = 1;
    flatfile_metadata.file_number = flatfile_pos_result.value().file_number;
    flatfile_metadata.data_pos = flatfile_pos_result.value().offset;
    flatfile_metadata.data_size = flatfile_pos_result.value().size;
    ASSERT_TRUE(chain_db.putHeaderMetadata(token, flatfile_hash, flatfile_metadata) == Status::Ok);

    auto flatfile_result = storage::ReadArchivalBlockDetailed(
        chain_db,
        &block_storage,
        flatfile_hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    ASSERT_TRUE(flatfile_result.result.status() == Status::Ok);
    ASSERT_TRUE(flatfile_result.source == storage::ArchivalReadSource::Flatfile);
    ASSERT_TRUE(flatfile_result.result.value().GetHash() == flatfile_hash);

    // Block 2: only present in legacy ChainDB shadow storage, no flatfile metadata.
    const Block legacy_only_block = MakeBlock(2);
    const uint256 legacy_only_hash = legacy_only_block.GetHash();
    ASSERT_TRUE(chain_db.putBlock(token, legacy_only_hash, legacy_only_block) == Status::Ok);

    auto fallback_result = storage::ReadArchivalBlockDetailed(
        chain_db,
        &block_storage,
        legacy_only_hash,
        storage::ArchivalReadMode::AllowLegacyFallback);
    ASSERT_TRUE(fallback_result.result.status() == Status::Ok);
    ASSERT_TRUE(fallback_result.source == storage::ArchivalReadSource::LegacyChainDB);
    ASSERT_TRUE(storage::HasArchivalBlockBody(
        chain_db,
        &block_storage,
        legacy_only_hash,
        storage::ArchivalReadMode::AllowLegacyFallback));

    auto strict_result = storage::ReadArchivalBlockDetailed(
        chain_db,
        &block_storage,
        legacy_only_hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    ASSERT_TRUE(strict_result.result.status() == Status::NotFound);
    ASSERT_TRUE(strict_result.source == storage::ArchivalReadSource::None);
    ASSERT_TRUE(!storage::HasArchivalBlockBody(
        chain_db,
        &block_storage,
        legacy_only_hash,
        storage::ArchivalReadMode::RequireFlatfiles));

    block_storage.close();
    chain_db.close();
    std::filesystem::remove_all(root);
}

TEST(ArchivalUndoReader_FlatfileAndFallbackModes) {
    const auto root = MakeTempRoot();
    const auto chain_db_dir = root / "chaindb";
    std::filesystem::create_directories(chain_db_dir);

    ChainDB chain_db;
    ASSERT_TRUE(chain_db.init(chain_db_dir) == Status::Ok);

    BlockStorage block_storage;
    ASSERT_TRUE(block_storage.init(root) == Status::Ok);

    const ChainWriteToken token = ChainWriteToken::CreateForTesting();

    const Block block = MakeBlock(3);
    const uint256 block_hash = block.GetHash();

    UndoRecord flatfile_undo;
    flatfile_undo.created.emplace_back(block.vtx[0].GetTxid().AsUint256(), 0);

    auto undo_pos_result = block_storage.writeUndo(block_hash, flatfile_undo.Serialize());
    ASSERT_TRUE(undo_pos_result.status() == Status::Ok);
    ASSERT_TRUE(chain_db.putUndo(token, block_hash, flatfile_undo) == Status::Ok);

    ChainDB::PersistedHeaderMetadata undo_metadata;
    undo_metadata.height = 3;
    undo_metadata.undo_file = undo_pos_result.value().file_number;
    undo_metadata.undo_pos = undo_pos_result.value().offset;
    undo_metadata.undo_size = undo_pos_result.value().size;
    ASSERT_TRUE(chain_db.putHeaderMetadata(token, block_hash, undo_metadata) == Status::Ok);

    auto flatfile_undo_result = storage::ReadArchivalUndoDetailed(
        chain_db,
        &block_storage,
        block_hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    ASSERT_TRUE(flatfile_undo_result.result.status() == Status::Ok);
    ASSERT_TRUE(flatfile_undo_result.source == storage::ArchivalReadSource::Flatfile);
    ASSERT_TRUE(flatfile_undo_result.result.value().created.size() == 1);

    const Block legacy_block = MakeBlock(4);
    const uint256 legacy_hash = legacy_block.GetHash();
    UndoRecord legacy_undo;
    legacy_undo.created.emplace_back(legacy_block.vtx[0].GetTxid().AsUint256(), 0);
    ASSERT_TRUE(chain_db.putUndo(token, legacy_hash, legacy_undo) == Status::Ok);

    auto fallback_undo_result = storage::ReadArchivalUndoDetailed(
        chain_db,
        &block_storage,
        legacy_hash,
        storage::ArchivalReadMode::AllowLegacyFallback);
    ASSERT_TRUE(fallback_undo_result.result.status() == Status::Ok);
    ASSERT_TRUE(fallback_undo_result.source == storage::ArchivalReadSource::LegacyChainDB);

    auto strict_undo_result = storage::ReadArchivalUndoDetailed(
        chain_db,
        &block_storage,
        legacy_hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    ASSERT_TRUE(strict_undo_result.result.status() == Status::NotFound);
    ASSERT_TRUE(strict_undo_result.source == storage::ArchivalReadSource::None);

    block_storage.close();
    chain_db.close();
    std::filesystem::remove_all(root);
}

TEST(GenesisInitialization_WritesFlatfileBackedGenesis) {
    const auto root = MakeTempRoot();
    const auto chain_db_dir = root / "chaindb";
    std::filesystem::create_directories(chain_db_dir);

    ChainDB chain_db;
    ASSERT_TRUE(chain_db.init(chain_db_dir) == Status::Ok);

    BlockStorage block_storage;
    ASSERT_TRUE(block_storage.init(root) == Status::Ok);

    ASSERT_TRUE(InitializeGenesis(&chain_db, &block_storage, nullptr));

    const uint256 genesis_hash = uint256::FromHexUnsafe(Params().genesis_hash);
    auto metadata_result = chain_db.getHeaderMetadata(genesis_hash);
    ASSERT_TRUE(metadata_result.status() == Status::Ok);
    ASSERT_TRUE(metadata_result.value().data_size > 0);
    ASSERT_TRUE(chain_db.getBlock(genesis_hash).status() == Status::NotFound);

    auto genesis_result = storage::ReadArchivalBlockDetailed(
        chain_db,
        &block_storage,
        genesis_hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    ASSERT_TRUE(genesis_result.result.status() == Status::Ok);
    ASSERT_TRUE(genesis_result.source == storage::ArchivalReadSource::Flatfile);
    ASSERT_TRUE(genesis_result.result.value().GetHash() == genesis_hash);

    block_storage.close();
    chain_db.close();
    std::filesystem::remove_all(root);
}

TEST(Reindexer_UsesFlatfilesWithoutShadowBodyWrites) {
    const auto root = MakeTempRoot();
    const auto source_chain_db_dir = root / "source-chaindb";
    const auto target_chain_db_dir = root / "target-chaindb";
    std::filesystem::create_directories(source_chain_db_dir);
    std::filesystem::create_directories(target_chain_db_dir);

    ChainDB source_chain_db;
    ASSERT_TRUE(source_chain_db.init(source_chain_db_dir) == Status::Ok);

    BlockStorage source_block_storage;
    ASSERT_TRUE(source_block_storage.init(root) == Status::Ok);
    ASSERT_TRUE(InitializeGenesis(&source_chain_db, &source_block_storage, nullptr));

    const uint256 genesis_hash = uint256::FromHexUnsafe(Params().genesis_hash);
    const Block block1 = MakeBlockWithPrev(genesis_hash, 42, Params().genesis.nTime + 1);
    const uint256 block1_hash = block1.GetHash();
    auto block1_pos = source_block_storage.writeBlock(block1_hash, block1);
    ASSERT_TRUE(block1_pos.status() == Status::Ok);

    const auto blk_path = root / "blocks" / "blk00000.dat";
    const auto file_size_before = std::filesystem::file_size(blk_path);

    ChainDB target_chain_db;
    ASSERT_TRUE(target_chain_db.init(target_chain_db_dir) == Status::Ok);

    BlockStorage reindex_block_storage;
    ASSERT_TRUE(reindex_block_storage.init(root) == Status::Ok);

    dinero::consensus::BlockReindexer::Config config;
    config.mode = dinero::consensus::BlockReindexer::Mode::FULL;
    config.use_assumevalid = true;
    config.progress_interval = 1000;

    dinero::consensus::BlockReindexer reindexer(root, &target_chain_db, &reindex_block_storage, config);
    auto result = reindexer.execute();
    ASSERT_TRUE(result.status() == Status::Ok);
    ASSERT_TRUE(result.value().success);

    const auto file_size_after = std::filesystem::file_size(blk_path);
    ASSERT_TRUE(file_size_after == file_size_before);

    auto tip_result = target_chain_db.getTip();
    ASSERT_TRUE(tip_result.status() == Status::Ok);
    ASSERT_TRUE(tip_result.value().height == 1);
    ASSERT_TRUE(tip_result.value().hash == block1_hash);

    auto genesis_metadata = target_chain_db.getHeaderMetadata(genesis_hash);
    ASSERT_TRUE(genesis_metadata.status() == Status::Ok);
    ASSERT_TRUE(genesis_metadata.value().data_size > 0);

    auto block1_metadata = target_chain_db.getHeaderMetadata(block1_hash);
    ASSERT_TRUE(block1_metadata.status() == Status::Ok);
    ASSERT_TRUE(block1_metadata.value().data_size > 0);

    ASSERT_TRUE(target_chain_db.getBlock(genesis_hash).status() == Status::NotFound);
    ASSERT_TRUE(target_chain_db.getBlock(block1_hash).status() == Status::NotFound);

    auto strict_genesis = storage::ReadArchivalBlockDetailed(
        target_chain_db,
        &reindex_block_storage,
        genesis_hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    ASSERT_TRUE(strict_genesis.result.status() == Status::Ok);
    ASSERT_TRUE(strict_genesis.source == storage::ArchivalReadSource::Flatfile);

    auto strict_block1 = storage::ReadArchivalBlockDetailed(
        target_chain_db,
        &reindex_block_storage,
        block1_hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    ASSERT_TRUE(strict_block1.result.status() == Status::Ok);
    ASSERT_TRUE(strict_block1.source == storage::ArchivalReadSource::Flatfile);

    reindex_block_storage.close();
    target_chain_db.close();
    source_block_storage.close();
    source_chain_db.close();
    std::filesystem::remove_all(root);
}

TEST(StrictFlatfileCoverage_PassesForGenesisAndTip) {
    const auto root = MakeTempRoot();
    const auto chain_db_dir = root / "chaindb";
    std::filesystem::create_directories(chain_db_dir);

    ChainDB chain_db;
    ASSERT_TRUE(chain_db.init(chain_db_dir) == Status::Ok);

    BlockStorage block_storage;
    ASSERT_TRUE(block_storage.init(root) == Status::Ok);
    ASSERT_TRUE(InitializeGenesis(&chain_db, &block_storage, nullptr));

    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 genesis_hash = uint256::FromHexUnsafe(Params().genesis_hash);
    const Block block1 = MakeBlockWithPrev(genesis_hash, 55, Params().genesis.nTime + 1);
    const uint256 block1_hash = block1.GetHash();
    const auto block1_pos = block_storage.writeBlock(block1_hash, block1);
    ASSERT_TRUE(block1_pos.status() == Status::Ok);

    PersistFlatfileIndexedBlock(chain_db, token, block1, 1, arith_uint256(1), block1_pos.value());

    const auto audit = storage::VerifyStrictFlatfileCoverage(chain_db, &block_storage, 1);
    ASSERT_TRUE(audit.ok);
    ASSERT_TRUE(audit.expected_body_count == 2);
    ASSERT_TRUE(audit.verified_body_count == 2);
    ASSERT_TRUE(audit.first_missing_height < 0);

    block_storage.close();
    chain_db.close();
    std::filesystem::remove_all(root);
}

TEST(StrictFlatfileCoverage_FailsWhenMetadataPointsToMissingBody) {
    const auto root = MakeTempRoot();
    const auto chain_db_dir = root / "chaindb";
    std::filesystem::create_directories(chain_db_dir);

    ChainDB chain_db;
    ASSERT_TRUE(chain_db.init(chain_db_dir) == Status::Ok);

    BlockStorage block_storage;
    ASSERT_TRUE(block_storage.init(root) == Status::Ok);
    ASSERT_TRUE(InitializeGenesis(&chain_db, &block_storage, nullptr));

    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 genesis_hash = uint256::FromHexUnsafe(Params().genesis_hash);
    const Block block1 = MakeBlockWithPrev(genesis_hash, 77, Params().genesis.nTime + 1);
    const uint256 block1_hash = block1.GetHash();

    rocksdb::WriteBatch batch;
    ASSERT_TRUE(chain_db.putHeader(token, block1_hash, block1.header, 1, arith_uint256(1), &batch) == Status::Ok);
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = genesis_hash;
    metadata.height = 1;
    metadata.chainwork = arith_uint256(1);
    metadata.status_flags = BLOCK_VALID_HEADER | BLOCK_HAVE_DATA;
    metadata.file_number = 99;
    metadata.data_pos = 0;
    metadata.data_size = 128;
    ASSERT_TRUE(chain_db.putHeaderMetadata(token, block1_hash, metadata, &batch) == Status::Ok);
    ASSERT_TRUE(chain_db.putHeightIndex(token, 1, block1_hash, &batch) == Status::Ok);
    ASSERT_TRUE(chain_db.setTip(token, block1_hash, 1, arith_uint256(1), &batch) == Status::Ok);
    ASSERT_TRUE(chain_db.writeBatch(token, std::move(batch), true) == Status::Ok);

    const auto audit = storage::VerifyStrictFlatfileCoverage(chain_db, &block_storage, 1);
    ASSERT_TRUE(!audit.ok);
    ASSERT_TRUE(audit.first_missing_height == 1);
    ASSERT_TRUE(audit.first_missing_hash == block1_hash);
    ASSERT_TRUE(audit.verified_body_count == 1);

    block_storage.close();
    chain_db.close();
    std::filesystem::remove_all(root);
}

TEST(ArchivalReader_RejectsMismatchedFlatfileBodyHash) {
    const auto root = MakeTempRoot();
    const auto chain_db_dir = root / "chaindb";
    std::filesystem::create_directories(chain_db_dir);

    ChainDB chain_db;
    ASSERT_TRUE(chain_db.init(chain_db_dir) == Status::Ok);

    BlockStorage block_storage;
    ASSERT_TRUE(block_storage.init(root) == Status::Ok);

    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const Block stored_block = MakeBlock(11);
    const Block indexed_block = MakeBlock(12);
    const uint256 stored_hash = stored_block.GetHash();
    const uint256 indexed_hash = indexed_block.GetHash();

    auto stored_pos = block_storage.writeBlock(stored_hash, stored_block);
    ASSERT_TRUE(stored_pos.status() == Status::Ok);

    PersistFlatfileIndexedBlock(chain_db, token, indexed_block, 1, arith_uint256(1), stored_pos.value());

    auto strict_result = storage::ReadArchivalBlockDetailed(
        chain_db,
        &block_storage,
        indexed_hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    ASSERT_TRUE(strict_result.result.status() == Status::Corruption);
    ASSERT_TRUE(strict_result.source == storage::ArchivalReadSource::None);

    block_storage.close();
    chain_db.close();
    std::filesystem::remove_all(root);
}

int main() {
    SelectParams(Chain::MAINNET);
    std::cout << "\n===========================================================\n";
    std::cout << "ARCHIVAL BLOCK READER TESTS\n";
    std::cout << "===========================================================\n\n";
    std::cout << "Running: ArchivalBlockReader_FlatfileAndFallbackModes..." << std::flush;
    test_ArchivalBlockReader_FlatfileAndFallbackModes();
    std::cout << " OK" << std::endl;
    std::cout << "Running: ArchivalUndoReader_FlatfileAndFallbackModes..." << std::flush;
    test_ArchivalUndoReader_FlatfileAndFallbackModes();
    std::cout << " OK" << std::endl;
    std::cout << "Running: GenesisInitialization_WritesFlatfileBackedGenesis..." << std::flush;
    test_GenesisInitialization_WritesFlatfileBackedGenesis();
    std::cout << " OK" << std::endl;
    std::cout << "Running: Reindexer_UsesFlatfilesWithoutShadowBodyWrites..." << std::flush;
    test_Reindexer_UsesFlatfilesWithoutShadowBodyWrites();
    std::cout << " OK" << std::endl;
    std::cout << "Running: StrictFlatfileCoverage_PassesForGenesisAndTip..." << std::flush;
    test_StrictFlatfileCoverage_PassesForGenesisAndTip();
    std::cout << " OK" << std::endl;
    std::cout << "Running: StrictFlatfileCoverage_FailsWhenMetadataPointsToMissingBody..." << std::flush;
    test_StrictFlatfileCoverage_FailsWhenMetadataPointsToMissingBody();
    std::cout << " OK" << std::endl;
    std::cout << "Running: ArchivalReader_RejectsMismatchedFlatfileBodyHash..." << std::flush;
    test_ArchivalReader_RejectsMismatchedFlatfileBodyHash();
    std::cout << " OK" << std::endl;
    std::cout << "\nALL ARCHIVAL BLOCK READER TESTS PASSED!" << std::endl;
    std::cout << "===========================================================\n" << std::endl;
    return 0;
}

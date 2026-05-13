/**
 * Regression guard for May 2026 missing header-metadata incidents.
 *
 * Reproduces the Dell/LA shape: canonical height index and blk*.dat block
 * bodies exist, but per-hash PersistedHeaderMetadata rows are absent. The
 * recovery pass must reconstruct only those metadata rows from already-durable
 * canonical data, without inventing undo metadata.
 */

#include "daemon/header_metadata_recovery.h"
#include "daemon/genesis_init.hpp"
#include "consensus/block_lifecycle.h"
#include "consensus/chainparams.h"
#include "consensus/chainwork.h"
#include "primitives/amount.h"
#include "primitives/block.h"
#include "storage/archival_block_reader.h"
#include "storage/block_storage.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <rocksdb/write_batch.h>

#include <cassert>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace dinero;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "[FAIL] " << msg << std::endl;
        std::abort();
    }
}

std::filesystem::path MakeTempRoot() {
    auto root = std::filesystem::temp_directory_path() /
                ("din_header_metadata_recovery_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

Transaction MakeCoinbaseTx(uint32_t n) {
    Transaction tx;
    tx.version = 2;

    TxInput in;
    in.prevout.txid = TxId(uint256());
    in.prevout.vout = 0xffffffff;
    in.scriptSig = {0x03, static_cast<uint8_t>(n & 0xff), 0x00, 0x00};
    in.sequence = 0xffffffff;
    tx.vin.push_back(in);

    TxOutput out;
    out.value = AmountUna::Una(50);
    out.scriptPubKey = {0x00, 0x14};
    out.scriptPubKey.insert(out.scriptPubKey.end(), 20, static_cast<uint8_t>(0x10 + n));
    tx.vout.push_back(out);

    tx.DetectWitnessVersion();
    return tx;
}

Block MakeBlockWithPrev(const uint256& prev_hash, uint32_t n) {
    Transaction coinbase = MakeCoinbaseTx(n);

    Block block;
    // Avoid ChainDB's legacy metadata fallback treating a version=1 header row
    // as old metadata; this fixture wants a true missing-metadata shape.
    block.header.version = 3;
    block.header.prev_block_hash = prev_hash;
    block.header.merkle_root = coinbase.GetTxid().AsUint256();
    block.header.utreexo_root = uint256();
    block.header.timestamp = Params().genesis.nTime + n;
    block.header.difficulty = Params().genesis.nBits;
    block.header.nonce = n;
    block.header.ZeroReserved();
    block.vtx.push_back(coinbase);
    block.utreexo.reset();
    return block;
}

void StageHeaderAndHeightOnly(ChainDB& db,
                              const ChainWriteToken& token,
                              const Block& block,
                              int height,
                              const arith_uint256& chainwork,
                              rocksdb::WriteBatch& batch) {
    const uint256 hash = block.GetHash();
    require(db.putHeader(token, hash, block.header, height, chainwork, &batch) == Status::Ok,
            "putHeader failed");
    require(db.putHeightIndex(token, height, hash, &batch) == Status::Ok,
            "putHeightIndex failed");
}

}  // namespace

int main() {
    SelectParams(Chain::REGTEST);

    const auto root = MakeTempRoot();
    const auto chain_db_dir = root / "chaindb";
    std::filesystem::create_directories(chain_db_dir);

    ChainDB db;
    require(db.init(chain_db_dir) == Status::Ok, "ChainDB init");

    BlockStorage block_storage;
    require(block_storage.init(root) == Status::Ok, "BlockStorage init");
    require(InitializeGenesis(&db, &block_storage, nullptr), "genesis init");

    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    const uint256 genesis_hash = uint256::FromHexUnsafe(Params().genesis_hash);
    auto genesis_work = db.getBlockWork(genesis_hash);
    require(genesis_work.status() == Status::Ok, "genesis work missing");

    const Block block1 = MakeBlockWithPrev(genesis_hash, 1);
    const uint256 hash1 = block1.GetHash();
    auto pos1 = block_storage.writeBlock(hash1, block1);
    require(pos1.status() == Status::Ok, "write block1");

    const Block block2 = MakeBlockWithPrev(hash1, 2);
    const uint256 hash2 = block2.GetHash();
    auto pos2 = block_storage.writeBlock(hash2, block2);
    require(pos2.status() == Status::Ok, "write block2");

    arith_uint256 work1 = genesis_work.value() + GetBlockProof(block1.header.difficulty);
    arith_uint256 work2 = work1 + GetBlockProof(block2.header.difficulty);

    rocksdb::WriteBatch batch;
    StageHeaderAndHeightOnly(db, token, block1, 1, work1, batch);
    StageHeaderAndHeightOnly(db, token, block2, 2, work2, batch);
    require(db.setTip(token, hash2, 2, work2, &batch) == Status::Ok, "setTip block2");
    require(db.writeBatch(token, std::move(batch), true) == Status::Ok, "commit header-only rows");

    require(db.getHeaderMetadata(hash1).status() == Status::NotFound,
            "test setup: block1 metadata should be missing");
    require(db.getHeaderMetadata(hash2).status() == Status::NotFound,
            "test setup: block2 metadata should be missing");

    daemon::HeaderMetadataRecoveryOptions dry_opts;
    dry_opts.datadir = root;
    dry_opts.window_start = 1;
    dry_opts.window_end = 2;
    dry_opts.write = false;
    dry_opts.live_chain_db = &db;
    dry_opts.live_block_storage = &block_storage;

    auto dry = daemon::RecoverMissingHeaderMetadataRange(dry_opts);
    require(dry.status() == Status::Ok, "dry recovery status");
    require(dry.value().final_status == "dry_run_complete", "dry final_status");
    require(dry.value().recoverable == 2, "dry recoverable count");
    require(db.getHeaderMetadata(hash1).status() == Status::NotFound,
            "dry run must not write block1 metadata");

    auto write_opts = dry_opts;
    write_opts.write = true;
    write_opts.write_token = &token;
    auto written = daemon::RecoverMissingHeaderMetadataRange(write_opts);
    require(written.status() == Status::Ok, "write recovery status");
    require(written.value().final_status == "ok", "write final_status");
    require(written.value().recovered == 2, "recovered count");

    auto meta1 = db.getHeaderMetadata(hash1);
    require(meta1.status() == Status::Ok, "block1 metadata recovered");
    require(meta1.value().data_pos == pos1.value().offset, "block1 data_pos recovered");
    require(meta1.value().data_size == pos1.value().size, "block1 data_size recovered");
    require((meta1.value().status_flags & BLOCK_HAVE_DATA) != 0, "HAVE_DATA set");
    require((meta1.value().status_flags & BLOCK_HAVE_UNDO) == 0, "HAVE_UNDO not fabricated");

    auto strict = storage::ReadArchivalBlockDetailed(
        db, &block_storage, hash1, storage::ArchivalReadMode::RequireFlatfiles);
    require(strict.result.status() == Status::Ok, "strict archival read after recovery");
    require(strict.result.value().GetHash() == hash1, "strict archival read hash");

    auto second = daemon::RecoverMissingHeaderMetadataRange(dry_opts);
    require(second.status() == Status::Ok, "second dry recovery status");
    require(second.value().already_ok == 2, "second pass sees recovered rows");

    const Block block3 = MakeBlockWithPrev(hash2, 3);
    const uint256 hash3 = block3.GetHash();
    auto pos3 = block_storage.writeBlock(hash3, block3);
    require(pos3.status() == Status::Ok, "write block3");

    const Block block4 = MakeBlockWithPrev(hash3, 4);
    const uint256 hash4 = block4.GetHash();
    auto pos4 = block_storage.writeBlock(hash4, block4);
    require(pos4.status() == Status::Ok, "write block4");

    daemon::HeaderMetadataRecoveryOptions missing_height_dry;
    missing_height_dry.datadir = root;
    missing_height_dry.window_start = 3;
    missing_height_dry.window_end = 4;
    missing_height_dry.write = false;
    missing_height_dry.live_chain_db = &db;
    missing_height_dry.live_block_storage = &block_storage;

    auto missing_dry = daemon::RecoverMissingHeaderMetadataRange(missing_height_dry);
    require(missing_dry.status() == Status::Ok, "missing-height dry recovery status");
    require(missing_dry.value().final_status == "dry_run_complete",
            "missing-height dry final_status");
    require(missing_dry.value().recoverable == 2, "missing-height dry recoverable count");
    require(db.getBlockHashByHeight(3).status() == Status::NotFound,
            "dry run must not write height index");

    auto missing_height_write = missing_height_dry;
    missing_height_write.write = true;
    missing_height_write.write_token = &token;
    auto missing_written = daemon::RecoverMissingHeaderMetadataRange(missing_height_write);
    require(missing_written.status() == Status::Ok, "missing-height write recovery status");
    require(missing_written.value().final_status == "ok", "missing-height write final_status");
    require(missing_written.value().recovered == 2, "missing-height recovered count");

    auto recovered_hash3 = db.getBlockHashByHeight(3);
    require(recovered_hash3.status() == Status::Ok, "height 3 index recovered");
    require(recovered_hash3.value() == hash3, "height 3 hash recovered");
    auto recovered_hash4 = db.getBlockHashByHeight(4);
    require(recovered_hash4.status() == Status::Ok, "height 4 index recovered");
    require(recovered_hash4.value() == hash4, "height 4 hash recovered");

    auto meta3 = db.getHeaderMetadata(hash3);
    require(meta3.status() == Status::Ok, "block3 metadata recovered");
    require(meta3.value().data_pos == pos3.value().offset, "block3 data_pos recovered");
    require(meta3.value().data_size == pos3.value().size, "block3 data_size recovered");
    require((meta3.value().status_flags & BLOCK_HAVE_DATA) != 0, "block3 HAVE_DATA set");
    require((meta3.value().status_flags & BLOCK_HAVE_UNDO) == 0,
            "block3 HAVE_UNDO not fabricated");

    block_storage.close();
    db.close();
    std::filesystem::remove_all(root);

    std::cout << "[PASS] header metadata recovery reconstructs missing rows safely" << std::endl;
    return 0;
}

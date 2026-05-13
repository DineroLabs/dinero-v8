/**
 * @file test_block_storage_roundtrip.cpp
 * @brief BlockStorage blk*.dat round-trip test (serialization + checksum + readback)
 *
 * Ensures:
 * 1) BlockStorage::writeBlock writes Dinero wire bytes to disk
 * 2) BlockStorage::readBlock can deserialize the same bytes back into a Block
 * 3) Re-serializing the decoded Block matches the original bytes (identity)
 */

#include "storage/block_storage.h"
#include "primitives/block.h"
#include "primitives/amount.h"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>  // _getpid
#define getpid _getpid
#else
#include <unistd.h>   // getpid
#endif

using namespace dinero;

#define TEST(name) \
    void test_##name(); \
    struct TestRunner_##name { \
        TestRunner_##name() { \
            std::cout << "Running: " << #name << "..." << std::flush; \
            test_##name(); \
            std::cout << " OK" << std::endl; \
        } \
    } test_runner_##name; \
    void test_##name()

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "\nASSERT_TRUE failed: " #cond << std::endl; \
            std::abort(); \
        } \
    } while (0)

static Transaction makeCoinbaseTx() {
    Transaction tx;
    tx.version = 2;

    TxInput in;
    in.prevout.txid = TxId(uint256());   // coinbase
    in.prevout.vout = 0xffffffff;
    in.scriptSig = {0x03, 0x01, 0x00, 0x00};  // dummy height push
    in.sequence = 0xffffffff;
    tx.vin.push_back(in);

    TxOutput out;
    out.value = AmountUna::Una(50);
    // P2WPKH-like dummy script: OP_0 <20 bytes>
    out.scriptPubKey = {0x00, 0x14};
    out.scriptPubKey.insert(out.scriptPubKey.end(), 20, 0x00);
    tx.vout.push_back(out);

    // Ensure witness flag matches actual data (none)
    tx.DetectWitnessVersion();

    return tx;
}

TEST(BlockStorageRoundTrip_WriteReadIdentity) {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto dir = tmp / ("din_block_storage_rt_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    BlockStorage storage;
    ASSERT_TRUE(storage.init(dir) == Status::Ok);

    Transaction coinbase = makeCoinbaseTx();

    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.merkle_root = coinbase.GetTxid().AsUint256();
    block.header.utreexo_root = uint256();
    block.header.timestamp = 123456789;
    block.header.difficulty = 0x207fffff;
    block.header.nonce = 0;
    block.header.ZeroReserved();
    block.vtx.push_back(coinbase);
    block.utreexo.reset();

    const uint256 block_hash = block.GetHash();

    auto write_res = storage.writeBlock(block_hash, block);
    ASSERT_TRUE(write_res.status() == Status::Ok);
    const FilePosition pos = write_res.value();
    ASSERT_TRUE(!pos.isNull());

    auto read_res = storage.readBlock(pos);
    ASSERT_TRUE(read_res.status() == Status::Ok);
    const Block& decoded = read_res.value();

    ASSERT_TRUE(decoded.vtx.size() == 1);
    ASSERT_TRUE(decoded.header.merkle_root == block.header.merkle_root);

    // Full byte-for-byte identity on wire serialization (includes CompactSize + tx bytes + utreexo flag)
    ASSERT_TRUE(decoded.Serialize() == block.Serialize());

    storage.close();
    std::filesystem::remove_all(dir);
}

TEST(BlockStorageRoundTrip_MissingBlockFileReturnsNotFound) {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto dir = tmp / ("din_block_storage_missing_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    BlockStorage storage;
    ASSERT_TRUE(storage.init(dir) == Status::Ok);

    Transaction coinbase = makeCoinbaseTx();

    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = uint256();
    block.header.merkle_root = coinbase.GetTxid().AsUint256();
    block.header.utreexo_root = uint256();
    block.header.timestamp = 123456790;
    block.header.difficulty = 0x207fffff;
    block.header.nonce = 1;
    block.header.ZeroReserved();
    block.vtx.push_back(coinbase);
    block.utreexo.reset();

    const uint256 block_hash = block.GetHash();

    auto write_res = storage.writeBlock(block_hash, block);
    ASSERT_TRUE(write_res.status() == Status::Ok);
    const FilePosition pos = write_res.value();
    ASSERT_TRUE(!pos.isNull());

    storage.close();

    const auto blk_path = dir / "blocks" / "blk00000.dat";
    ASSERT_TRUE(std::filesystem::exists(blk_path));
    std::filesystem::remove(blk_path);

    auto read_res = storage.readBlock(pos);
    ASSERT_TRUE(read_res.status() == Status::NotFound);
    ASSERT_TRUE(storage.hasBlock(pos) == Status::NotFound);

    std::filesystem::remove_all(dir);
}

int main() {
    std::cout << "\n===========================================================\n";
    std::cout << "BLOCK STORAGE ROUND-TRIP TESTS\n";
    std::cout << "===========================================================\n\n";

    // Tests run via static initialization

    std::cout << "\nALL BLOCK STORAGE ROUND-TRIP TESTS PASSED!" << std::endl;
    std::cout << "===========================================================\n" << std::endl;
    return 0;
}

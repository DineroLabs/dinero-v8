/**
 * Regression: an AssumeUTXO PRE-BASE backfill body is written to flat-file
 * storage and then permanently forgotten.
 *
 * Field failure (DineroDPI macOS datadir, snapshot base 83572): nine bodies at
 * heights 34396..36815 were downloaded, verified and stored, yet every
 * background-validation pass reported them unreadable, so the replay restarted
 * from zero forever.
 *
 * Root cause: ChainstateService::PersistStoredBodyPosition can only CREATE a
 * missing ChainDB header-metadata row from g_block_index. That index is rebuilt
 * from ChainDB's HEIGHT index, which on a snapshot-bootstrapped node has no
 * pre-base entries (the live node logged "Loaded 1 block index entries"). So
 * FindBlockIndex() missed, the function returned silently, no row was written,
 * and HasArchivalBlockBody / ReadArchivalBlock — which resolve a body ONLY via
 * getHeaderMetadata(hash).{file_number,data_pos,data_size} — could never see it.
 *
 * The header for such a block IS known: it lives in the HeaderChainSelector,
 * the same source background validation uses to derive the canonical pre-base
 * chain. This pins that fallback.
 *
 * Checks are exit-nonzero (require/abort), NOT assert() — this test must still
 * gate under NDEBUG (Release/CI).
 */

#include "daemon/services/chainstate_service.h"
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"
#include "consensus/chainparams.h"
#include "consensus/header_chain.h"
#include "primitives/amount.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "storage/block_storage.h"
#include "storage/chain_db.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace dinero;
using dinero::consensus::HeaderChainSelector;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "[FAIL] " << msg << std::endl;
        std::abort();
    }
}

std::filesystem::path MakeTempRoot() {
    auto root = std::filesystem::temp_directory_path() /
                ("din_persist_pre_base_body_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

// Regtest header: PoW is intentionally skipped there, so nonce=1 is accepted.
BlockHeader MakeHeader(const uint256& prev_hash, uint32_t time,
                       const uint256& merkle_root) {
    BlockHeader header;
    header.version = 1;
    header.prev_block_hash = prev_hash;
    header.merkle_root = merkle_root;
    header.timestamp = time;
    header.difficulty = 0x1d00ffff;
    header.nonce = 1;
    header.utreexo_root = uint256();
    return header;
}

Transaction MakeCoinbaseTx(uint8_t tag) {
    Transaction tx;
    tx.version = 2;

    TxInput in;
    in.prevout.txid = TxId(uint256());
    in.prevout.vout = 0xffffffff;
    in.scriptSig = {0x03, tag, 0x00, 0x00};
    in.sequence = 0xffffffff;
    tx.vin.push_back(in);

    TxOutput out;
    out.value = AmountUna::Una(50);
    out.scriptPubKey = {0x00, 0x14};
    out.scriptPubKey.insert(out.scriptPubKey.end(), 20, tag);
    tx.vout.push_back(out);

    tx.DetectWitnessVersion();
    return tx;
}

// A body whose header is exactly `header`, so Block::GetHash() == header hash.
Block MakeBody(const BlockHeader& header, uint8_t tag) {
    Block block;
    block.header = header;
    block.vtx.push_back(MakeCoinbaseTx(tag));
    return block;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// #1 — the regression: header known only to the HeaderChainSelector
// ─────────────────────────────────────────────────────────────────────────────
void test01_PreBaseBodyBecomesReadableFromHeaderChain(
        const std::filesystem::path& root) {
    const auto dir = root / "t01";
    std::filesystem::create_directories(dir);

    ChainDB db;
    require(db.init(dir / "chaindb") == Status::Ok, "t01: ChainDB::init failed");

    auto storage = std::make_shared<BlockStorage>();
    require(storage->init(dir) == Status::Ok, "t01: BlockStorage::init failed");

    // Canonical pre-base chain lives ONLY in the header selector — exactly the
    // state of a snapshot-bootstrapped node below its base height.
    auto selector = std::make_shared<HeaderChainSelector>();
    const Transaction cb0 = MakeCoinbaseTx(0x01);
    const BlockHeader genesis =
        MakeHeader(uint256(), 1000000, cb0.GetTxid().AsUint256());
    require(selector->AddHeader(genesis), "t01: genesis AddHeader rejected");

    const Transaction cb1 = MakeCoinbaseTx(0x02);
    const BlockHeader h1 =
        MakeHeader(genesis.GetHash(), 1000001, cb1.GetTxid().AsUint256());
    require(selector->AddHeader(h1), "t01: h1 AddHeader rejected");

    const Block body = MakeBody(h1, 0x02);
    const uint256 hash = body.GetHash();
    require(hash == h1.GetHash(), "t01: body hash must equal its header hash");

    // The scheduler's store step: body on disk at a verified position.
    const auto pos_result = storage->writeBlock(hash, body);
    require(pos_result.status() == Status::Ok, "t01: writeBlock failed");
    const FilePosition pos = pos_result.value();

    ChainstateService svc;
    svc.setChainDB(&db);
    svc.setBlockStorage(storage);
    svc.setHeaderChainSelector(selector);

    // Preconditions — these are the field state, and they must all hold or the
    // test is not reproducing the bug.
    require(db.getHeaderMetadata(hash).status() != Status::Ok,
            "t01: precondition — no header-metadata row may exist yet");
    require(dinero::FindBlockIndex(hash) == nullptr,
            "t01: precondition — hash must be absent from g_block_index");
    require(!svc.hasBlockByHash(hash),
            "t01: precondition — body must not resolve before the persist call");

    // The #309 persist callback the scheduler invokes after storing a body.
    svc.PersistStoredBodyPosition(hash, pos);

    require(svc.hasBlockByHash(hash),
            "t01: REGRESSION — pre-base body still unresolvable after persist "
            "(no header-metadata row was created)");

    const auto read = svc.getBlockByHash(hash);
    require(read.status() == Status::Ok,
            "t01: strict archival read failed after persist");
    require(read.value().GetHash() == hash,
            "t01: strict archival read returned the wrong block");

    const auto md_result = db.getHeaderMetadata(hash);
    require(md_result.status() == Status::Ok, "t01: metadata row missing");
    const auto md = md_result.value();
    require(md.file_number == pos.file_number, "t01: file_number mismatch");
    require(md.data_pos == pos.offset, "t01: data_pos mismatch");
    require(md.data_size == pos.size, "t01: data_size mismatch");
    require((md.status_flags & BLOCK_HAVE_DATA) != 0,
            "t01: BLOCK_HAVE_DATA not set");
    require(md.height == 1,
            "t01: height must come from the header chain, got " +
            std::to_string(md.height));
    require(md.parent_hash == genesis.GetHash(),
            "t01: parent_hash must come from the header chain");
    // chainwork is the one field whose two source branches use different
    // plumbing (ChainworkFromHex(idx->chainwork) vs entry.chainwork), so pin it.
    const auto entry = selector->GetHeaderValue(hash);
    require(entry.has_value(), "t01: header chain lost the entry");
    require(md.chainwork == entry->chainwork,
            "t01: chainwork must be copied from the header chain entry");
    require(!(md.chainwork == arith_uint256()), "t01: chainwork must not be zero");

    storage->close();
    db.close();
    std::cout << "  [OK] pre-base body readable via header-chain fallback"
              << std::endl;
}

// ─────────────────────────────────────────────────────────────────────────────
// #2 — a hash known to NOTHING must not fabricate a row
// ─────────────────────────────────────────────────────────────────────────────
void test02_UnknownHashWritesNoRow(const std::filesystem::path& root) {
    const auto dir = root / "t02";
    std::filesystem::create_directories(dir);

    ChainDB db;
    require(db.init(dir / "chaindb") == Status::Ok, "t02: ChainDB::init failed");

    auto storage = std::make_shared<BlockStorage>();
    require(storage->init(dir) == Status::Ok, "t02: BlockStorage::init failed");

    auto selector = std::make_shared<HeaderChainSelector>();

    const Transaction cb = MakeCoinbaseTx(0x03);
    const BlockHeader orphan =
        MakeHeader(uint256(), 1000002, cb.GetTxid().AsUint256());
    const Block body = MakeBody(orphan, 0x03);
    const uint256 hash = body.GetHash();

    const auto pos_result = storage->writeBlock(hash, body);
    require(pos_result.status() == Status::Ok, "t02: writeBlock failed");

    ChainstateService svc;
    svc.setChainDB(&db);
    svc.setBlockStorage(storage);
    svc.setHeaderChainSelector(selector);

    svc.PersistStoredBodyPosition(hash, pos_result.value());

    require(db.getHeaderMetadata(hash).status() != Status::Ok,
            "t02: a hash unknown to both the block index and the header chain "
            "must not get a synthesized metadata row");
    require(!svc.hasBlockByHash(hash), "t02: body must stay unresolvable");

    storage->close();
    db.close();
    std::cout << "  [OK] unknown hash writes no row" << std::endl;
}

int main() {
    SelectParams(Chain::REGTEST);
    const auto root = MakeTempRoot();

    std::cout << "=== PersistStoredBodyPosition pre-base body tests ===\n";
    test01_PreBaseBodyBecomesReadableFromHeaderChain(root);
    test02_UnknownHashWritesNoRow(root);
    std::cout << "ALL PRE-BASE BODY PERSIST TESTS PASSED\n";

    std::filesystem::remove_all(root);
    return 0;
}

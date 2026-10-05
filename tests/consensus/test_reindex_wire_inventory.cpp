#include "consensus/reindexer_detail.h"
#include "consensus/chainparams.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace dinero;
using namespace dinero::consensus;
using namespace dinero::consensus::reindex_detail;
using Bytes = std::vector<uint8_t>;
namespace {
void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void U32(std::ofstream& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.put(static_cast<char>(value >> (8 * i)));
}
void Frame(std::ofstream& out, const Bytes& bytes, bool corrupt = false) {
    uint32_t checksum = 2166136261u;
    for (const auto byte : bytes) { checksum ^= byte; checksum *= 16777619u; }
    U32(out, Params().magic); U32(out, static_cast<uint32_t>(bytes.size()));
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    U32(out, checksum ^ (corrupt ? 1u : 0u));
    Require(out.good(), "fixture frame write failed");
}
Transaction Coinbase() {
    Transaction tx; tx.version = 2;
    TxInput in; in.prevout.vout = UINT32_MAX; in.scriptSig = {1, 1};
    tx.vin.push_back(in);
    tx.vout.emplace_back(AmountUna::Una(1), Bytes{0x51});
    return tx;
}
struct Temp {
    std::filesystem::path path;
    Temp() {
        path = std::filesystem::temp_directory_path() /
            ("dinero-reindex-wire-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        Require(std::filesystem::create_directory(path), "fixture directory exists");
    }
    ~Temp() { std::error_code error; std::filesystem::remove_all(path, error); }
};
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected independent envelope vector");
        SelectParams(Chain::REGTEST);
        std::ifstream vector_file(argv[1], std::ios::binary);
        Require(vector_file.good(), "envelope vector missing");
        Bytes envelope{std::istreambuf_iterator<char>(vector_file), {}};
        Require(!envelope.empty(), "empty envelope vector");
        Block first;
        first.header.version = 1;
        first.header.prev_block_hash = uint256::FromHexUnsafe(Params().genesis_hash);
        first.header.difficulty = Params().genesis.nBits;
        first.vtx.push_back(Coinbase());
        auto old_string = first.Serialize();
        Bytes old(old_string.begin(), old_string.end());
        Require(Block::Deserialize(old).has_value(), "historical fixture must decode");
        BlockHeader second = first.header;
        second.prev_block_hash = first.GetHash(); second.nonce = 1;
        auto prefix = second.SerializeForHash();
        Bytes mixed(prefix.begin(), prefix.end()); mixed.push_back(2);
        const auto cb = Coinbase().Serialize(TxSerializationMode::WithWitness);
        mixed.insert(mixed.end(), cb.begin(), cb.end());
        mixed.insert(mixed.end(), envelope.begin(), envelope.end()); mixed.push_back(0);
        Require(!Block::Deserialize(mixed), "mixed frame must distinguish legacy scan");
        // Header linkage is intentionally tested independently of Merkle roots,
        // PoW or proofs. No canonical validity is asserted for these frames.
        BlockHeader third = second; third.prev_block_hash = second.GetHash(); third.nonce = 2;
        const auto third_prefix = third.SerializeForHash();
        Bytes malformed(third_prefix.begin(), third_prefix.end());
        Require(!Block::Deserialize(malformed), "third body must be malformed");
        Temp dir; auto file = dir.path / "blk00007.dat";
        {
            std::ofstream out(file, std::ios::binary);
            Frame(out, old); Frame(out, mixed); Frame(out, malformed);
        }
        BlockReindexer::Stats stats;
        auto scanned = ReadDiskBlocks({file}, &stats);
        Require(scanned.ok() && scanned->size() == 3, "scanner dropped selected raw bodies");
        const auto& records = *scanned;
        Require(stats.parse_skipped_blocks == 0, "valid frame counted as skipped");
        Require(stats.total_bytes == old.size() + mixed.size() + malformed.size() + 36,
            "frame accounting changed");
        Require(records[0].body == old && records[1].body == mixed && records[2].body == malformed,
            "inventory changed exact body bytes");
        Require(records[1].header.SerializeForHash() == second.SerializeForHash() &&
            records[1].hash == second.GetHash() && records[1].prev_hash == first.GetHash(),
            "header identity changed");
        Require(records[1].pos == FilePosition(7, old.size() + 12, mixed.size()),
            "framed locator changed");
        auto best = SelectCanonicalChain(records);
        Require(best.ok() && *best == std::vector<size_t>({0, 1, 2}), "raw body chain silently shortened");
        auto anchored = SelectCanonicalChain(records, second.GetHash().GetHex());
        Require(anchored.ok() && *anchored == std::vector<size_t>({0, 1}), "anchored chain lost mixed frame");
        std::cout << "ReindexWireInventory: exact historical/mixed/malformed inventory and selection PASS\n";
        {
            std::ofstream out(file, std::ios::binary | std::ios::app);
            Frame(out, mixed, true);
        }
        stats = {};
        scanned = ReadDiskBlocks({file}, &stats);
        Require(scanned.ok() && scanned->size() == 3 && stats.parse_skipped_blocks == 1,
            "checksum skip policy changed");
        for (unsigned size = 1; size < 4; ++size) {
            auto partial = dir.path / ("partial-" + std::to_string(size));
            { std::ofstream out(partial, std::ios::binary); out.write("abc", size); }
            Require(!ReadDiskBlocks({file, partial}).ok(), "partial magic accepted as clean EOF");
        }
        auto tail = dir.path / "tail";
        { std::ofstream out(tail, std::ios::binary); U32(out, Params().magic); U32(out, 128); out.put(0); }
        Require(!ReadDiskBlocks({tail}).ok(), "truncated frame body accepted");
        Require(!ReadDiskBlocks({dir.path / "missing"}).ok(), "missing file returned empty success");
        std::cout << "ReindexWireInventory: checksum policy, incomplete framing and IO refusal PASS\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

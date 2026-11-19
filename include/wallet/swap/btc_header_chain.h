#pragma once
// Bitcoin header chain for swaps without Bitcoin Core
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3.2).
//
// The node keeps its own chain of Bitcoin block headers and believes nothing
// about a confirmed transaction that does not prove into it. This file is the
// chain: where the headers come from (peers, servers) is someone else's job.
//
// A header is accepted only if
//   - it links to a header already accepted;
//   - its hash meets the target in its own `bits`, and that target is valid
//     and not easier than the network's limit;
//   - its `bits` are what the 2016-block retarget rule requires (networks
//     that retarget and do not allow minimum-difficulty blocks);
//   - its time is above the median of the previous eleven headers and no more
//     than two hours ahead of the caller's clock.
// The best chain is the one with the most accumulated work, never the longest.
//
// The chain starts at a checkpoint compiled into the binary, whose height is a
// multiple of 2016 so the first retarget after it can be checked. Headers below
// the checkpoint are unknown and refused.
//
// Pure: no I/O, no clock, no globals. Not thread-safe; the owner serializes.

#include "wallet/swap/htlc.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dinero::swap {

inline constexpr uint32_t kBtcRetargetInterval = 2016;               // blocks
inline constexpr uint32_t kBtcRetargetTimespan = 14 * 24 * 60 * 60;  // seconds
inline constexpr uint32_t kBtcMaxFutureBlockTime = 2 * 60 * 60;      // seconds
inline constexpr uint32_t kBtcMedianTimeSpan = 11;                   // headers
// A fork that leaves the best chain deeper than this is refused: it has never
// happened on Bitcoin, and it bounds what a hostile source can make us keep.
inline constexpr uint32_t kBtcMaxReorgDepth = kBtcRetargetInterval;
// Headers kept off the best chain (competing tips); more are refused.
inline constexpr size_t kBtcMaxSideHeaders = 4096;

struct BtcBlockHeader {
    int32_t version{};
    Bytes32 prev_hash{};    // wire byte order (reverse of the usual hex)
    Bytes32 merkle_root{};  // wire byte order
    uint32_t time{};
    uint32_t bits{};
    uint32_t nonce{};
    bool operator==(const BtcBlockHeader&) const = default;
};

// Exactly 80 bytes, or nullopt.
std::optional<BtcBlockHeader> ParseBtcBlockHeader(const std::vector<uint8_t>& raw);
std::vector<uint8_t> SerializeBtcBlockHeader(const BtcBlockHeader& header);
Bytes32 BtcBlockHash(const BtcBlockHeader& header);  // wire byte order

// The usual hex of a block hash or txid (byte-reversed), and back.
std::string BtcHashToHex(const Bytes32& wire);
std::optional<Bytes32> BtcHashFromHex(const std::string& hex);  // 64 hex digits, or nullopt

// True if `bits` encodes a positive target that fits 256 bits and is not above
// (easier than) `pow_limit_bits`, and the header's hash is at or below it.
bool BtcCheckProofOfWork(const BtcBlockHeader& header, uint32_t pow_limit_bits);

// Bitcoin's retarget: the target of the period that ended with `last_bits`,
// scaled by how long the period took (clamped to a quarter and four times the
// two weeks), never above the limit.
uint32_t BtcNextWorkRequired(uint32_t first_block_time, uint32_t last_block_time, uint32_t last_bits,
                             uint32_t pow_limit_bits);

struct BtcChainParams {
    std::string name;                    // "main", "regtest"
    uint32_t pow_limit_bits{};
    bool retargets{true};                // false: every header keeps its parent's bits (regtest)
    bool allows_min_difficulty{false};   // testnet rules: `bits` are only checked against the limit
    uint32_t checkpoint_height{};        // a multiple of kBtcRetargetInterval
    BtcBlockHeader checkpoint;

    // Mainnet from block 971,712 (a retarget boundary mined in October 2026).
    // The header was taken from a Bitcoin peer and matched against two
    // independent servers; it carries mainnet proof of work, which cannot be
    // forged. Move it forward with each release: see the note in the .cpp.
    static BtcChainParams Mainnet();
    static BtcChainParams MainnetFromGenesis();  // every header since 2009: tests only
    // Testnet4 from block 157,248. Testnet allows minimum-difficulty blocks, so
    // `bits` are only held to the limit there: weaker than mainnet by design.
    static BtcChainParams Testnet4();
    static BtcChainParams Regtest();     // Bitcoin Core's regtest genesis
};

class BtcHeaderChain {
public:
    // Throws std::invalid_argument if the checkpoint does not satisfy its own
    // proof of work or its height is not a retarget boundary.
    explicit BtcHeaderChain(BtcChainParams params);

    enum class Status {
        Accepted,   // new and valid (it may or may not have become the tip)
        Duplicate,  // already known
        Orphan,     // its parent is unknown: ask for the headers in between
        Invalid,    // breaks a rule: `reason` says which; the source is lying or broken
    };
    struct Result {
        Status status{Status::Invalid};
        std::string reason;
    };
    // `now_unix` is the caller's wall clock (the two-hour future limit).
    Result Accept(const BtcBlockHeader& header, uint32_t now_unix);

    const BtcChainParams& params() const { return params_; }
    uint32_t TipHeight() const;
    Bytes32 TipHash() const;  // wire byte order
    const BtcBlockHeader& Tip() const;

    // On the best chain only.
    std::optional<BtcBlockHeader> AtHeight(uint32_t height) const;
    std::optional<uint32_t> HeightOf(const Bytes32& block_hash) const;
    // 1 for the tip, 0 if the block is not on the best chain.
    uint32_t Confirmations(const Bytes32& block_hash) const;

    // Median time of the tip and its ten ancestors; nullopt while fewer than
    // eleven headers are known (just after a checkpoint above genesis).
    std::optional<uint32_t> MedianTimePast() const;

    // Block hashes for a `getheaders` request: the tip, then back in steps that
    // double, ending with the checkpoint.
    std::vector<Bytes32> Locator() const;

    // The best chain above the checkpoint as raw 80-byte headers, oldest first.
    std::vector<uint8_t> SerializeBestChain() const;
    // Replays `bytes` through Accept: a damaged or tampered file cannot put an
    // invalid header in the chain. Returns how many headers were accepted;
    // stops at the first one that is not.
    size_t LoadBestChain(const std::vector<uint8_t>& bytes, uint32_t now_unix);

    // Takes the headers of `other`'s best chain that this chain lacks, each
    // through Accept (those already known are skipped). Returns how many were
    // new; stops at the first one this chain refuses.
    size_t Adopt(const BtcHeaderChain& other, uint32_t now_unix);

    size_t side_header_count() const { return nodes_.size() - best_.size(); }

private:
    struct Node {
        BtcBlockHeader header;
        Bytes32 hash{};
        uint32_t height{};
        // Work accumulated from the checkpoint (exclusive), little-endian limbs.
        uint64_t work[4]{};
    };
    struct HashKey {
        size_t operator()(const Bytes32& h) const noexcept;
    };

    const Node* Find(const Bytes32& hash) const;
    const Node* Ancestor(const Node* node, uint32_t height) const;
    std::optional<uint32_t> MedianTimePastOf(const Node* node) const;
    uint32_t ExpectedBits(const Node* parent, const BtcBlockHeader& header) const;
    void Reorganize(const Bytes32& new_tip);
    void DropStaleSideHeaders();

    BtcChainParams params_;
    std::unordered_map<Bytes32, Node, HashKey> nodes_;
    std::vector<Bytes32> best_;  // best_[i] is the block at checkpoint_height + i
};

}  // namespace dinero::swap

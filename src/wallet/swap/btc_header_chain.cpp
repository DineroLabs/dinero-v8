#include "wallet/swap/btc_header_chain.h"

#include "crypto/sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace dinero::swap {
namespace {

// 256-bit unsigned integer, little-endian 64-bit limbs. Only what targets and
// chain work need; kept here so this file depends on nothing but SHA-256.
using U256 = std::array<uint64_t, 4>;

int Compare(const U256& a, const U256& b) {
    for (int i = 3; i >= 0; --i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

bool IsZero(const U256& a) { return (a[0] | a[1] | a[2] | a[3]) == 0; }

// No 128-bit integers and no compiler built-ins here: MSVC has neither.

// a += b; returns the carry out of bit 255.
bool AddInPlace(U256& a, const U256& b) {
    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i) {
        const uint64_t sum = a[i] + b[i];
        const uint64_t with_carry = sum + carry;
        carry = (sum < a[i] || with_carry < sum) ? 1 : 0;
        a[i] = with_carry;
    }
    return carry != 0;
}

void SubInPlace(U256& a, const U256& b) {  // requires a >= b (or the wrap-around the caller asks for)
    uint64_t borrow = 0;
    for (int i = 0; i < 4; ++i) {
        const uint64_t diff = a[i] - b[i];
        const uint64_t next_borrow = (a[i] < b[i] || diff < borrow) ? 1 : 0;
        a[i] = diff - borrow;
        borrow = next_borrow;
    }
}

// floor(a * m / d), exact: the product is held in 320 bits, so it never wraps.
// nullopt if the quotient does not fit 256 bits. d != 0.
// In 32-bit limbs, so that every intermediate fits 64 bits.
std::optional<U256> MulDiv(const U256& a, uint32_t m, uint32_t d) {
    uint32_t wide[10];  // least significant first
    uint64_t carry = 0;
    for (int i = 0; i < 8; ++i) {
        const uint64_t limb = (a[i / 2] >> (32 * (i % 2))) & 0xffffffffu;
        carry += limb * m;  // < 2^32 * 2^32, and the carry in is < 2^32: no overflow
        wide[i] = static_cast<uint32_t>(carry);
        carry >>= 32;
    }
    wide[8] = static_cast<uint32_t>(carry);
    wide[9] = 0;
    uint64_t rem = 0;
    for (int i = 9; i >= 0; --i) {
        const uint64_t cur = (rem << 32) | wide[i];  // rem < d < 2^32
        wide[i] = static_cast<uint32_t>(cur / d);
        rem = cur % d;
    }
    if (wide[8] != 0 || wide[9] != 0) return std::nullopt;
    U256 out{};
    for (int i = 0; i < 4; ++i) out[i] = (static_cast<uint64_t>(wide[2 * i + 1]) << 32) | wide[2 * i];
    return out;
}

bool Bit(const U256& a, int bit) { return (a[bit / 64] >> (bit % 64)) & 1; }

// Quotient of n / d (d != 0), schoolbook binary long division.
U256 Divide(const U256& n, const U256& d) {
    U256 q{}, r{};
    for (int bit = 255; bit >= 0; --bit) {
        const bool overflow = Bit(r, 255);  // r << 1 would lose its top bit
        for (int i = 3; i > 0; --i) r[i] = (r[i] << 1) | (r[i - 1] >> 63);
        r[0] = (r[0] << 1) | (Bit(n, bit) ? 1 : 0);
        if (overflow || Compare(r, d) >= 0) {
            SubInPlace(r, d);  // with overflow the true remainder is r + 2^256, and wrap-around subtraction is exact
            q[bit / 64] |= uint64_t{1} << (bit % 64);
        }
    }
    return q;
}

int BitLength(const U256& a) {
    for (int i = 3; i >= 0; --i) {
        if (a[i]) {
            int bits = 0;
            for (uint64_t v = a[i]; v; v >>= 1) ++bits;
            return 64 * i + bits;
        }
    }
    return 0;
}

// Bitcoin's compact target encoding ("bits"). nullopt: negative, zero, or
// wider than 256 bits.
std::optional<U256> TargetFromCompact(uint32_t bits) {
    const uint32_t size = bits >> 24;
    const uint32_t word = bits & 0x007fffff;
    if (word == 0) return std::nullopt;           // zero target
    if (bits & 0x00800000) return std::nullopt;   // negative
    if (size > 34 || (word > 0xff && size > 33) || (word > 0xffff && size > 32)) return std::nullopt;  // overflow
    U256 t{};
    if (size <= 3) {
        t[0] = word >> (8 * (3 - size));
        if (t[0] == 0) return std::nullopt;
        return t;
    }
    const uint32_t shift = 8 * (size - 3);  // at most 248
    t[shift / 64] = static_cast<uint64_t>(word) << (shift % 64);
    if (shift % 64 > 40 && shift / 64 < 3) t[shift / 64 + 1] = static_cast<uint64_t>(word) >> (64 - shift % 64);
    return t;
}

uint32_t CompactFromTarget(const U256& t) {
    uint32_t size = static_cast<uint32_t>((BitLength(t) + 7) / 8);
    uint32_t compact = 0;
    if (size <= 3) {
        compact = static_cast<uint32_t>(t[0] << (8 * (3 - size)));
    } else {
        const uint32_t shift = 8 * (size - 3);
        uint64_t low = t[shift / 64] >> (shift % 64);
        if (shift % 64 && shift / 64 < 3) low |= t[shift / 64 + 1] << (64 - shift % 64);
        compact = static_cast<uint32_t>(low & 0x00ffffff);
    }
    if (compact & 0x00800000) {  // the mantissa's top bit is the sign: use one more byte
        compact >>= 8;
        ++size;
    }
    return compact | (size << 24);
}

U256 FromWire(const Bytes32& wire) {  // a hash's wire bytes are the number, little-endian
    U256 v{};
    for (int i = 0; i < 4; ++i) {
        for (int b = 0; b < 8; ++b) v[i] |= static_cast<uint64_t>(wire[8 * i + b]) << (8 * b);
    }
    return v;
}

// Work of one block: 2^256 / (target + 1), computed as ~target / (target + 1) + 1.
U256 BlockProof(uint32_t bits) {
    const auto target = TargetFromCompact(bits);
    if (!target) return U256{};
    U256 inverted;
    for (int i = 0; i < 4; ++i) inverted[i] = ~(*target)[i];
    U256 plus_one = *target;
    if (AddInPlace(plus_one, U256{1, 0, 0, 0})) return U256{1, 0, 0, 0};  // target was 2^256 - 1
    U256 work = Divide(inverted, plus_one);
    AddInPlace(work, U256{1, 0, 0, 0});
    return work;
}

void PutLe32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t GetLe32(const uint8_t* p) {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

}  // namespace

// ---- Headers ------------------------------------------------------------------

std::optional<BtcBlockHeader> ParseBtcBlockHeader(const std::vector<uint8_t>& raw) {
    if (raw.size() != 80) return std::nullopt;
    BtcBlockHeader h;
    h.version = static_cast<int32_t>(GetLe32(raw.data()));
    std::copy(raw.begin() + 4, raw.begin() + 36, h.prev_hash.begin());
    std::copy(raw.begin() + 36, raw.begin() + 68, h.merkle_root.begin());
    h.time = GetLe32(raw.data() + 68);
    h.bits = GetLe32(raw.data() + 72);
    h.nonce = GetLe32(raw.data() + 76);
    return h;
}

std::vector<uint8_t> SerializeBtcBlockHeader(const BtcBlockHeader& h) {
    std::vector<uint8_t> out;
    out.reserve(80);
    PutLe32(out, static_cast<uint32_t>(h.version));
    out.insert(out.end(), h.prev_hash.begin(), h.prev_hash.end());
    out.insert(out.end(), h.merkle_root.begin(), h.merkle_root.end());
    PutLe32(out, h.time);
    PutLe32(out, h.bits);
    PutLe32(out, h.nonce);
    return out;
}

Bytes32 BtcBlockHash(const BtcBlockHeader& header) {
    const auto raw = SerializeBtcBlockHeader(header);
    Bytes32 once{}, twice{};
    crypto::CSHA256().Write(raw.data(), raw.size()).Finalize(once.data());
    crypto::CSHA256().Write(once.data(), once.size()).Finalize(twice.data());
    return twice;
}

std::string BtcHashToHex(const Bytes32& wire) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 31; i >= 0; --i) {
        out.push_back(digits[wire[i] >> 4]);
        out.push_back(digits[wire[i] & 15]);
    }
    return out;
}

std::optional<Bytes32> BtcHashFromHex(const std::string& hex) {
    if (hex.size() != 64) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Bytes32 wire{};
    for (int i = 0; i < 32; ++i) {
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        wire[31 - i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return wire;
}

bool BtcCheckProofOfWork(const BtcBlockHeader& header, uint32_t pow_limit_bits) {
    const auto target = TargetFromCompact(header.bits);
    const auto limit = TargetFromCompact(pow_limit_bits);
    if (!target || !limit || Compare(*target, *limit) > 0) return false;
    return Compare(FromWire(BtcBlockHash(header)), *target) <= 0;
}

uint32_t BtcNextWorkRequired(uint32_t first_block_time, uint32_t last_block_time, uint32_t last_bits,
                             uint32_t pow_limit_bits) {
    const auto limit = TargetFromCompact(pow_limit_bits);
    const auto target = TargetFromCompact(last_bits);
    if (!limit || !target) return pow_limit_bits;  // unreachable for accepted headers
    int64_t actual = static_cast<int64_t>(last_block_time) - static_cast<int64_t>(first_block_time);
    actual = std::clamp<int64_t>(actual, kBtcRetargetTimespan / 4, int64_t{kBtcRetargetTimespan} * 4);
    const auto next = MulDiv(*target, static_cast<uint32_t>(actual), kBtcRetargetTimespan);
    if (!next || Compare(*next, *limit) > 0) return CompactFromTarget(*limit);
    return CompactFromTarget(*next);
}

// ---- Networks -----------------------------------------------------------------

namespace {
BtcBlockHeader GenesisHeader(uint32_t time, uint32_t bits, uint32_t nonce) {
    BtcBlockHeader h;
    h.version = 1;
    h.merkle_root = *BtcHashFromHex("4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
    h.time = time;
    h.bits = bits;
    h.nonce = nonce;
    return h;
}

// A checkpoint is one real header at a retarget boundary, given as its 80 raw
// bytes. To move it forward: pick a boundary at least a month old, fetch the
// header from a Bitcoin peer, confirm two unrelated servers report the same
// hash at that height, and refresh the fixtures (tests/wallet/fixtures) so the
// tests replay real headers from the new checkpoint across the next retarget.
BtcBlockHeader CheckpointHeader(const char* raw_hex) {
    std::vector<uint8_t> raw;
    for (const char* p = raw_hex; p[0] && p[1]; p += 2) {
        auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        raw.push_back(static_cast<uint8_t>(nibble(p[0]) << 4 | nibble(p[1])));
    }
    return *ParseBtcBlockHeader(raw);
}

// Bitcoin's proof-of-work limit (mainnet and testnet4), and the bits of its
// genesis block. Nothing to do with Dinero's own difficulty: this file only
// follows Bitcoin's header chain.
constexpr uint32_t kBitcoinPowLimitBits = 0x1d00ffff;  // Bitcoin level
}  // namespace

BtcChainParams BtcChainParams::Mainnet() {
    BtcChainParams p;
    p.name = "main";
    p.pow_limit_bits = kBitcoinPowLimitBits;
    p.retargets = true;
    p.checkpoint_height = 971712;  // 00000000000000000000c185f0906690a08d62d6fe842682a888d5560217e467
    p.checkpoint = CheckpointHeader(
        "006005202e5ea9d74cd63c1fb65514e47c38f2a4767ad8d6ae51000000000000000000003d63f20e90eb3b43e72ea91858f41d1d"
        "92103442acfa6106400e04e08f4d94df8b1ad26a930002176d7224e8");
    return p;
}

BtcChainParams BtcChainParams::MainnetFromGenesis() {
    BtcChainParams p;
    p.name = "main";
    p.pow_limit_bits = kBitcoinPowLimitBits;
    p.retargets = true;
    p.checkpoint_height = 0;
    p.checkpoint = GenesisHeader(1231006505, kBitcoinPowLimitBits, 2083236893);
    return p;
}

BtcChainParams BtcChainParams::Testnet4() {
    BtcChainParams p;
    p.name = "testnet4";
    p.pow_limit_bits = kBitcoinPowLimitBits;
    p.retargets = true;
    p.allows_min_difficulty = true;
    p.checkpoint_height = 157248;  // 000000000000000311e5891b569e988c5c35da1cf4e59d8a7ead8fc8d322e384
    p.checkpoint = CheckpointHeader(
        "00604225783e9dae6773e19ec62820e5f726da4725bdc00d4d587ea9a070c300000000003d2985b298031247302f042b3b379233"
        "1269eb7b84a3b989a9ff2a7e67611bc5937ad96ad12404194dbe125e");
    return p;
}

BtcChainParams BtcChainParams::Regtest() {
    BtcChainParams p;
    p.name = "regtest";
    p.pow_limit_bits = 0x207fffff;
    p.retargets = false;
    p.checkpoint_height = 0;
    p.checkpoint = GenesisHeader(1296688602, 0x207fffff, 2);
    return p;
}

// ---- Chain --------------------------------------------------------------------

size_t BtcHeaderChain::HashKey::operator()(const Bytes32& h) const noexcept {
    size_t v;
    std::memcpy(&v, h.data(), sizeof v);  // block hashes are uniformly distributed
    return v;
}

BtcHeaderChain::BtcHeaderChain(BtcChainParams params) : params_(std::move(params)) {
    if (params_.checkpoint_height % kBtcRetargetInterval != 0) {
        throw std::invalid_argument("bitcoin checkpoint height is not a retarget boundary");
    }
    if (!BtcCheckProofOfWork(params_.checkpoint, params_.pow_limit_bits)) {
        throw std::invalid_argument("bitcoin checkpoint fails its own proof of work");
    }
    Node node;
    node.header = params_.checkpoint;
    node.hash = BtcBlockHash(params_.checkpoint);
    node.height = params_.checkpoint_height;
    best_.push_back(node.hash);
    nodes_.emplace(node.hash, node);
}

const BtcHeaderChain::Node* BtcHeaderChain::Find(const Bytes32& hash) const {
    const auto it = nodes_.find(hash);
    return it == nodes_.end() ? nullptr : &it->second;
}

// The ancestor of `node` at `height` (itself if equal); nullptr below the checkpoint.
const BtcHeaderChain::Node* BtcHeaderChain::Ancestor(const Node* node, uint32_t height) const {
    if (!node || height > node->height || height < params_.checkpoint_height) return nullptr;
    while (node && node->height > height) {
        const size_t index = node->height - params_.checkpoint_height;
        if (index < best_.size() && best_[index] == node->hash) {  // on the best chain: jump
            return Find(best_[height - params_.checkpoint_height]);
        }
        node = Find(node->header.prev_hash);
    }
    return node;
}

std::optional<uint32_t> BtcHeaderChain::MedianTimePastOf(const Node* node) const {
    std::vector<uint32_t> times;
    const Node* walk = node;
    while (walk && times.size() < kBtcMedianTimeSpan) {
        times.push_back(walk->header.time);
        if (walk->height == params_.checkpoint_height) break;
        walk = Find(walk->header.prev_hash);
    }
    // Fewer than eleven is Bitcoin's own rule only at the very start of the
    // chain. Above a later checkpoint the missing headers would make the median
    // too high, and a valid header could be refused: no answer instead.
    if (times.size() < kBtcMedianTimeSpan && params_.checkpoint_height != 0) return std::nullopt;
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

uint32_t BtcHeaderChain::ExpectedBits(const Node* parent, const BtcBlockHeader& header) const {
    (void)header;
    if (!params_.retargets) return parent->header.bits;
    const uint32_t height = parent->height + 1;
    if (height % kBtcRetargetInterval != 0) return parent->header.bits;
    // Bitcoin measures the period from its first block to its last: 2015 intervals.
    const Node* first = Ancestor(parent, height - kBtcRetargetInterval);
    if (!first) return 0;  // cannot happen: the checkpoint is a boundary
    return BtcNextWorkRequired(first->header.time, parent->header.time, parent->header.bits, params_.pow_limit_bits);
}

BtcHeaderChain::Result BtcHeaderChain::Accept(const BtcBlockHeader& header, uint32_t now_unix) {
    const Bytes32 hash = BtcBlockHash(header);
    if (Find(hash)) return {Status::Duplicate, ""};
    const Node* parent = Find(header.prev_hash);
    if (!parent) return {Status::Orphan, "unknown parent"};
    if (parent->height == UINT32_MAX) return {Status::Invalid, "height overflow"};
    if (parent->height + kBtcMaxReorgDepth < TipHeight()) {
        return {Status::Invalid, "fork deeper than " + std::to_string(kBtcMaxReorgDepth) + " blocks"};
    }
    if (!BtcCheckProofOfWork(header, params_.pow_limit_bits)) return {Status::Invalid, "proof of work"};
    if (!params_.allows_min_difficulty && header.bits != ExpectedBits(parent, header)) {
        return {Status::Invalid, "wrong difficulty bits"};
    }
    if (const auto mtp = MedianTimePastOf(parent); mtp && header.time <= *mtp) {
        return {Status::Invalid, "time not above the median of the previous eleven"};
    }
    if (static_cast<uint64_t>(header.time) > static_cast<uint64_t>(now_unix) + kBtcMaxFutureBlockTime) {
        return {Status::Invalid, "time more than two hours ahead"};
    }
    const bool extends_tip = header.prev_hash == TipHash();
    if (!extends_tip && side_header_count() >= kBtcMaxSideHeaders) {
        return {Status::Invalid, "too many competing headers"};
    }

    Node node;
    node.header = header;
    node.hash = hash;
    node.height = parent->height + 1;
    U256 work{parent->work[0], parent->work[1], parent->work[2], parent->work[3]};
    if (AddInPlace(work, BlockProof(header.bits))) return {Status::Invalid, "chain work overflow"};
    std::copy(work.begin(), work.end(), node.work);
    const Node* tip = Find(TipHash());
    const U256 tip_work{tip->work[0], tip->work[1], tip->work[2], tip->work[3]};
    nodes_.emplace(hash, node);
    if (Compare(work, tip_work) > 0) {
        Reorganize(hash);
        DropStaleSideHeaders();
    }
    return {Status::Accepted, ""};
}

void BtcHeaderChain::Reorganize(const Bytes32& new_tip) {
    std::vector<Bytes32> path;  // new tip back to (not including) the fork point
    const Node* walk = Find(new_tip);
    while (walk) {
        const size_t index = walk->height - params_.checkpoint_height;
        if (index < best_.size() && best_[index] == walk->hash) break;  // the fork point
        path.push_back(walk->hash);
        walk = Find(walk->header.prev_hash);
    }
    if (!walk) return;  // cannot happen: every node descends from the checkpoint
    best_.resize(walk->height - params_.checkpoint_height + 1);
    best_.insert(best_.end(), path.rbegin(), path.rend());
}

void BtcHeaderChain::DropStaleSideHeaders() {
    if (side_header_count() == 0) return;
    const uint32_t tip = TipHeight();
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        const Node& n = it->second;
        const size_t index = n.height - params_.checkpoint_height;
        const bool on_best = index < best_.size() && best_[index] == n.hash;
        if (!on_best && n.height + kBtcMaxReorgDepth < tip) {
            it = nodes_.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t BtcHeaderChain::TipHeight() const {
    return params_.checkpoint_height + static_cast<uint32_t>(best_.size() - 1);
}
Bytes32 BtcHeaderChain::TipHash() const { return best_.back(); }
const BtcBlockHeader& BtcHeaderChain::Tip() const { return Find(best_.back())->header; }

std::optional<BtcBlockHeader> BtcHeaderChain::AtHeight(uint32_t height) const {
    if (height < params_.checkpoint_height || height > TipHeight()) return std::nullopt;
    return Find(best_[height - params_.checkpoint_height])->header;
}

std::optional<uint32_t> BtcHeaderChain::HeightOf(const Bytes32& block_hash) const {
    const Node* n = Find(block_hash);
    if (!n) return std::nullopt;
    const size_t index = n->height - params_.checkpoint_height;
    if (index >= best_.size() || best_[index] != n->hash) return std::nullopt;
    return n->height;
}

uint32_t BtcHeaderChain::Confirmations(const Bytes32& block_hash) const {
    const auto height = HeightOf(block_hash);
    return height ? TipHeight() - *height + 1 : 0;
}

std::optional<uint32_t> BtcHeaderChain::MedianTimePast() const {
    const Node* tip = Find(best_.back());
    std::vector<uint32_t> times;
    for (size_t i = best_.size(); i > 0 && times.size() < kBtcMedianTimeSpan; --i) {
        times.push_back(Find(best_[i - 1])->header.time);
    }
    (void)tip;
    if (times.size() < kBtcMedianTimeSpan) return std::nullopt;
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

std::vector<Bytes32> BtcHeaderChain::Locator() const {
    std::vector<Bytes32> out;
    size_t index = best_.size() - 1, step = 1;
    while (true) {
        out.push_back(best_[index]);
        if (index == 0) break;
        if (out.size() >= 10) step *= 2;
        index = index > step ? index - step : 0;
    }
    return out;
}

std::vector<uint8_t> BtcHeaderChain::SerializeBestChain() const {
    std::vector<uint8_t> out;
    out.reserve(80 * (best_.size() - 1));
    for (size_t i = 1; i < best_.size(); ++i) {
        const auto raw = SerializeBtcBlockHeader(Find(best_[i])->header);
        out.insert(out.end(), raw.begin(), raw.end());
    }
    return out;
}

size_t BtcHeaderChain::Adopt(const BtcHeaderChain& other, uint32_t now_unix) {
    size_t accepted = 0;
    for (size_t i = 1; i < other.best_.size(); ++i) {
        if (Find(other.best_[i])) continue;
        const Status status = Accept(other.Find(other.best_[i])->header, now_unix).status;
        if (status == Status::Accepted) ++accepted;
        else if (status != Status::Duplicate) break;
    }
    return accepted;
}

size_t BtcHeaderChain::LoadBestChain(const std::vector<uint8_t>& bytes, uint32_t now_unix) {
    size_t accepted = 0;
    for (size_t offset = 0; offset + 80 <= bytes.size(); offset += 80) {
        const auto header = ParseBtcBlockHeader({bytes.begin() + offset, bytes.begin() + offset + 80});
        if (!header || Accept(*header, now_unix).status != Status::Accepted) break;
        ++accepted;
    }
    return accepted;
}

}  // namespace dinero::swap

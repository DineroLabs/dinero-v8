#include "compact_spartan_codec.h"
#include "zk/zkvm/r1cs_spartan.h"
#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace dinero::experimental {
namespace {
constexpr std::array<uint8_t, 4> kTag{'D', 'Z', 'E', '1'};
// Prototype ceiling, not a consensus parameter. Current circuits are below it.
// Bounding trusted dimensions too keeps every size/offset calculation small.
constexpr size_t kMaxCircuitEntries = 1u << 22;
size_t LogCeil(size_t n) { return std::bit_width(n - 1); }
size_t EvaluationBytes(size_t rounds) { return 32 + 4 + rounds * 66 + 64; }

class Reader {
  public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    bool Skip(size_t n) {
        if (n > bytes_.size() - at_)
            return false;
        at_ += n;
        return true;
    }
    bool Number(size_t n, uint64_t expected) {
        if (n > bytes_.size() - at_)
            return false;
        uint64_t value = 0;
        for (size_t i = 0; i < n; ++i)
            value = (value << 8) | bytes_[at_++];
        return value == expected;
    }
    bool Match(std::span<const uint8_t> expected) {
        if (expected.size() > bytes_.size() - at_)
            return false;
        const bool equal = std::equal(expected.begin(), expected.end(), bytes_.begin() + at_);
        at_ += expected.size();
        return equal;
    }
    bool Done() const { return at_ == bytes_.size(); }

  private:
    std::span<const uint8_t> bytes_;
    size_t at_ = 0;
};
} // namespace

CompactSpartanCodec::CompactSpartanCodec(uint8_t version, const zk::zkvm::R1CS &circuit)
    : version_(version) {
    const auto n = circuit.num_variables(), m = circuit.num_constraints();
    if ((version != 4 && version != 6) || n == 0 || m == 0 || n > kMaxCircuitEntries ||
        m > kMaxCircuitEntries)
        throw std::invalid_argument("unsupported compact prototype circuit/profile");
    witness_ = zk::zkvm::HyraxParams::from_n(n);
    error_ = zk::zkvm::HyraxParams::from_n(m);
    outer_rounds_ = LogCeil(m);
    inner_rounds_ = LogCeil(n);
    witness_rounds_ = LogCeil(witness_.n_cols);
    error_rounds_ = LogCeil(error_.n_cols);
    error_offset_ = 1 + 24 + 33 * witness_.n_rows + 24;
    expanded_size_ = error_offset_ + 33 * error_.n_rows + 32 + 8 + 128 * outer_rounds_ + 128 + 8 +
                     96 * inner_rounds_ + EvaluationBytes(witness_rounds_) +
                     EvaluationBytes(error_rounds_);
    circuit_hash_ = zk::zkvm::spartan_hash_r1cs_structure(circuit);
}

bool CompactSpartanCodec::Check(std::span<const uint8_t> proof, bool compact) const {
    // Exact length, all dimensions, all round counts and circuit identity are
    // checked before ANY buffer allocation or call to the historical parser.
    const size_t expected = expanded_size_ - (compact ? 33 * error_.n_rows : 0);
    if (proof.size() != expected)
        return false;
    Reader reader(proof);
    const auto dimensions = [&](const zk::zkvm::HyraxParams &h) {
        return reader.Number(8, h.n_rows) && reader.Number(8, h.n_cols) &&
               reader.Number(8, h.n_total);
    };
    const auto evaluation = [&](size_t rounds) {
        return reader.Skip(32) && reader.Number(4, rounds) && reader.Skip(rounds * 66 + 64);
    };
    if (!reader.Number(1, version_) || !dimensions(witness_) ||
        !reader.Skip(33 * witness_.n_rows) || !dimensions(error_))
        return false;
    if (!compact) {
        const auto rows = proof.subspan(error_offset_, 33 * error_.n_rows);
        if (!std::all_of(rows.begin(), rows.end(), [](uint8_t byte) { return byte == 0; }))
            return false;
        if (!reader.Skip(rows.size()))
            return false;
    }
    return reader.Match(circuit_hash_) && reader.Number(8, outer_rounds_) &&
           reader.Skip(128 * outer_rounds_ + 128) && reader.Number(8, inner_rounds_) &&
           reader.Skip(96 * inner_rounds_) && evaluation(witness_rounds_) &&
           evaluation(error_rounds_) && reader.Done();
}

std::optional<std::vector<uint8_t>>
CompactSpartanCodec::Pack(std::span<const uint8_t> proof) const {
    if (!Check(proof, false))
        return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(kTag.size() + expanded_size_ - 33 * error_.n_rows);
    out.insert(out.end(), kTag.begin(), kTag.end());
    out.insert(out.end(), proof.begin(), proof.begin() + error_offset_);
    out.insert(out.end(), proof.begin() + error_offset_ + 33 * error_.n_rows, proof.end());
    return out;
}

std::optional<std::vector<uint8_t>>
CompactSpartanCodec::Expand(std::span<const uint8_t> compact) const {
    if (compact.size() < kTag.size() || !std::equal(kTag.begin(), kTag.end(), compact.begin()))
        return std::nullopt;
    const auto payload = compact.subspan(kTag.size());
    if (!Check(payload, true))
        return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(expanded_size_);
    out.insert(out.end(), payload.begin(), payload.begin() + error_offset_);
    out.insert(out.end(), 33 * error_.n_rows, 0);
    out.insert(out.end(), payload.begin() + error_offset_, payload.end());
    return out;
}
} // namespace dinero::experimental

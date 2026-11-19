#include "wallet/swap/btc_raw_tx.h"

#include "crypto/sha256.h"

namespace dinero::swap {
namespace {

// Bounds-checked reader: every failure sets `ok` to false and yields zeros.
struct Reader {
    const std::vector<uint8_t>& data;
    size_t pos{0};
    bool ok{true};

    size_t Left() const { return data.size() - pos; }
    uint64_t Le(size_t n) {
        if (!ok || Left() < n) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
        pos += n;
        return v;
    }
    // Bitcoin's CompactSize; a longer encoding than needed is refused, as Bitcoin does.
    uint64_t Compact() {
        const uint64_t first = Le(1);
        if (first < 0xfd) return first;
        const size_t width = first == 0xfd ? 2 : first == 0xfe ? 4 : 8;
        const uint64_t v = Le(width);
        const uint64_t least = width == 2 ? 0xfd : width == 4 ? 0x10000 : 0x100000000ULL;
        if (v < least) ok = false;
        return v;
    }
    std::vector<uint8_t> Bytes(uint64_t n) {
        if (!ok || n > Left()) {
            ok = false;
            return {};
        }
        std::vector<uint8_t> out(data.begin() + pos, data.begin() + pos + n);
        pos += n;
        return out;
    }
};

}  // namespace

std::optional<BtcRawTx> ParseBtcRawTx(const std::vector<uint8_t>& raw) {
    if (raw.size() < 10 || raw.size() > kBtcMaxRawTxSize) return std::nullopt;
    Reader r{raw};
    BtcRawTx tx;
    tx.version = static_cast<int32_t>(r.Le(4));

    // BIP144: a zero where the input count would be, then the flag 0x01.
    if (r.Left() >= 2 && raw[r.pos] == 0x00) {
        if (raw[r.pos + 1] != 0x01) return std::nullopt;
        tx.has_witness = true;
        r.pos += 2;
    }
    const size_t inputs_begin = r.pos;

    const uint64_t input_count = r.Compact();
    if (!r.ok || input_count == 0 || input_count > r.Left() / 41) return std::nullopt;  // an input is at least 41 bytes
    tx.vin.resize(input_count);
    for (auto& in : tx.vin) {
        const auto prev = r.Bytes(32);
        if (!r.ok) return std::nullopt;
        std::copy(prev.begin(), prev.end(), in.prev_txid.begin());
        in.prev_vout = static_cast<uint32_t>(r.Le(4));
        in.script_sig = r.Bytes(r.Compact());
        in.sequence = static_cast<uint32_t>(r.Le(4));
        if (!r.ok) return std::nullopt;
    }
    const uint64_t output_count = r.Compact();
    if (!r.ok || output_count > r.Left() / 9) return std::nullopt;  // an output is at least 9 bytes
    tx.vout.resize(output_count);
    for (auto& out : tx.vout) {
        out.value_sat = r.Le(8);
        out.script_pubkey = r.Bytes(r.Compact());
        if (!r.ok) return std::nullopt;
    }
    const size_t outputs_end = r.pos;

    if (tx.has_witness) {
        bool any = false;
        for (auto& in : tx.vin) {
            const uint64_t items = r.Compact();
            if (!r.ok || items > r.Left()) return std::nullopt;  // an item is at least one byte
            in.witness.resize(items);
            for (auto& item : in.witness) {
                item = r.Bytes(r.Compact());
                if (!r.ok) return std::nullopt;
            }
            any = any || items != 0;
        }
        if (!any) return std::nullopt;  // Bitcoin refuses a witness flag with no witness
    }
    tx.locktime = static_cast<uint32_t>(r.Le(4));
    if (!r.ok || r.pos != raw.size()) return std::nullopt;

    // The id covers version, inputs, outputs and locktime: everything but the
    // marker, the flag and the witness.
    std::vector<uint8_t> stripped;
    stripped.reserve(4 + (outputs_end - inputs_begin) + 4);
    stripped.insert(stripped.end(), raw.begin(), raw.begin() + 4);
    stripped.insert(stripped.end(), raw.begin() + inputs_begin, raw.begin() + outputs_end);
    stripped.insert(stripped.end(), raw.end() - 4, raw.end());
    tx.stripped_size = stripped.size();
    Bytes32 once{};
    crypto::CSHA256().Write(stripped.data(), stripped.size()).Finalize(once.data());
    crypto::CSHA256().Write(once.data(), once.size()).Finalize(tx.txid.data());
    return tx;
}

}  // namespace dinero::swap

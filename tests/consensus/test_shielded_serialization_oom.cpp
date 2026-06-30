// Regression test: untrusted CompactSize counts in DeserializeShieldedBundle
// must be capped against remaining buffer bytes BEFORE resize(), otherwise a
// remote peer can trigger an unbounded allocation (remote OOM / length_error).
//
// Discriminator: a buffer that declares num_spends / num_outputs = 2^64-1 with
// no backing data. WITHOUT the fix, resize() is called with 2^64-1 and throws
// std::length_error (or aborts). WITH the fix, the count is rejected as
// Truncated before any allocation.
//
// Exit non-zero on failure (does NOT rely on assert()).

#include "consensus/shielded/shielded_serialization.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

using namespace dinero::consensus::shielded;

namespace {

void push_u64_le(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

// Append a CompactSize encoding of 2^64-1 (0xFF prefix + 8 bytes of 0xFF).
void push_compact_u64max(std::vector<uint8_t>& b) {
    b.push_back(0xFF);
    for (int i = 0; i < 8; ++i) b.push_back(0xFF);
}

int run_case(const char* name, const std::vector<uint8_t>& buf) {
    ShieldedBundle out;
    BundleDecodeError err = BundleDecodeError::Ok;
    try {
        err = DeserializeShieldedBundle(buf, &out);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "FAIL[%s]: deserialize threw (unbounded resize not capped): %s\n",
                     name, e.what());
        return 1;
    }
    if (err != BundleDecodeError::Truncated) {
        std::fprintf(stderr, "FAIL[%s]: expected Truncated, got %d\n",
                     name, static_cast<int>(err));
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    int rc = 0;

    // Case 1: oversized num_spends.
    {
        std::vector<uint8_t> buf;
        push_u64_le(buf, 0);          // value_balance
        push_compact_u64max(buf);     // num_spends = 2^64-1, no backing data
        rc |= run_case("num_spends", buf);
    }

    // Case 2: oversized num_outputs (num_spends = 0).
    {
        std::vector<uint8_t> buf;
        push_u64_le(buf, 0);          // value_balance
        buf.push_back(0x00);          // num_spends = 0
        push_compact_u64max(buf);     // num_outputs = 2^64-1, no backing data
        rc |= run_case("num_outputs", buf);
    }

    if (rc == 0) {
        std::printf("PASS: oversized shielded-bundle counts rejected as Truncated\n");
    }
    return rc;
}

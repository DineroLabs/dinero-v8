// Deterministic vector dumper for the shielded state commitment.
//
// Purpose: prove the encoding and digests are identical across compilers
// (GCC vs Clang) and configurations (Debug vs Release). Consensus values that
// differ by toolchain would fork the network along build lines, and that class
// of bug is invisible to a test suite that only ever runs one build.
//
// This has no test framework on purpose. It prints bytes. Two builds either
// emit an identical file or they do not, and `cmp` is the assertion.
//
// Real precedent for the concern on this very code: NullifierEntry's
// operator== had to be written out longhand because a defaulted one is a hard
// error under the C++17 Linux/gcc build while compiling cleanly on macOS.
// Same header, same source, different toolchain, different outcome.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "consensus/shielded/nullifier_accumulator.h"
#include "consensus/shielded/shielded_root.h"
#include "consensus/state_commitment.h"
#include "primitives/uint256.h"

namespace sh = dinero::consensus::shielded;

namespace {

std::string Hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(v.size() * 2);
    for (uint8_t b : v) {
        out.push_back(d[b >> 4]);
        out.push_back(d[b & 0x0f]);
    }
    return out;
}

// Deterministic filler: no RNG, no time, no addresses.
std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>((seed + i * 31u + (i >> 3)) & 0xff);
    }
    return v;
}

dinero::uint256 U256(uint8_t seed) {
    dinero::uint256 h;
    const auto p = Pattern(32, seed);
    std::memcpy(h.data, p.data(), 32);
    return h;
}

sh::NullifierEntry Entry(uint32_t height, uint8_t seed) {
    sh::NullifierEntry e;
    e.height = height;
    const auto p = Pattern(32, seed);
    for (size_t i = 0; i < 32; ++i) e.nullifier[i] = p[i];
    return e;
}

void EmitRoot(const char* name,
              const std::vector<uint8_t>& tree_root,
              uint64_t tree_size,
              const dinero::uint256& nacc,
              const std::vector<uint8_t>& anchor) {
    const auto pre = sh::BuildShieldedRootPreimage(tree_root, tree_size, nacc, anchor);
    const auto dig = sh::ComputeShieldedRootFromParts(tree_root, tree_size, nacc, anchor);
    std::printf("root %-28s preimage_len=%zu\n", name, pre.size());
    std::printf("root %-28s preimage=%s\n", name, Hex(pre).c_str());
    std::printf("root %-28s digest=%s\n", name, dig.GetHex().c_str());
}

void EmitAcc(const char* name, std::vector<sh::NullifierEntry> entries) {
    const auto d = sh::ComputeNullifierAccumulator(entries);
    std::printf("acc  %-28s n=%zu digest=%s\n", name, entries.size(), d.GetHex().c_str());
}

}  // namespace

int main() {
    std::printf("# shielded state commitment — deterministic vectors\n");
    std::printf("# tag=%c%c%c%c version=%u\n",
                sh::SHIELDED_ROOT_TAG[0], sh::SHIELDED_ROOT_TAG[1],
                sh::SHIELDED_ROOT_TAG[2], sh::SHIELDED_ROOT_TAG[3],
                static_cast<unsigned>(sh::SHIELDED_ROOT_VERSION));

    // --- shielded root: structural edges -----------------------------------
    EmitRoot("empty_all", {}, 0, dinero::uint256(), {});
    EmitRoot("empty_tree_nonzero_size", {}, 1, dinero::uint256(), {});
    EmitRoot("tree32_size0", Pattern(32, 0x11), 0, U256(0x22), {});
    EmitRoot("tree32_size1", Pattern(32, 0x11), 1, U256(0x22), {});
    EmitRoot("typical", Pattern(32, 0xA0), 61000, U256(0xB0), Pattern(64, 0xC0));

    // Length/endianness boundaries — a size field written in the wrong width or
    // byte order shows up here and nowhere else.
    EmitRoot("size_0x7f",       Pattern(32, 1), 0x7full,               U256(2), Pattern(8, 3));
    EmitRoot("size_0xff",       Pattern(32, 1), 0xffull,               U256(2), Pattern(8, 3));
    EmitRoot("size_0x100",      Pattern(32, 1), 0x100ull,              U256(2), Pattern(8, 3));
    EmitRoot("size_0xffffffff", Pattern(32, 1), 0xffffffffull,         U256(2), Pattern(8, 3));
    EmitRoot("size_2pow32",     Pattern(32, 1), 0x100000000ull,        U256(2), Pattern(8, 3));
    EmitRoot("size_max",        Pattern(32, 1), 0xffffffffffffffffull, U256(2), Pattern(8, 3));

    // Anchor length boundaries.
    for (size_t n : {size_t{0}, size_t{1}, size_t{31}, size_t{32}, size_t{33}, size_t{255}, size_t{256}}) {
        char nm[64];
        std::snprintf(nm, sizeof(nm), "anchor_len_%zu", n);
        EmitRoot(nm, Pattern(32, 0x40), 7, U256(0x50), Pattern(n, 0x60));
    }

    // Non-32-byte tree roots: the encoding must be unambiguous about length.
    for (size_t n : {size_t{1}, size_t{31}, size_t{33}, size_t{64}}) {
        char nm[64];
        std::snprintf(nm, sizeof(nm), "tree_len_%zu", n);
        EmitRoot(nm, Pattern(n, 0x70), 3, U256(0x80), Pattern(4, 0x90));
    }

    // --- nullifier accumulator --------------------------------------------
    EmitAcc("empty", {});
    EmitAcc("single", {Entry(1, 0x01)});
    EmitAcc("two_sorted", {Entry(1, 0x01), Entry(2, 0x02)});
    // Must equal two_sorted: the accumulator sorts internally.
    EmitAcc("two_reversed", {Entry(2, 0x02), Entry(1, 0x01)});
    // Must equal single: duplicates are collapsed.
    EmitAcc("dup_same", {Entry(1, 0x01), Entry(1, 0x01)});
    // Same nullifier at different heights is NOT a duplicate.
    EmitAcc("same_nf_diff_height", {Entry(1, 0x01), Entry(2, 0x01)});
    EmitAcc("height_zero", {Entry(0, 0x05)});
    EmitAcc("height_max", {Entry(0xffffffffu, 0x05)});
    EmitAcc("height_boundary_61000", {Entry(60999, 0x06), Entry(61000, 0x06), Entry(61001, 0x06)});

    {
        std::vector<sh::NullifierEntry> many;
        many.reserve(256);
        for (uint32_t i = 0; i < 256; ++i) {
            many.push_back(Entry(i, static_cast<uint8_t>(i)));
        }
        EmitAcc("many_256_ascending", many);
        std::vector<sh::NullifierEntry> rev(many.rbegin(), many.rend());
        EmitAcc("many_256_descending", rev);  // must equal ascending
    }

    // --- coinbase encoding (state_commitment_v1) ---------------------------
    // In the determinism matrix too: the script is consensus data, so an
    // architecture-dependent encoding would fork the chain exactly as an
    // architecture-dependent digest would.
    for (uint8_t seed : {0x00, 0x01, 0x7f, 0x80, 0xff}) {
        dinero::uint256 r;
        const auto p = Pattern(32, seed);
        std::memcpy(r.data, p.data(), 32);
        const auto script = dinero::consensus::BuildStateCommitmentScript(r);
        std::printf("cbsc seed_%02x                     len=%zu script=%s\n",
                    seed, script.size(), Hex(script).c_str());
    }

    std::printf("# end\n");
    return 0;
}

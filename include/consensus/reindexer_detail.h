// Copyright (c) 2026 Dinero Labs.
//
// Internals of the block reindexer, exposed for testing.
//
// SelectCanonicalChain used to live in reindexer.cpp's anonymous namespace and
// therefore could not be tested directly. It is the step that crashed the
// daemon on any mainnet-sized datadir (issue #708): it resolved parent links by
// recursing once per block, so at ~107,750 blocks the descent from tip to
// genesis overflowed the default 8 MB stack before reindex did any work at all.
//
// A regression test for that has to call this function with a chain deeper than
// the stack can absorb. Nothing else outside the reindexer should depend on
// this header.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common/status.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "storage/block_storage.h"  // FilePosition

namespace dinero {
namespace consensus {
namespace reindex_detail {

// One block as recovered from a blk*.dat scan.
struct DiskBlockRecord {
    Block block;
    FilePosition pos;
    uint256 hash;
    uint256 prev_hash;
};

// Select the canonical chain from a parsed records vector.
//
// Empty `known_tip_hash_hex`: legacy chainwork-search — resolve every record
// against parent-hash links, pick the highest-chainwork connected tip, walk
// back to genesis. Non-empty: anchored mode — start the backward walk from
// that hash and skip the search entirely.
//
// Returns record indices in genesis-to-tip order.
//
// Stack usage is O(1) in chain height: the parent-link resolution is iterative.
// Do not reintroduce recursion here — see #708.
StatusOr<std::vector<size_t>> SelectCanonicalChain(
    const std::vector<DiskBlockRecord>& records,
    const std::string& known_tip_hash_hex = std::string());

}  // namespace reindex_detail
}  // namespace consensus
}  // namespace dinero

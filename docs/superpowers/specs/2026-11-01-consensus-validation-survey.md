# Consensus validation — Phase A survey (CV1 header reach, baseline c4940d86c, 2026-11-01)

Read-only. Nothing was built except a configure-only tree, ~/src/dinero-sep-builds/consensus-off.
Raw inputs: header-reach-*-{off,on}.json (preprocessor -MM -MG over every object of the target; configure-only
trees consensus-off and consensus-on, 0 preprocess errors, 0 unresolved deps; positive control
coins_db.cpp->rocksdb PASS, negative control pow.cpp->rocksdb FAIL).

> Scope note (2026-11-02): this public draft covers **CV1 header reach only**. The live-rule inventory and
> caller/duplicate-object analysis that motivated the other gates are tracked privately and are not part of this
> PR. This PR is report-only and **do not merge**.

## 1. Summary
Leg 1a (the wallet session's Stages 1-3) makes the `dinero_consensus` **library** storage-free. CV1 measures
that boundary directly: the preprocessor `#include` reach of every object in `dinero_consensus` and
`dinero_consensus_core`, failing on any forbidden family (storage/daemon/p2p/net/rpc/wallet/mining/indexing/
mempool/node/chainstate/core, plus RocksDB and sqlite3) outside an allowlist.

## 2. Header reach (measured, OFF and ON; results identical)
- dinero_consensus (53 objects) reaches 77 forbidden headers (storage/daemon/p2p/wallet/indexing/mining + RocksDB + sqlite3).
  RocksDB: coins_db, coins_view_cache, global_utxo_set_impl, tx_validation, utxo_position_index. sqlite3: addr_codec,
  global_utxo_set_impl. This matches leg 1a's own section 6.1 table exactly, an independent confirmation by preprocessor.
  Every one of them except two comes only from global_utxo_set_impl.cpp, utxo_position_index.cpp and
  addr_codec.cpp, all of which leg 1a moves out (Tasks 2.2-2.4).
- What's left after leg 1a: include/mining/header_layout.h (15 objects) and include/storage/tip_info.h
  (serialization.cpp). Both are pure (std headers plus consensus/primitives only), so the problem is
  where they live, not what they depend on.
- dinero_consensus_core (30 objects): only those same two pure headers.
- The 18 include/consensus/*.h files that include storage/p2p/daemon headers are NOT reached by either
  library. They belong to dinero_core, dinero_chainstate or the Orchard libraries, so "consensus" is a
  directory name there, not a layer.
- GATE GAP addressed by CV1: the existing K2 check uses link closure and include *directories*
  (FORBIDDEN_INCLUDE), not `#include` lines, so a header-only leak through the umbrella include/ directory goes
  unseen. CV1 (scripts/ci/consensus-separation/header_reach.py) measures preprocessor header reach for the
  consensus objects and fails on any forbidden family outside the allowlist.

## 3. Bucket rule (reported, never failing)
Reaches from the sources leg 1a moves or changes (coins_db, coins_view_cache, global_utxo_set_impl,
utxo_position_index, addr_codec, tx_validation) are bucket A: reported, never failing, and expected to disappear
as leg 1a lands. The baseline shrinks as they go, and a shrink is a reported improvement, not a failure.
`include/mining/header_layout.h` and `include/storage/tip_info.h` are allowlisted as pure; moving them is a
follow-up.

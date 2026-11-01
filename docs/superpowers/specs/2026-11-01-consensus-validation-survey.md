# Consensus validation — Phase A survey (baseline c4940d86c, 2026-11-01)

Read-only. Nothing was built except a configure-only tree, ~/src/dinero-sep-builds/consensus-off.
Raw inputs: header-reach-*-{off,on}.json (preprocessor -MM -MG over every object of the target; configure-only
trees consensus-off and consensus-on, 0 preprocess errors, 0 unresolved deps; positive control coins_db.cpp->rocksdb PASS,
negative control pow.cpp->rocksdb FAIL);
the rule-placement trace (subagent, file:line throughout; items marked VERIFIED were re-read by Claude).

## 1. Summary
Leg 1a (the wallet session's Stages 1-3) makes the dinero_consensus **library** storage-free. That is
necessary, but it leaves most consensus **rules** where they are: the live block rules do not run in that
library. They run in dinero_core, mixed with ChainstateService, ChainDB, HeaderChain locks, DaemonContext
and global Params(). The pure validators in dinero_consensus are mostly not on the live path.

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
- GATE GAP: K2 checks link closure and include *directories* (FORBIDDEN_INCLUDE), not #include
  lines, so a header-only leak through the umbrella include/ directory goes unseen. Proposed gate C1:
  preprocessor header reach for the consensus objects, failing on any forbidden family outside an
  allowlist. Prototype: scratchpad header_reach.py, to move to scripts/ci/consensus-separation/.

## 3. Where the live rules run (legacy path, before Orchard activation at height 150000)
Entry: BlockAcceptor::AcceptBlockFromRPC (src/daemon/block_acceptor.cpp:122). The P2P path re-hexes the
block into it (:737 -> :807). Then ActivateBestChain -> ChainstateService::ConnectTip
(chainstate_service.cpp:23080) -> BlockValidator::ConnectBlockInternal (src/consensus/block_validation.cpp:911).
**Every one of these is compiled into dinero_core** (CMakeLists.txt:1063 ff.; block_validation.cpp :1150).

| Rule | Live site | Context it reads |
|---|---|---|
| ASERT bits | BlockAcceptor::ValidateProofOfWork :1080 | DaemonContext, ChainstateService, the HeaderChain lock, ChainDB |
| PoW <= target | inline at block_acceptor :1204 (does not use CheckProofOfWork in pow.cpp) | Params() |
| MTP | ValidateTimestamp :1250 | BlockIndex -> HeaderChain -> ChainDB |
| merkle / CVE-2012-2459 | ValidateMerkleRoot :1473 | pure |
| block sigops | CheckBlockSigops (sigops.cpp:190), called from block_acceptor :1521 | — |
| coinbase reward | CheckCoinbaseReward (block_reward.cpp:76), called from block_validation :1768 | Params() |
| tx inputs/maturity | BlockValidator::ValidateTransaction :2908 | IConsensusUTXOSet, plus a global FindBlockIndex fallback for MTP (:3033) |
| witness commitment | EnforceWitnessCommitment (witness_commitment.cpp:205), called at :1069 | pure given params |
| DNRF filter | presence: chainstate_service :23325; hash: :24608 | ChainstateService |
| activation heights | ~10 sites, all through global Params()/GetActiveChain() | DNRF activation is a compile-time constant (filter_commitment.h:49) |

Orchard path (>=150000; release builds are ON): the header check is in dinero_orchard_header, which
PRIVATE-links dinero_core. The body/coins/transparent rules use ChainStateView or plain structs.
Staging, state root and legacy accounting take ChainDB& and BlockStorage*, and orchard_block_staging.cpp:34
defines ChainDB::stageOrchardBlock.

## 4. Parallel and dead rule sets
- Live validators: BlockValidator (connect), TransactionValidator (P2P pre-check against the TIP under
  readLock, validation_queue.cpp:534-550, with maturity hard-coded to 100), and reindexer.cpp:308/2333+
  (its own fee path, reading chain_db_).
- Not live: tx_validation validateTransaction/validateInputs (only caller is miner_engine.cpp, which is in
  no CMake list), VerifyBlockStateless, PowConsensusEngine::ValidateBlock (the engine is constructed at
  daemon_app.cpp:2860 and used for CreateBlockTemplate, mining_service.cpp:285, but no ValidateBlock call exists through
  any interface; only tests/consensus/test_consensus_validation_tripwires.cpp; the comment at header_chain.cpp:1029 says otherwise),
  BlockValidator::GetBlockSubsidy.
  => NOTE for wallet Task 2.6 (verified by grep, qualified and unqualified, over src/include/tests/tools/wallet-core):
  the free functions validateTransaction/validateInputs/checkSequenceLocks have NO compiled caller. The only call is
  miner_engine.cpp:527 (in no CMake list); every other hit is a member function with the same name, or a comment.
- Coinbase maturity is written 4 ways. src/core/consensus/coinbase_maturity.cpp is dead and off by one
  (current-cb+1). The live CoinbaseMaturity is used only by the mempool and the wallet.
- Compiled into two libraries (ODR risk, not built to confirm): block_index.cpp and undo.cpp (core +
  consensus); state_commitment.cpp (core + orchard_block_staging); snapshot_binding, utxo_set_digest
  (core + orchard_snapshot). This is the same class as B3 (LNK2005 on Windows).
- src/consensus_core/CMakeLists.txt is never included, so it is dead. The live dinero_consensus_core is
  src/consensus/CMakeLists.txt:29 (30 sources, a subset of consensus plus difficulty_validation.cpp).
  Its consumers are the Orchard transaction_reader and state_root, tools, and a few tests.

## 5. Options for this project (for owner)
A. Gate first (no behavior change): C1 header-reach gate; an inventory/anchor file for the live rule
   sites above; pin "which validator is live" with tests. Same pattern as the siblings' Stage 0.
B. Rule extraction (later, staged): a pure `CheckBlock(block, params, height)` and a
   `ContextualCheckBlock(block, ChainStateView-like ctx)` in dinero_consensus, called from
   BlockAcceptor/BlockValidator with identical results. Proven by differential tests over historical
   blocks before switching. Every step that moves a frozen file (block_acceptor.cpp is frozen) is a
   CONSENSUS CHANGE stop point.
C. Pass params instead of reading Params() at rule sites: deferred, the largest item.
Depends on: leg 1a (library storage-free); X4 Coin type; mempool F1 (unguarded chain reads).

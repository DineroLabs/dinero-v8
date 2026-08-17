#include "rpc/methods_mining_extras.h"
#include "rpc/rpc_registry.h"
#include "din_json.h"
#include "storage/chain_direct.h"
#include "bech32_encoder.h"
#include "common/logger.h"
#include "common/sha256d.h"  // For Dinero::Common::double_sha256
#include "consensus/consensus.hpp"
#include "consensus/block_filter.h"
#include "consensus/filter_commitment.h"
#include "consensus/outpoint.h"
#include "consensus/subsidy.h"  // Canonical monetary policy
#include "consensus/utreexo_accumulator.h"  // For UtreexoForest
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "consensus/chainparams.h"
#include "consensus/pow.hpp"
#include "consensus/merkle_root.h"  // For ComputeMerkleRoot (Phase 11a.2)
#include "primitives/block.h"
// Note: Transaction is already included via primitives/block.h -> wallet/transaction.h
#include "daemon/block_acceptor.h"
#include "daemon/daemon_context.h"  // For DaemonContext access
#include "daemon/services/chainstate_service.h"  // For BlockValidator access
#include "consensus/block_validation.h"  // For ComputeUtreexoRootPure
#include "daemon/services/mining_service.h"  // For MiningService wrapper
#include "daemon/services/mempool_service.h"  // For mempool transaction selection
#include "storage/archival_block_reader.h"
#include "crypto/sha256.h"
#include "mining/mining_script_override.h"  // Global mining override
#include "mining/block_assembler.h"
#include "mining/address_validator.h"  // Taproot-only mining policy (Phase 26.4)
#include "script/ctcommit.h"  // CT script builder
#include "util/hex.h"  // For HexToBytes
#include <json/json.h>
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <atomic>   // #458: coinbase extranonce counter
#include <cstdint>   // #458: extranonce session id
#include <openssl/rand.h>   // #458: extranonce session id
#include <array>   // #458: extranonce session id

// External globals
extern RpcRegistry g_rpcRegistry;

namespace dinero {
namespace rpc {

namespace {

std::unordered_set<dinero::OutPoint> CollectEphemeralOutputs(const dinero::Block& block) {
    std::unordered_map<dinero::OutPoint, size_t> intra_block_outputs;
    std::unordered_set<dinero::OutPoint> ephemeral_outputs;

    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto txid = block.vtx[tx_idx].GetTxid();
        for (uint32_t vout = 0; vout < block.vtx[tx_idx].vout.size(); ++vout) {
            intra_block_outputs[dinero::OutPoint(txid, vout)] = tx_idx;
        }
    }

    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
        for (const auto& input : block.vtx[tx_idx].vin) {
            const dinero::OutPoint prevout(input.prevout.txid, input.prevout.vout);
            if (intra_block_outputs.count(prevout) != 0) {
                ephemeral_outputs.insert(prevout);
            }
        }
    }

    return ephemeral_outputs;
}

std::unordered_map<dinero::OutPoint, dinero::consensus::SpentOutputData> CollectIntraBlockSpentOutputs(
    const dinero::Block& block,
    uint32_t height
) {
    std::unordered_map<dinero::OutPoint, dinero::consensus::SpentOutputData> outputs;

    for (const auto& tx : block.vtx) {
        const auto txid = tx.GetTxid();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            dinero::consensus::SpentOutputData spent_output;
            spent_output.value = tx.vout[vout].value.GetUna();
            spent_output.scriptPubKey = tx.vout[vout].scriptPubKey;
            spent_output.created_height = height;
            spent_output.is_coinbase = tx.IsCoinbase();
            spent_output.is_confidential = tx.vout[vout].is_confidential;
            spent_output.commitment = tx.vout[vout].commitment;
            outputs.emplace(dinero::OutPoint(txid, vout), std::move(spent_output));
        }
    }

    return outputs;
}

bool WaitForActiveTip(dinero::ChainDB* chain_db,
                      const std::string& expected_hash,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                      std::chrono::milliseconds poll = std::chrono::milliseconds(10)) {
    if (!chain_db) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (dinero::storage::GetBestBlockHash(chain_db) == expected_hash) {
            return true;
        }
        std::this_thread::sleep_for(poll);
    }

    return dinero::storage::GetBestBlockHash(chain_db) == expected_hash;
}

void MaybeAddFilterCommitment(
    dinero::Block& block,
    const std::vector<dinero::Transaction>& mempool_txs,
    dinero::ChainDB* chain_db,
    uint32_t height
) {
    if (!chain_db || block.vtx.empty()) {
        return;
    }

    if (dinero::consensus::FindFilterCommitmentIndex(block.vtx[0]).has_value()) {
        return;
    }

    std::vector<std::vector<uint8_t>> filter_scripts;

    for (const auto& tx : block.vtx) {
        for (const auto& out : tx.vout) {
            if (!out.scriptPubKey.empty() && out.scriptPubKey[0] != 0x6a) {
                filter_scripts.push_back(out.scriptPubKey);
            }
        }
    }

    for (const auto& tx : mempool_txs) {
        for (const auto& in : tx.vin) {
            if (in.prevout.txid.IsNull()) {
                continue;
            }

            auto coin_result = chain_db->getCoin(in.prevout.txid.AsUint256(), in.prevout.vout);
            if (coin_result.status() != dinero::Status::Ok) {
                continue;
            }

            auto script_pubkey = util::HexToBytes(coin_result.value().script_pubkey);
            if (!script_pubkey.empty()) {
                filter_scripts.push_back(std::move(script_pubkey));
            }
        }
    }

    auto filter = dinero::consensus::GCSFilter::Build(filter_scripts, block.header.prev_block_hash);
    if (filter.IsEmpty()) {
        return;
    }

    auto commitment_script = dinero::consensus::BuildFilterCommitmentScript(filter.GetHash());
    if (commitment_script.empty()) {
        return;
    }

    dinero::TxOutput filter_output;
    filter_output.value = dinero::AmountUna::Zero();
    filter_output.scriptPubKey = std::move(commitment_script);
    block.vtx[0].vout.push_back(std::move(filter_output));

    dinero::g_logger.info("[generatetoaddress] Added DNRF filter commitment at height " +
                          std::to_string(height) + " (" +
                          std::to_string(filter.element_count) + " scripts)");
}

}  // namespace

// This file contains the two largest and most complex mining-related RPC methods:
// 1. generatetoaddress (~310 lines) - Mines blocks to address (regtest only)
// 2. getblocktemplate (~220 lines) - Provides block template for external miners

// ═══════════════════════════════════════════════════════════
// generatetoaddress - Mine blocks to specified address (regtest only)
// ═══════════════════════════════════════════════════════════
// ✅ AUTHORITATIVE RPC PATH (VNext)
// This is the ACTIVE implementation registered via registerMiningExtrasMethodsVNext()
// Uses REAL PoW with trivial regtest difficulty (Model B - Bitcoin Core compatible)
//
// Legacy daemon handlers in MiningExtrasHandlers.cpp are DEPRECATED and guarded.
// ═══════════════════════════════════════════════════════════

din::Json handle_generatetoaddress(
    std::shared_ptr<dinero::rpc::MiningState> mining_state,
    dinero::ChainDB* chain_db,
    const MiningExtrasConfig& config,
    const din::Json& params
) {
    din::Json result;
    auto* daemon_ctx = DaemonContext::instance();
    auto* block_storage = daemon_ctx ? daemon_ctx->block_storage.get() : nullptr;

    // Re-enabled for testing P2P block propagation
    try {
        // Validate parameters [nblocks, address]
        if (!params.isArray() || params.size() < 2) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Invalid params: expected [nblocks, address]";
            return result;
        }

        int nblocks = params[0].asInt();
        std::string address = params[1].asString();

        if (nblocks < 1 || nblocks > 1000) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Invalid block count (1-1000)";
            return result;
        }

        // Validate address
        auto decode_result = Bech32Encoder::decode_segwit_address(address);
        if (!decode_result.valid) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Invalid address format";
            return result;
        }

        // ═══════════════════════════════════════════════════════════════════
        // TAPROOT-ONLY MINING POLICY
        // ═══════════════════════════════════════════════════════════════════
        // Dinero mining uses Taproot-only coinbase outputs by policy.
        // Wallets remain fully backward compatible with all address types.
        // Uses centralized IsTaprootAddress() validator (witness v1 + 32 bytes)
        // ═══════════════════════════════════════════════════════════════════
        if (!dinero::mining::IsTaprootAddress(address)) {
            result["error"]["code"] = -5;
            result["error"]["message"] = dinero::mining::GetTaprootRequiredMessage(address);
            return result;
        }

        // Determine address type based on HRP and witness version
        dinero::MiningAddressType addrType = dinero::MiningAddressType::TAPROOT;

        // ✅ OPTION C: Set global mining override (for BlockAssembler or any block builder)
        dinero::g_mining_override_active = true;
        dinero::g_mining_override_witver = decode_result.witness_version;
        dinero::g_mining_override_witprog = decode_result.witness_program;
        dinero::g_mining_override_type = addrType;

        g_logger.info("[RPC OVERRIDE] Applied global mining script override: type=" +
                      std::to_string(static_cast<int>(addrType)) + " ver=" +
                      std::to_string(dinero::g_mining_override_witver) + " len=" +
                      std::to_string(dinero::g_mining_override_witprog.size()) + " bytes");

        // MiningManager v2 integration point:
        // witness override state is applied globally until this path is migrated.
        // Old Mining class was removed in Phase C.
        // auto* ctx = DaemonContext::instance();
        // if (ctx && ctx->mining) {
        //     // MiningManager v2 API needed here
        // }

        din::Json block_hashes = din::arr();

        // Mine each block
        for (int i = 0; i < nblocks; i++) {
            // ✅ CRITICAL: Fetch fresh tip on EVERY iteration
            // Each block must build on the new tip created by the previous block
            uint32_t current_height = dinero::storage::GetChainHeight(chain_db);
            uint32_t height = current_height + 1;
            std::string prevHash = dinero::storage::GetBestBlockHash(chain_db);

            // For height 1, use genesis hash; only genesis (height 0) has 64 zeros
            // GetBestBlockHash should return genesis hash when at height 0
            if (prevHash.empty() || prevHash.length() != 64) {
                // Fallback to genesis hash from chain params
                prevHash = dinero::Params().genesis_hash;
            }

            // Create block
            dinero::Block block;
            block.header.version = 1;

            // ✅ CRITICAL FIX: Zero-initialize reserved field
            // BlockHeader v1 requires reserved[12] to be all zeros (consensus rule).
            // Without this, reserved contains garbage from the stack, which:
            // 1. Gets included in the header hash (different hash each run)
            // 2. Causes "bad-reserved" validation failure on peers
            block.header.ZeroReserved();

            // ✅ CRITICAL FIX: NO endianness conversion needed!
            // GetBestBlockHash() returns GetHex() (big-endian display format)
            // FromHexUnsafe() expects big-endian display format and handles internal conversion
            // Manual byte reversal would create a DIFFERENT hash that doesn't match the tip
            block.header.prev_block_hash = uint256::FromHexUnsafe(prevHash);

            // ✅ TIMESTAMP FIX: Calculate median time past and ensure timestamp is valid
            // Timestamp must be > median time past of last 11 blocks
            auto GetMTP = [chain_db](uint32_t end_height) -> uint32_t {
                std::vector<uint32_t> timestamps;
                timestamps.reserve(11);
                for (int i = 0; i < 11 && end_height >= static_cast<uint32_t>(i); ++i) {
                    auto h_result = chain_db->getBlockHashByHeight(end_height - i);
                    if (h_result.status() != dinero::Status::Ok) break;
                    auto hdr_result = chain_db->getHeader(h_result.value());
                    if (hdr_result.status() != dinero::Status::Ok) break;
                    timestamps.push_back(hdr_result.value().timestamp);
                }
                if (timestamps.empty()) return dinero::Params().genesis.nTime;
                std::sort(timestamps.begin(), timestamps.end());
                return timestamps[timestamps.size() / 2];
            };

            uint32_t prevMTP = (height > 0) ? GetMTP(current_height) : dinero::Params().genesis.nTime;
            uint32_t current_time = static_cast<uint32_t>(std::time(nullptr));
            uint32_t block_time = std::max(prevMTP + 1, current_time);

            block.header.timestamp = static_cast<uint64_t>(block_time);
            block.header.timestamp = block_time;

            const Consensus consensus = GetConsensusForCurrentNetwork();
            const uint32_t bits = GetNextWorkRequiredWithChainDB(
                static_cast<int32_t>(height),
                static_cast<int64_t>(block.header.timestamp),
                consensus,
                chain_db,
                block_storage);
            if (bits == 0) {
                throw std::runtime_error("Cannot compute difficulty for generated block");
            }

            block.header.difficulty = bits;
            block.header.difficulty = bits;
            block.header.nonce = 0;

            // Debug: Confirm difficulty was set correctly
            std::cerr << "[generatetoaddress] Set block.header.difficulty = 0x" << std::hex << block.header.difficulty
                      << " (bits=" << block.header.difficulty << ")" << std::dec << std::endl;

            // Create coinbase transaction
            dinero::Transaction coinbase;
            coinbase.version = 1;
            coinbase.lockTime = 0;

            // Coinbase input with BIP34 height commitment
            dinero::TxInput input;
            input.prevout.txid = dinero::TxId(dinero::uint256());  // Zero hash for coinbase - Phase M.4
            input.prevout.vout = 0xffffffff;

            std::vector<uint8_t> scriptSig;
            // ✅ BIP34 FIX: Proper height encoding with length prefix
            // BIP34 requires: [length_byte] [height_bytes...]
            // Example for height 1: 0x01 0x01 (length=1, value=1)
            if (height >= 1) {
                // Encode height as minimal script number
                std::vector<uint8_t> heightBytes;
                uint32_t n = height;
                while (n > 0) {
                    heightBytes.push_back(n & 0xff);
                    n >>= 8;
                }
                // If MSB is set, add padding byte
                if (!heightBytes.empty() && (heightBytes.back() & 0x80)) {
                    heightBytes.push_back(0x00);
                }

                // Push length prefix, then height bytes
                scriptSig.push_back(static_cast<uint8_t>(heightBytes.size()));
                scriptSig.insert(scriptSig.end(), heightBytes.begin(), heightBytes.end());
            }

            // Add "DIN" tag for identification
            const char* tag = "DIN";
            scriptSig.push_back(3);  // Length of "DIN"
            scriptSig.push_back('D');
            scriptSig.push_back('I');
            scriptSig.push_back('N');

            // Issue #458 — coinbase extranonce, so two templates built on the
            // same parent can never be byte-identical.
            //
            // The nonce search starts at 0 every time, and the timestamp does
            // NOT reliably differ between attempts:
            //
            //     block_time = max(prevMTP + 1, wall clock)
            //
            // Once the chain's median time past has run ahead of wall clock —
            // the normal state in regtest after mining a burst — this collapses
            // to the constant prevMTP + 1. It is then completely independent of
            // wall clock, so waiting does not change it and neither does a
            // restart of any duration.
            //
            // Without an extranonce the same parent therefore reproduces the
            // identical header and identical block hash, which made a
            // rolled-back chain impossible to extend:
            //
            //   mine 16 -> invalidateblock(hash@12) -> height 11
            //   generatetoaddress(9) -> re-mines the SAME hash as the block
            //   just invalidated, which carries BLOCK_FAILED_VALID, so
            //   BlockAcceptor skips AddCandidate ("has persistent
            //   BLOCK_FAILED_VALID flag") and it can never activate.
            //
            // The invalidation guard is correct and is deliberately untouched;
            // the defect was that the miner produced a duplicate rather than a
            // new block.
            //
            // A process-lifetime counter alone is NOT sufficient: it resets to
            // zero on restart, and because block_time can be frozen at
            // prevMTP + 1, a restarted node mining from the same parent would
            // rebuild the invalidated block exactly. The extranonce therefore
            // combines a 128-bit per-process random session id with a 64-bit
            // counter: the session id makes templates distinct ACROSS restarts
            // with computationally negligible collision probability, the
            // counter makes them distinct WITHIN a process.
            //
            // Coinbase scriptSig must be 2..100 bytes (tx_validation.cpp:346).
            // This adds 25 on top of ~6-9, leaving ample headroom. The BIP34
            // height push stays first; trailing bytes are free-form.
            {
                // Drawn once per process from the CSPRNG. Not key material —
                // its only job is to differ across restarts.
                //
                // FAIL CLOSED if the CSPRNG is unavailable. There is no safe
                // fallback: a time/pid/address mix is NOT guaranteed unique
                // across a restart (pid reuse, similar address-space layout and
                // an identical frozen timestamp can reproduce it), and silently
                // mining a duplicate of an invalidated block is worse than not
                // mining at all. Mining availability is not worth knowingly
                // risking a block that can never activate.
                struct ExtranonceSession {
                    std::array<uint8_t, 16> id{};
                    bool ok = false;
                };
                static const ExtranonceSession session = [] {
                    ExtranonceSession s;
                    s.ok = (RAND_bytes(s.id.data(),
                                       static_cast<int>(s.id.size())) == 1);
                    if (!s.ok) {
                        dinero::g_logger.error(
                            "[generatetoaddress] RAND_bytes failed for the "
                            "coinbase extranonce session id; refusing to mine "
                            "rather than risk reproducing an invalidated block");
                    }
                    return s;
                }();

                if (!session.ok) {
                    result["error"]["code"] = -32603;
                    result["error"]["message"] =
                        "Cannot mine: CSPRNG unavailable for the coinbase "
                        "extranonce session id. Mining is refused because a "
                        "non-random extranonce can reproduce a previously "
                        "invalidated block, which carries BLOCK_FAILED_VALID "
                        "and can never activate.";
                    return result;
                }

                static std::atomic<uint64_t> g_coinbase_extranonce{0};
                const uint64_t counter =
                    g_coinbase_extranonce.fetch_add(1, std::memory_order_relaxed);

                scriptSig.push_back(24);  // push 24 bytes: 16 session + 8 counter
                scriptSig.insert(scriptSig.end(), session.id.begin(),
                                 session.id.end());
                for (int i = 0; i < 8; ++i) {
                    scriptSig.push_back(
                        static_cast<uint8_t>((counter >> (i * 8)) & 0xff));
                }
            }

            input.scriptSig = scriptSig;
            input.sequence = 0xffffffff;
            coinbase.vin.push_back(input);

            // ════════════════════════════════════════════════════════════════════
            // MEMPOOL TRANSACTION SELECTION
            // ════════════════════════════════════════════════════════════════════
            // Select transactions from mempool for inclusion in this block.
            // This ensures mined blocks include pending transactions (not just coinbase).
            std::vector<dinero::Transaction> mempool_txs;
            uint64_t total_fees = 0;

            auto* daemon_ctx = DaemonContext::instance();
            auto* mempool_ptr =
                (daemon_ctx && daemon_ctx->mempool) ? &daemon_ctx->mempool->mempool() : nullptr;
            if (mempool_ptr) {
                auto& mempool = *mempool_ptr;
                // V5 freeze fork: pass the target height so frozen txs are
                // filtered out of the template.
                mempool_txs = mempool.selectTransactionsForBlock(1000000, 4000000, height);

                // Calculate total fees from selected transactions
                for (const auto& mtx : mempool_txs) {
                    auto fee_opt = mempool.getTransactionFee(mtx.GetTxid().AsUint256());
                    if (fee_opt) {
                        total_fees += *fee_opt;
                    }
                }

                if (!mempool_txs.empty()) {
                    g_logger.info("[generatetoaddress] Including " + std::to_string(mempool_txs.size()) +
                                  " mempool transactions, total fees: " + std::to_string(total_fees) + " una");
                    // DEBUG: Check witness data in mempool transactions
                    for (size_t i = 0; i < mempool_txs.size(); i++) {
                        const auto& mtx = mempool_txs[i];
                        bool has_witness = false;
                        for (const auto& inp : mtx.vin) {
                            if (!inp.witness.empty()) {
                                has_witness = true;
                                g_logger.info("[generatetoaddress] DEBUG: tx[" + std::to_string(i) +
                                             "] input has witness with " + std::to_string(inp.witness.size()) +
                                             " elements, first element size: " +
                                             (inp.witness.empty() ? "0" : std::to_string(inp.witness[0].size())) + " bytes");
                            }
                        }
                        if (!has_witness) {
                            g_logger.warn("[generatetoaddress] DEBUG: tx[" + std::to_string(i) +
                                         "] has NO WITNESS DATA! witness_version=" + std::to_string(mtx.witness_version));
                        }
                    }
                }
            }

            if (!mempool_txs.empty()) {
                std::unordered_set<uint256> deferred_txids;
                auto filtered_txs = dinero::FilterChainBackedTemplateTransactions(
                    mempool_txs,
                    [&](const dinero::OutPoint& outpoint) {
                        auto coin_result =
                            chain_db->getCoin(outpoint.txid.AsUint256(), outpoint.vout);
                        return coin_result.status() == dinero::Status::Ok;
                    },
                    &deferred_txids);

                if (filtered_txs.size() != mempool_txs.size()) {
                    total_fees = 0;
                    for (const auto& tx : filtered_txs) {
                        auto fee_opt = mempool_ptr->getTransactionFee(tx.GetTxid().AsUint256());
                        if (fee_opt) {
                            total_fees += *fee_opt;
                        }
                    }
                    g_logger.warning("[generatetoaddress] Deferred " +
                                     std::to_string(mempool_txs.size() - filtered_txs.size()) +
                                     " tx(s) with non-chain-backed inputs until parent confirmations land");
                    mempool_txs = std::move(filtered_txs);
                }
            }

            auto build_block_candidate = [&](const std::vector<dinero::Transaction>& selected_mempool_txs,
                                             uint64_t selected_total_fees,
                                             std::string* build_error) -> bool {
                try {
                    block.vtx.clear();
                    block.utreexo.reset();
                    block.header.utreexo_root = uint256();

                    dinero::Transaction candidate_coinbase = coinbase;
                    dinero::TxOutput output;
                    dinero::AmountUna subsidy = dinero::ConsensusSubsidy::GetBlockSubsidy(height);
                    dinero::AmountUna fees_amount = dinero::AmountUna::Una(selected_total_fees);
                    output.value = subsidy.Add(fees_amount).value_or(subsidy);

                    uint8_t witness_opcode =
                        (decode_result.witness_version == 0) ? 0x00 : (0x50 + decode_result.witness_version);
                    output.scriptPubKey.push_back(witness_opcode);
                    output.scriptPubKey.push_back(static_cast<uint8_t>(decode_result.witness_program.size()));
                    output.scriptPubKey.insert(output.scriptPubKey.end(),
                                              decode_result.witness_program.begin(),
                                              decode_result.witness_program.end());
                    candidate_coinbase.vout.clear();
                    candidate_coinbase.vout.push_back(output);

                    block.vtx.push_back(candidate_coinbase);
                    for (const auto& mtx : selected_mempool_txs) {
                        block.vtx.push_back(mtx);
                    }

                    MaybeAddFilterCommitment(block, selected_mempool_txs, chain_db, height);

                    if (!selected_mempool_txs.empty()) {
                        dinero::consensus::BlockUtreexoData utreexo_data;

                        if (!daemon_ctx || !daemon_ctx->chainstate) {
                            throw std::runtime_error(
                                "generatetoaddress: chainstate unavailable for accumulator_root_before");
                        }
                        auto chainstate_service =
                            std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx->chainstate);
                        if (!chainstate_service || !chainstate_service->utreexoForest()) {
                            throw std::runtime_error(
                                "generatetoaddress: utreexo forest unavailable for accumulator_root_before");
                        }
                        auto* proof_forest = chainstate_service->utreexoForest();
                        // Snapshot the live forest ONCE under its shared lock; derive the
                        // root, the spend proof, and the self-verify roots from the local
                        // clone so they stay consistent AND cannot race the block-connect
                        // writer freeing the forest (audit: forest read-during-free UAF).
                        // Leaf-safe: the lock is held only around the in-memory clone.
                        dinero::consensus::UtreexoForest forest_view;
                        {
                            auto forest_lock =
                                chainstate_service->GetConsensusUTXOSet()->LockForestShared();
                            forest_view = proof_forest->clone();
                        }
                        utreexo_data.accumulator_root_before = forest_view.getCommitment();

                        const auto ephemeral_outputs = CollectEphemeralOutputs(block);
                        const auto intra_block_spent_outputs = CollectIntraBlockSpentOutputs(block, height);
                        std::vector<dinero::consensus::UtreexoHash> proof_targets;

                        for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
                            const auto& tx = block.vtx[tx_idx];
                            for (const auto& input : tx.vin) {
                                const dinero::OutPoint prevout(input.prevout.txid, input.prevout.vout);
                                dinero::consensus::SpentOutputData spent;

                                const auto intra_block_it = intra_block_spent_outputs.find(prevout);
                                if (intra_block_it != intra_block_spent_outputs.end()) {
                                    spent = intra_block_it->second;
                                } else {
                                    auto coin_result =
                                        chain_db->getCoin(input.prevout.txid.AsUint256(), input.prevout.vout);
                                    if (coin_result.status() != dinero::Status::Ok) {
                                        throw std::runtime_error(
                                            "generatetoaddress: missing coin for input " +
                                            input.prevout.txid.AsUint256().GetHex() + ":" +
                                            std::to_string(input.prevout.vout));
                                    }
                                    const auto& coin = coin_result.value();
                                    spent.value = coin.amount;
                                    spent.scriptPubKey = util::HexToBytes(coin.script_pubkey);
                                    spent.created_height = static_cast<uint32_t>(coin.height);
                                    spent.is_coinbase = coin.coinbase;
                                    spent.is_confidential = coin.is_confidential;
                                    spent.commitment = coin.commitment;
                                }

                                utreexo_data.spent_outputs.push_back(spent);

                                if (ephemeral_outputs.count(prevout) != 0) {
                                    continue;
                                }

                                proof_targets.push_back(dinero::consensus::HashUTXOForCreationHeight(
                                    input.prevout.txid.AsUint256(),
                                    input.prevout.vout,
                                    spent.value,
                                    spent.scriptPubKey,
                                    spent.created_height,
                                    spent.is_coinbase));
                            }
                        }

                        utreexo_data.spend_proof = forest_view.generateBlockProof(
                            proof_targets,
                            dinero::consensus::GetUtreexoProofFormatVersion(height));
                        if (utreexo_data.spend_proof.targets.size() != proof_targets.size() ||
                            utreexo_data.spend_proof.positions.size() != proof_targets.size()) {
                            throw std::runtime_error(
                                "generatetoaddress: generated invalid batch proof shape for block " +
                                std::to_string(height));
                        }

                        if (!utreexo_data.spend_proof.targets.empty() &&
                            !forest_view.verifyBatchProofStateless(
                                utreexo_data.spend_proof.targets,
                                utreexo_data.spend_proof.positions,
                                utreexo_data.spend_proof.proof_hashes,
                                utreexo_data.spend_proof.numLeaves,
                                forest_view.getRoots())) {
                            throw std::runtime_error(
                                "generatetoaddress: self-verification failed for block " +
                                std::to_string(height));
                        }

                        block.utreexo = utreexo_data;

                        g_logger.info("[generatetoaddress] Built Utreexo data: " +
                                     std::to_string(utreexo_data.spent_outputs.size()) + " spent outputs, " +
                                     std::to_string(utreexo_data.spend_proof.targets.size()) + " proof targets");
                    }

                    block.header.merkle_root = dinero::consensus::ComputeMerkleRoot(block.vtx);
                    auto recomputed = dinero::consensus::ComputeMerkleRoot(block.vtx);
                    if (recomputed != block.header.merkle_root) {
                        throw std::runtime_error(
                            "CRITICAL: Merkle root mismatch after computation - this should never happen");
                    }

                    return true;
                } catch (const std::exception& e) {
                    if (build_error) {
                        *build_error = e.what();
                    }
                    block.vtx.clear();
                    block.utreexo.reset();
                    block.header.utreexo_root = uint256();
                    return false;
                }
            };

            auto fee_for_tx = [&](const dinero::Transaction& tx) -> uint64_t {
                if (!mempool_ptr) {
                    return 0;
                }
                auto fee_opt = mempool_ptr->getTransactionFee(tx.GetTxid().AsUint256());
                return fee_opt.has_value() ? fee_opt.value() : 0;
            };

            std::string build_error;
            bool built_candidate = build_block_candidate(mempool_txs, total_fees, &build_error);

            if (!built_candidate && mempool_ptr && !mempool_txs.empty()) {
                g_logger.warning("[generatetoaddress] Candidate set poisoned by mempool (" +
                                 std::to_string(mempool_txs.size()) + " txs): " + build_error);

                bool healed_via_missing_prevout = false;
                {
                    std::vector<dinero::Transaction> narrowed_txs = mempool_txs;
                    uint64_t narrowed_fees = total_fees;
                    std::string narrowed_error = build_error;
                    std::unordered_set<uint256> quarantined_roots;

                    for (size_t attempt = 0; attempt < mempool_txs.size(); ++attempt) {
                        auto missing_prevout = dinero::ParseTemplatePoisonMissingPrevout(narrowed_error);
                        if (!missing_prevout.has_value()) {
                            break;
                        }

                        std::unordered_set<uint256> direct_spenders;
                        auto removal_set = dinero::CollectTemplatePoisonRemovalSet(
                            narrowed_txs,
                            missing_prevout.value(),
                            &direct_spenders);
                        if (removal_set.empty()) {
                            break;
                        }

                        std::vector<dinero::Transaction> filtered_txs;
                        uint64_t filtered_fees = 0;
                        filtered_txs.reserve(narrowed_txs.size());
                        for (const auto& tx : narrowed_txs) {
                            const auto txid = tx.GetTxid().AsUint256();
                            if (removal_set.count(txid) != 0) {
                                continue;
                            }
                            filtered_fees += fee_for_tx(tx);
                            filtered_txs.push_back(tx);
                        }

                        quarantined_roots.insert(direct_spenders.begin(), direct_spenders.end());

                        std::string rescue_error;
                        if (build_block_candidate(filtered_txs, filtered_fees, &rescue_error)) {
                            for (const auto& culprit_txid : quarantined_roots) {
                                mempool_ptr->excludeFromBlockTemplates(
                                    culprit_txid,
                                    "generatetoaddress self-heal after missing pure leaf: " + build_error);
                            }

                            if (!quarantined_roots.empty()) {
                                g_logger.warning("[generatetoaddress] Quarantined " +
                                                 std::to_string(quarantined_roots.size()) +
                                                 " root template-poisoning tx(s) after missing-prevout isolation");
                            }

                            mempool_txs = std::move(filtered_txs);
                            total_fees = filtered_fees;
                            built_candidate = true;
                            healed_via_missing_prevout = true;
                            break;
                        }

                        narrowed_txs = std::move(filtered_txs);
                        narrowed_fees = filtered_fees;
                        narrowed_error = rescue_error;
                    }
                }

                if (!healed_via_missing_prevout) {
                    for (size_t culprit_index = mempool_txs.size(); culprit_index-- > 0;) {
                        const uint256 culprit_txid = mempool_txs[culprit_index].GetTxid().AsUint256();
                        std::unordered_set<uint256> removal_set{culprit_txid};

                        bool changed = true;
                        while (changed) {
                            changed = false;
                            for (const auto& tx : mempool_txs) {
                                const uint256 txid = tx.GetTxid().AsUint256();
                                if (removal_set.count(txid) != 0) {
                                    continue;
                                }
                                for (const auto& input : tx.vin) {
                                    if (removal_set.count(input.prevout.txid.AsUint256()) != 0) {
                                        removal_set.insert(txid);
                                        changed = true;
                                        break;
                                    }
                                }
                            }
                        }

                        std::vector<dinero::Transaction> filtered_txs;
                        uint64_t filtered_fees = 0;
                        filtered_txs.reserve(mempool_txs.size());
                        for (const auto& tx : mempool_txs) {
                            if (removal_set.count(tx.GetTxid().AsUint256()) != 0) {
                                continue;
                            }
                            filtered_fees += fee_for_tx(tx);
                            filtered_txs.push_back(tx);
                        }

                        std::string rescue_error;
                        if (!build_block_candidate(filtered_txs, filtered_fees, &rescue_error)) {
                            continue;
                        }

                        mempool_ptr->excludeFromBlockTemplates(
                            culprit_txid,
                            "generatetoaddress self-heal after candidate failure: " + build_error);
                        g_logger.warning("[generatetoaddress] Quarantined template-poisoning tx " +
                                         culprit_txid.GetHex().substr(0, 16) + "... and skipped " +
                                         std::to_string(removal_set.size() - 1) + " dependent tx(s)");

                        mempool_txs = std::move(filtered_txs);
                        total_fees = filtered_fees;
                        built_candidate = true;
                        break;
                    }

                    if (!built_candidate) {
                        std::string coinbase_only_error;
                        if (!build_block_candidate({}, 0, &coinbase_only_error)) {
                            throw std::runtime_error(
                                coinbase_only_error.empty() ? build_error : coinbase_only_error);
                        }
                        g_logger.error("[generatetoaddress] Unable to isolate a single poisoning tx; "
                                       "falling back to coinbase-only block");
                        mempool_txs.clear();
                        total_fees = 0;
                        built_candidate = true;
                    }
                }
            }

            if (!built_candidate) {
                throw std::runtime_error(build_error);
            }

            // ✅ PHASE 10d: Compute Utreexo root using BlockValidator (SINGLE SOURCE OF TRUTH)
            // CRITICAL: Mining MUST use the same ComputeUtreexoRootPure() as validation
            // to ensure identical leaf encoding, ordering, and forest structure.
            // Using separate manual forest construction caused mismatches.
            // Note: daemon_ctx already defined above for mempool access
            if (daemon_ctx && daemon_ctx->chainstate) {
                auto chainstate_svc = std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx->chainstate);
                if (chainstate_svc) {
                    auto* block_validator = chainstate_svc->GetBlockValidator();
                    if (block_validator) {
                        dinero::uint256 computed_root;
                        std::string utreexo_error;
                        if (block_validator->ComputeUtreexoRootPure(block, height, computed_root, utreexo_error)) {
                            block.header.utreexo_root = computed_root;
                            g_logger.info("[generatetoaddress] Computed utreexo_root via BlockValidator: " +
                                         computed_root.GetHex().substr(0, 16) + "...");

                            // ════════════════════════════════════════════════════════════════
                            // TEMPORARY ASSERT: Verify miner and validator compute same root
                            // Remove after 48 hours of successful testing
                            // ════════════════════════════════════════════════════════════════
                            #ifndef NDEBUG
                            {
                                dinero::uint256 verify_root;
                                std::string verify_error;
                                bool verify_ok = block_validator->ComputeUtreexoRootPure(block, height, verify_root, verify_error);
                                if (!verify_ok) {
                                    g_logger.error("[MINING ASSERT] Re-computation FAILED: " + verify_error);
                                    assert(false && "ComputeUtreexoRootPure failed on re-verification");
                                }
                                if (verify_root != block.header.utreexo_root) {
                                    g_logger.error("[MINING ASSERT] ROOT MISMATCH!");
                                    g_logger.error("  Header root:   " + block.header.utreexo_root.GetHex());
                                    g_logger.error("  Recomputed:    " + verify_root.GetHex());
                                    assert(false && "Utreexo root mismatch between miner computation and header");
                                }
                                g_logger.info("[MINING ASSERT] ✅ Root verified: miner == header");
                            }
                            #endif
                        } else {
                            g_logger.error("[generatetoaddress] ComputeUtreexoRootPure failed: " + utreexo_error);
                            // Fall back to zero root (will fail validation, but better than garbage)
                            block.header.utreexo_root = uint256();
                        }
                    } else {
                        g_logger.error("[generatetoaddress] No BlockValidator available");
                        block.header.utreexo_root = uint256();
                    }
                } else {
                    g_logger.error("[generatetoaddress] No ChainstateService available");
                    block.header.utreexo_root = uint256();
                }
            } else {
                g_logger.error("[generatetoaddress] No DaemonContext available for utreexo computation");
                block.header.utreexo_root = uint256();
            }

            // Mine the block (regtest - low difficulty)
            bool mined = false;
            std::array<uint8_t, 32> target;

            try {
                target = dinero::TargetFromBitsBE(block.header.difficulty);
            } catch (const std::exception& e) {
                throw std::runtime_error("TargetFromBitsBE failed: " + std::string(e.what()));
            }

            for (uint32_t nonce = 0; nonce < 0xffffffff && !mined; nonce++) {
                block.header.nonce = nonce;

                std::string header_bytes;
                try {
                    header_bytes = block.header.Serialize();
                } catch (const std::exception& e) {
                    throw std::runtime_error("Block header serialize failed at nonce " + std::to_string(nonce) + ": " + std::string(e.what()));
                }

                if (header_bytes.empty()) {
                    throw std::runtime_error("Empty header bytes");
                }

                std::string hash_hex;
                try {
                    hash_hex = dinero::crypto::double_sha256(
                        reinterpret_cast<const uint8_t*>(header_bytes.data()),
                        header_bytes.size()
                    );
                } catch (const std::exception& e) {
                    throw std::runtime_error("double_sha256 failed: " + std::string(e.what()));
                }

                // Validate hash string length
                if (hash_hex.size() != 64) {
                    throw std::runtime_error("Invalid hash length: " + std::to_string(hash_hex.size()) + " (expected 64)");
                }

                // Convert hash from hex string to byte array (BE format for target comparison)
                // CRITICAL FIX: Do NOT reverse bytes - CSHA256::Finalize() already outputs
                // big-endian format (MSB at index 0), and HashBelowTargetBE expects big-endian.
                // The previous code reversed bytes, causing a mismatch with the validator.
                std::array<uint8_t, 32> hash_bytes{};
                try {
                    for (size_t i = 0; i < 32; i++) {
                        if (i * 2 + 1 >= hash_hex.size()) {
                            throw std::runtime_error("Hash string too short");
                        }
                        std::string hex_byte_str = hash_hex.substr(i * 2, 2);
                        unsigned long byte_val = std::strtoul(hex_byte_str.c_str(), nullptr, 16);
                        hash_bytes[i] = static_cast<uint8_t>(byte_val);  // NO reversal - keep big-endian
                    }
                } catch (const std::exception& e) {
                    throw std::runtime_error("Hash parsing failed: " + std::string(e.what()));
                }

                // Check if hash meets target
                if (dinero::HashBelowTargetBE(hash_bytes, target)) {
                    mined = true;

                    // [DEBUG] Log header bytes and hash for miner vs validator comparison
                    std::string header_hex_miner;
                    header_hex_miner.reserve(header_bytes.size() * 2);
                    for (size_t i = 0; i < header_bytes.size(); i++) {
                        char buf[3];
                        snprintf(buf, sizeof(buf), "%02x", (unsigned char)header_bytes[i]);
                        header_hex_miner += buf;
                    }
                    g_logger.info("[MINER] header_bytes_128 = " + header_hex_miner);
                    g_logger.info("[MINER] hash_computed (big-endian) = " + hash_hex);

                    std::string target_hex;
                    for (size_t i = 0; i < 32; i++) {
                        char buf[3];
                        snprintf(buf, sizeof(buf), "%02x", target[i]);
                        target_hex += buf;
                    }
                    g_logger.info("[MINER] target (big-endian) = " + target_hex);
                    g_logger.info("[MINER] nonce = " + std::to_string(nonce));
                }
            }

            if (!mined) {
                result["error"]["code"] = -32000;
                result["error"]["message"] = "Failed to mine block";
                return result;
            }

            // Submit block
            std::string blockHex;
            try {
                std::string blockBinary = block.Serialize();  // Returns binary data
                if (blockBinary.empty()) {
                    throw std::runtime_error("Empty block serialization for height " + std::to_string(height));
                }

                // Convert binary to hex string
                std::ostringstream hexStream;
                for (unsigned char c : blockBinary) {
                    hexStream << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
                }
                blockHex = hexStream.str();
            } catch (const std::exception& e) {
                throw std::runtime_error("Block serialize failed for height " + std::to_string(height) + ": " + std::string(e.what()));
            }

            // ✅ DEBUG ASSERT: Verify block builds on current tip (not stale template)
            // This assert catches bugs where:
            // - Block template is cached incorrectly
            // - prevHash is not refreshed per iteration
            // - Code refactoring breaks the "fresh tip per block" invariant
            #ifndef NDEBUG
                std::string current_tip = dinero::storage::GetBestBlockHash(chain_db);
                std::string block_parent = block.header.prev_block_hash.GetHex();
                if (block_parent != current_tip) {
                    dinero::g_logger.error("[DEBUG ASSERT] RPC mining built block on STALE tip!");
                    dinero::g_logger.error("  Block parent: " + block_parent);
                    dinero::g_logger.error("  Current tip:  " + current_tip);
                    dinero::g_logger.error("  Height:       " + std::to_string(height));
                    assert(false && "RPC mining invariant violated: block must extend current tip");
                }
            #endif

            dinero::BlockAcceptResult accept_result;
            try {
                accept_result = dinero::BlockAcceptor::AcceptBlockFromRPC(blockHex, "generatetoaddress");
            } catch (const std::exception& e) {
                result["error"]["code"] = -32000;
                result["error"]["message"] = "Block rejected: Exception: " + std::string(e.what());
                dinero::g_logger.error("[generatetoaddress] AcceptBlockFromRPC exception: " + std::string(e.what()));
                return result;
            }

            if (accept_result.rejected()) {
                result["error"]["code"] = -32000;
                result["error"]["message"] = "Block rejected: " + accept_result.reason;
                return result;
            }

            // Treat non-activation as an RPC failure so tests and callers do not
            // assume consensus state advanced when ConnectTip failed.
            std::string mined_hash = accept_result.block_hash.GetHex();
            if (!WaitForActiveTip(chain_db, mined_hash)) {
                std::string active_tip = dinero::storage::GetBestBlockHash(chain_db);
                result["error"]["code"] = -32000;
                result["error"]["message"] =
                    "Block not activated: mined=" + mined_hash + " active_tip=" + active_tip;
                return result;
            }

            // ✅ CRITICAL FIX: Notify wallet manager about new block
            // Note: WalletNotifier integration is handled by WalletWorker via queue
            // The wallet automatically picks up blocks via the onBlockConnected event

            block_hashes.append(accept_result.block_hash.GetHex());

            // ✅ Next iteration will fetch fresh tip from DB (no manual tracking needed)
        }

        result["blocks"] = block_hashes;
        result["message"] = "Generated " + std::to_string(nblocks) + " blocks";
        return result;

    } catch (const std::exception& e) {
        result["error"]["code"] = -32000;
        result["error"]["message"] = std::string("Mining error: ") + e.what();
        return result;
    }
}

// ═══════════════════════════════════════════════════════════
// getblocktemplate - Get block template for mining
// ═══════════════════════════════════════════════════════════

din::Json handle_getblocktemplate(
    mempool::TransactionPool* tx_pool,
    dinero::ChainDB* chain_db,
    const din::Json& params
) {
    din::Json result;
    auto* daemon_ctx = DaemonContext::instance();
    auto* block_storage = daemon_ctx ? daemon_ctx->block_storage.get() : nullptr;

    // Get current chain state
    uint32_t height = dinero::storage::GetChainHeight(chain_db);
    std::string best_hash = dinero::storage::GetBestBlockHash(chain_db);
    uint64_t total_issued = dinero::storage::GetTotalCoinsMined(chain_db);

    // Select transactions for block
    auto selected_txs = tx_pool->select_transactions_for_block();

    // Calculate block subsidy (coinbase reward)
    uint32_t next_height = height + 1;
    // Phase M.6.2: Extract raw value from AmountUna for RPC boundary
    uint64_t subsidy = dinero::ConsensusSubsidy::GetBlockSubsidy(next_height).GetUna();

    // ========== CANONICAL DIFFICULTY SELECTOR ==========
    // ALWAYS route through GetNextWorkRequired - never compute in RPC.
    // This ensures GBT uses the same difficulty as validation/mining.
    const Consensus consensus = GetConsensusForCurrentNetwork();

    uint32_t bits;

    const int64_t prevMTP = (height == 0)
        ? static_cast<int64_t>(dinero::Params().genesis.nTime)
        : dinero::GetMedianTimePastAtHeight(chain_db, block_storage, height);
    if (height > 0 && prevMTP == 0) {
        result["error"] = "Cannot calculate median time past";
        return result;
    }
    const int64_t currentMTP = GetConsensusReferenceTime(
        static_cast<int32_t>(next_height),
        prevMTP,
        static_cast<int64_t>(std::time(nullptr)));
    bits = GetNextWorkRequiredWithChainDB(
        static_cast<int32_t>(next_height),
        currentMTP,
        consensus,
        chain_db,
        block_storage);

    // ✅ SAFETY CHECK: Refuse to return GBT with invalid consensus params
    // This catches bugs like zero bits or zero anchor_time that would cause mining failures
    if (bits == 0) {
        std::cerr << "❌ CRITICAL ERROR: GetNextWorkRequired returned bits=0 (invalid difficulty)" << std::endl;
        result["error"] = "Internal error: Invalid difficulty calculation (bits=0)";
        return result;
    }

    // Format bits as 8-char hex string (BIP22 standard - no 0x prefix)
    char bits_hex[9];
    std::snprintf(bits_hex, sizeof(bits_hex), "%08x", bits);

    // Log difficulty provenance for operations troubleshooting
    const char* phase = "ASERT";  // ASERT from block 1, no bootstrap phase
    std::cerr << "[GBT] h=" << next_height
              << " bits=" << bits_hex
              << " src=GetNextWorkRequired"
              << " phase=" << phase << std::endl;

    // ════════════════════════════════════════════════════════════════════
    // UTREEXO COMMITMENT COMPUTATION
    // ════════════════════════════════════════════════════════════════════
    // Compute current Utreexo state (BEFORE this block) for header offset 68.
    // This is the accumulator state after all previous blocks' UTXOs.
    std::string utreexo_commitment_hex = std::string(64, '0');  // Default: zeros for genesis

    if (height > 0) {
        // Build Utreexo forest from all previous blocks' UTXOs
        dinero::consensus::UtreexoForest forest;
        auto* daemon_ctx = DaemonContext::instance();
        auto chainstate_service =
            (daemon_ctx && daemon_ctx->chainstate)
                ? std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx->chainstate)
                : nullptr;

        for (uint32_t h = 1; h <= height; h++) {
            auto h_result = chain_db->getBlockHashByHeight(h);
            if (h_result.status() != dinero::Status::Ok) continue;

            auto b_result = chainstate_service
                ? chainstate_service->getBlockByHash(h_result.value())
                : dinero::storage::ReadArchivalBlock(*chain_db, block_storage, h_result.value());
            if (b_result.status() != dinero::Status::Ok) continue;

            const auto& prev_block = b_result.value();
            // Add coinbase UTXOs from this block
            for (size_t out_idx = 0; out_idx < prev_block.vtx[0].vout.size(); out_idx++) {
                std::vector<uint8_t> utxo_data;
                TxId prev_txid = prev_block.vtx[0].GetTxid();
                std::vector<uint8_t> txid_bytes = util::HexToBytes(prev_txid.AsUint256().GetHex());
                utxo_data.insert(utxo_data.end(), txid_bytes.begin(), txid_bytes.end());
                uint32_t vout = out_idx;
                for (int j = 0; j < 4; j++) utxo_data.push_back((vout >> (j * 8)) & 0xFF);
                uint64_t amt = prev_block.vtx[0].vout[out_idx].value.GetUna();
                for (int j = 0; j < 8; j++) utxo_data.push_back((amt >> (j * 8)) & 0xFF);
                utxo_data.insert(utxo_data.end(),
                    prev_block.vtx[0].vout[out_idx].scriptPubKey.begin(),
                    prev_block.vtx[0].vout[out_idx].scriptPubKey.end());
                std::vector<uint8_t> leaf_hash = Dinero::Common::double_sha256_raw(utxo_data);
                forest.add(leaf_hash);
            }
        }

        // Get commitment as hex string
        auto commitment = forest.getCommitment();
        if (commitment.size() == 32) {
            std::ostringstream oss;
            for (uint8_t byte : commitment) {
                oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
            }
            utreexo_commitment_hex = oss.str();
        }
    }

    std::cerr << "[GBT] utreexocommitment=" << utreexo_commitment_hex.substr(0, 16) << "..." << std::endl;

    result["version"] = 1;
    result["height"] = static_cast<int>(next_height);
    result["previousblockhash"] = best_hash;
    result["bits"] = std::string(bits_hex);
    result["utreexocommitment"] = utreexo_commitment_hex;  // 64-char hex for header offset 68
    // ✅ BIP113 FIX: Use currentMTP (guaranteed > prevMTP) instead of wall clock time
    // This ensures blocks always pass the "block.time > MTP" validation check
    result["curtime"] = currentMTP;
    result["mintime"] = prevMTP + 1;  // BIP113: Minimum valid timestamp is MTP + 1
    result["maxtime"] = static_cast<int64_t>(std::time(nullptr) + 7200); // 2 hours from now
    result["mutable"] = din::arr();
    result["mutable"].append("time");
    result["mutable"].append("transactions");
    result["mutable"].append("prevblock");

    // Add transactions
    din::Json transactions = din::arr();
    uint64_t total_fees = 0;

    for (const auto& tx : selected_txs) {
        din::Json tx_info;
        tx_info["txid"] = tx.txid;
        tx_info["fee"] = static_cast<int64_t>(tx.fee);
        tx_info["sigops"] = 0; // Simplified
        tx_info["weight"] = static_cast<int>(tx.size * 4);

        // Convert raw data to hex
        std::ostringstream hex_stream;
        for (uint8_t byte : tx.raw_data) {
            hex_stream << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
        }
        tx_info["data"] = hex_stream.str();

        transactions.append(tx_info);
        total_fees += tx.fee;
    }

    result["transactions"] = transactions;
    result["coinbasevalue"] = static_cast<int64_t>(subsidy + total_fees);
    result["longpollid"] = best_hash;

    return result;
}

// ═══════════════════════════════════════════════════════════
// vNext Registration Function  
// ═══════════════════════════════════════════════════════════

void registerMiningExtrasMethods(
    HttpRpcServer* server,
    mempool::TransactionPool* tx_pool,
    dinero::ChainDB* chain_db,
    const MiningExtrasConfig& config,
    std::shared_ptr<dinero::rpc::MiningState> mining_state
) {
    RpcMethodMeta meta;

    // generatetoaddress
    meta.name = "generatetoaddress";
    meta.description = "Mine blocks to specified address (regtest only)";
    meta.params.push_back({"nblocks", "int", "Number of blocks to mine (1-1000)", false});
    meta.params.push_back({"address", "string", "Bech32 address for coinbase rewards", false});
    meta.result.type = "array";
    meta.result.desc = "Array of block hashes generated";
    g_rpcRegistry.registerHandler("mining.generatetoaddress",
        [mining_state, chain_db, config](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return handle_generatetoaddress(mining_state, chain_db, config, params);
        }, meta, "mining_extras");

    // getblocktemplate
    meta = RpcMethodMeta();
    meta.name = "getblocktemplate";
    meta.description = "Returns block template for external miners";
    meta.result.type = "object";
    meta.result.desc = "Block template with transactions, difficulty, and metadata";
    g_rpcRegistry.registerHandler("mining.gettemplate",
        [tx_pool, chain_db](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return handle_getblocktemplate(tx_pool, chain_db, params);
        }, meta, "mining_extras");

    // Alias for compatibility with standard mining software
    g_rpcRegistry.registerHandler("mining.getblocktemplate",
        [tx_pool, chain_db](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return handle_getblocktemplate(tx_pool, chain_db, params);
        }, meta, "mining_extras");

    dinero::g_logger.info("Registered 3 mining extras RPC methods in vNext (incl. mining.getblocktemplate alias)");
}

// ═══════════════════════════════════════════════════════════════
// AUTO-REGISTRATION: Call registerMiningExtrasMethods() at startup
// ═══════════════════════════════════════════════════════════════

namespace {
    // Dummy variable to force execution at program startup
    // This ensures mining extras methods are registered automatically
    struct MiningExtrasAutoRegistration {
        MiningExtrasAutoRegistration() {
            // Note: Cannot call registerMiningExtrasMethods() here directly since it
            // requires server, tx_pool, etc. Instead, rely on main.cpp to call it
            // OR use the vNext self-registering pattern like other method files
            dinero::g_logger.info("[AutoReg] Mining extras registration deferred (requires context from main.cpp)");
        }
    };

    // This static variable will be constructed before main() runs
    static MiningExtrasAutoRegistration _auto_register_mining_extras;
}

} // namespace rpc
} // namespace dinero

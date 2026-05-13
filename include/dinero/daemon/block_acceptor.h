#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <optional>
#include "wallet/transaction.h"
#include "mining/header_layout.h"  // Canonical 128-byte header constants
#include "consensus/undo.h"        // UndoRecord, SpentCoin, CreatedOut
#include "consensus/utreexo_accumulator.h"  // BlockUtreexoData
#include "primitives/block.h"      // Block
#include "primitives/uint256.h"    // uint256
#include "daemon/interfaces/ingress_types.h"  // Step 5: Canonical ingress types

// Forward declarations
struct DaemonContext;  // Global scope (defined in daemon/daemon_context.h)

namespace dinero {
    class ChainDB;

// ============================================================================
// Step 5: Block Ingress Types
// ============================================================================
//
// BlockRejectCode and BlockAcceptResult are now defined in:
//   include/daemon/interfaces/ingress_types.h
//
// This header re-exports them for backward compatibility.
// New code should include ingress_types.h directly.
// ============================================================================

// Legacy alias for backward compatibility (deprecated)
using AcceptResult = BlockAcceptResult;

/**
 * ParsedBlock - Represents a fully parsed Dinero block
 *
 * Header Layout (128 bytes - DINERO_HEADER_SIZE_BYTES):
 *   Offset 0:   version          (4 bytes)
 *   Offset 4:   prevBlockHash    (32 bytes)
 *   Offset 36:  merkleRoot       (32 bytes)
 *   Offset 68:  timestamp        (4 bytes)
 *   Offset 72:  bits             (4 bytes)
 *   Offset 76:  nonce            (4 bytes)
 *   Offset 80:  utreexoCommitment (32 bytes) <- Dinero AFTER-state commitment
 *   Offset 112: reserved         (16 bytes) <- Reserved for future use
 *
 * The block hash is SHA256d of ALL 128 bytes.
 */
struct ParsedBlock {
    // Block header (128 bytes total)
    uint32_t version;
    std::string prevBlockHash;      // 32-byte hex string
    std::string merkleRoot;         // 32-byte hex string
    uint32_t timestamp;
    uint32_t bits;
    uint32_t nonce;
    std::string utreexoRoot;        // 32-byte hex string - AFTER-state Utreexo root (Phase M.4: renamed from utreexoCommitment)

    // Block data
    std::vector<std::string> transactions; // Raw transaction hex strings
    uint32_t txCount;
    size_t blockSize;
    std::string blockHash;          // Display-order hex (for logging/PoW compat only)
    uint256 blockHashRaw;           // Canonical hash from wire bytes (raw uint256, for consensus)

    // Optional Utreexo proof data (for blocks with non-coinbase transactions)
    // Parsed from block body after transactions (if present)
    std::optional<dinero::consensus::BlockUtreexoData> utreexo_data;
};

class BlockAcceptor {
public:
    // Main entry points for block acceptance
    static AcceptResult AcceptBlockFromRPC(const std::string& blockHex, const std::string& source = "rpc");
    static AcceptResult AcceptBlockFromPeer(const Block& block, const std::string& peer_id);

    // Reorg support: Disconnect chain tip (regtest-only)
    static bool ApplyTipInvalidation(const std::string& blockhash, std::string& error);

    // Week 3: Context injection (set by ChainstateService)
    static void SetContext(DaemonContext* ctx) { ctx_ = ctx; }

    // Transaction parsing (public for external use)
    static bool ParseTransaction(const uint8_t* data, size_t dataSize, size_t& offset, dinero::Transaction& tx);

private:
    // Block parsing and validation
    static ParsedBlock ParseBlockFromHex(const std::string& blockHex);
    static bool ValidateBlockHeader(const ParsedBlock& block, std::string& error);
    static bool ValidateProofOfWork(const ParsedBlock& block, uint32_t blockHeight, std::string& error, const std::string& parentHashHex = "");
    static bool ValidateTimestamp(const ParsedBlock& block, std::string& error);

    // Parent chain validation
    static bool FindParentBlock(const std::string& prevHash, uint64_t& parentHeight, std::string& parentChainwork, std::string& error);
    static bool ValidateParentLink(const ParsedBlock& block, uint64_t parentHeight, std::string& error);

    // Transaction validation
    static bool ValidateMerkleRoot(const ParsedBlock& block, std::string& error);
    static bool ValidateBlockSigops(const ParsedBlock& block, std::string& error);
    static bool ValidateContextual(const ParsedBlock& block, uint64_t height, std::string& error);
    static bool ValidateCheckpoint(const ParsedBlock& block, uint64_t height, std::string& error);
    static bool ValidateCoinbase(const std::string& coinbaseTx, uint64_t expectedHeight, uint64_t total_fee_budget, std::string& error);

    // Block operations
    static bool ConnectBlock(const ParsedBlock& block, uint64_t height, const std::string& parentChainwork, std::string& error, bool updateTip = true);
    static bool DisconnectBlock(const ParsedBlock& block, uint64_t height, std::string& error);
    static UndoRecord BuildUndoForBlock(const ParsedBlock& block, uint64_t height, ChainDB* chain_db);
    static Block ConvertParsedBlockToBlock(const ParsedBlock& parsed_block);

    // Utility functions
    static std::string CalculateChainwork(const std::string& parentChainwork, uint32_t bits);
    static std::string ComputeBlockHash(const ParsedBlock& block);
    static std::vector<uint8_t> HexToBytes(const std::string& hex);
    static std::string BytesToHex(const uint8_t* data, size_t len);
    static uint32_t ReadUint32LE(const uint8_t* data);

    // Phase M.4: Clean boundary conversion (ParsedBlock → BlockHeader)
    static BlockHeader ToBlockHeader(const ParsedBlock& parsed);
    static void WriteUint32LE(uint8_t* data, uint32_t value);
    static bool ReadVarInt(const uint8_t* data, size_t dataSize, size_t& offset, uint64_t& value);
    static std::string ComputeMerkleRoot(const std::vector<std::string>& transactions);

    // Notification system
    static void NotifyBlockConnected(const ParsedBlock& block, uint64_t height);

    // Week 3: Context pointer (set by ChainstateService)
    static DaemonContext* ctx_;
};

} // namespace dinero

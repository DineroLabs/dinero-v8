// ChainDbWalletView — node-side implementation of the wallet chain view
// (Task 5.2). Compiled into dinero_chainstate.

#include "storage/chain_db_wallet_view.h"

#include "consensus/asert_params.h"
#include "din_json.h"
#include "storage/archival_block_reader.h"
#include "storage/chain_db.h"

namespace dinero::storage {
namespace {
// Compact-bits -> difficulty, relative to the DIFFICULTY_1 reference.
double BitsToDifficulty(uint32_t bits) {
    arith_uint256 max_target;
    max_target.SetCompact(ASERTConsensus::DIFFICULTY_1_BITS);
    arith_uint256 current_target;
    current_target.SetCompact(bits);
    if (current_target == arith_uint256(0)) return 1.0;
    arith_uint256 quotient = max_target / current_target;
    double difficulty = static_cast<double>(quotient.GetWord(0));
    if (quotient.GetWord(1) > 0)
        difficulty += static_cast<double>(quotient.GetWord(1)) * 18446744073709551616.0;  // 2^64
    return difficulty > 0.0 ? difficulty : 1.0;
}
} // namespace

std::optional<wallet::WalletChainTip> ChainDbWalletView::GetTip() const {
    auto tip = db_.getTip();
    if (!tip.ok()) return std::nullopt;
    return wallet::WalletChainTip{static_cast<int32_t>(tip.value().height),
                                  tip.value().hash};
}

std::optional<uint256> ChainDbWalletView::GetBlockHashAtHeight(int32_t height) const {
    auto hash = db_.getBlockHashByHeight(height);
    if (!hash.ok()) return std::nullopt;
    return hash.value();
}

std::optional<Block> ChainDbWalletView::ReadBlock(const uint256& hash) const {
    auto block = ReadArchivalBlock(db_, block_storage_, hash);
    if (!block.ok()) return std::nullopt;
    return std::move(block).value();
}

uint32_t ChainDbWalletView::GetBestHeight() const {
    auto tip = db_.getTip();
    if (!tip.ok()) return 0;
    return static_cast<uint32_t>(tip.value().height);
}

std::string ChainDbWalletView::GetBestHash() const {
    auto tip = db_.getTip();
    if (!tip.ok())
        return "0000000000000000000000000000000000000000000000000000000000000000";
    return tip.value().hash.GetHex();
}

double ChainDbWalletView::GetDifficulty() const {
    auto tip = db_.getTip();
    if (!tip.ok() || tip.value().height == 0) return 1.0;
    auto hash = db_.getBlockHashByHeight(tip.value().height);
    if (!hash.ok()) return 1.0;
    auto header = db_.getHeader(hash.value());
    if (!header.ok()) return 1.0;
    return BitsToDifficulty(header.value().difficulty);
}

::Json::Value ChainDbWalletView::GetBlockHeader(const std::string& hash_hex) const {
    ::Json::Value result;
    uint256 hash;
    if (!uint256::FromHex(hash_hex, hash)) return result;
    auto header = db_.getHeader(hash);
    if (!header.ok()) return result;
    auto height = db_.getBlockHeight(hash);

    const auto& h = header.value();
    result["hash"] = hash_hex;
    result["version"] = static_cast<int>(h.version);
    result["previousblockhash"] = h.prev_block_hash.GetHex();
    result["merkleroot"] = h.merkle_root.GetHex();
    result["time"] = static_cast<::Json::UInt64>(h.timestamp);
    result["bits"] = h.difficulty;
    result["nonce"] = static_cast<::Json::UInt>(h.nonce);
    result["height"] = height.ok() ? height.value() : -1;
    // Stage-5 review fix: the dead ChainDBHeightProvider reported the TIP's
    // difficulty for every block; report this block's own.
    result["difficulty"] = BitsToDifficulty(h.difficulty);
    return result;
}

} // namespace dinero::storage

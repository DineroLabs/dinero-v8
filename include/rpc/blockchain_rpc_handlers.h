#pragma once

namespace dinero {
namespace rpc {

/**
 * Register blockchain-related RPC methods
 *
 * This includes:
 * - getbestblockhash: Get hash of the best (tip) block
 * - getblockcount: Get current blockchain height
 * - getblockhash: Get block hash by height
 * - getblock: Get block information
 * - getblockheader: Get block header information
 * - getdifficulty: Get current mining difficulty
 * - getchaintips: Get information about chain tips
 */
void registerBlockchainMethods();

} // namespace rpc
} // namespace dinero

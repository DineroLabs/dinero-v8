#pragma once
#include "privacy/silent_scanner.h"

// Implement these 2 functions with your node's tx/mempool types.
// 1) Fill TxView for a mempool tx (has witnesses available)
// 2) Fill TxView for a block tx (prevout lookup required if legacy inputs)
namespace din::sp::glue {
  // Example signatures (adjust to your Tx types)
  template <typename TxType>
  din::sp::TxView make_txview_from_mempool_tx(const TxType& tx);

  template <typename TxType, typename UtxoLookup>
  din::sp::TxView make_txview_from_block_tx(const TxType& tx, UtxoLookup lookup_prevout_spk);
}

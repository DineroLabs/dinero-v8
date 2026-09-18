#pragma once

#include "daemon/services/chainstate_service.h"
#include "storage/chain_db.h"
#include "util/hex.h"

namespace dinero::rpc {

// A proof must bind creation metadata from canonical storage, never wallet or
// request fields. Frozen snapshot records are a fallback only for NotFound,
// under the existing exact-base AND live-leaf authorization check.
inline StatusOr<Coin> ResolveUtreexoProofCoin(ChainstateService& chainstate,
    const ChainDB& db, const uint256& txid, uint32_t vout) {
    auto current = db.getCoin(txid, vout);
    if (current.status() != Status::NotFound) return current;
    const auto frozen = chainstate.ResolveLivePreBaseCoin(OutPoint(TxId(txid), vout));
    if (!frozen) return Status::NotFound;
    Coin coin;
    coin.amount = frozen->value.GetUna();
    coin.script_pubkey = util::hex(frozen->scriptPubKey);
    coin.height = frozen->height;
    coin.coinbase = frozen->isCoinbase;
    coin.is_confidential = frozen->is_confidential;
    coin.commitment = frozen->commitment;
    return coin;
}

}  // namespace dinero::rpc

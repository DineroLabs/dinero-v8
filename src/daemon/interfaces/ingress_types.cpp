#include "daemon/interfaces/ingress_types.h"

namespace dinero {

const char* TxRejectCodeToString(TxRejectCode code) {
    switch (code) {
        case TxRejectCode::OK: return "ok";
        case TxRejectCode::ALREADY_IN_MEMPOOL: return "txn-already-in-mempool";
        case TxRejectCode::ALREADY_IN_CHAIN: return "txn-already-known";
        case TxRejectCode::INVALID_TX: return "txn-validation-failed";
        case TxRejectCode::INSUFFICIENT_FEE: return "insufficient-fee";
        case TxRejectCode::DOUBLE_SPEND_NO_RBF: return "txn-mempool-conflict";
        case TxRejectCode::RBF_REJECTED: return "txn-rbf-rejected";
        case TxRejectCode::TOO_MANY_ANCESTORS: return "too-many-ancestors";
        case TxRejectCode::ANCESTOR_SIZE_EXCEEDED: return "ancestor-size-limit-exceeded";
        case TxRejectCode::TOO_MANY_DESCENDANTS: return "too-many-descendants";
        case TxRejectCode::DESCENDANT_SIZE_EXCEEDED: return "descendant-size-limit-exceeded";
        case TxRejectCode::MEMPOOL_FULL: return "mempool-full";
        case TxRejectCode::MISSING_INPUTS: return "missing-inputs";
        case TxRejectCode::SCRIPT_VERIFY_FAILED: return "script-verification-failed";
        case TxRejectCode::LOCKTIME_NOT_SATISFIED: return "locktime-not-satisfied";
        default: return "unknown-reject-reason";
    }
}

const char* BlockRejectCodeToString(BlockRejectCode code) {
    switch (code) {
        case BlockRejectCode::OK: return "ok";
        case BlockRejectCode::INVALID_HEADER: return "bad-header";
        case BlockRejectCode::INVALID_POW: return "high-hash";
        case BlockRejectCode::INVALID_MERKLE_ROOT: return "bad-txnmrklroot";
        case BlockRejectCode::INVALID_TIMESTAMP: return "time-too-old";
        case BlockRejectCode::INVALID_COINBASE: return "bad-cb-amount";
        case BlockRejectCode::INVALID_TRANSACTION: return "bad-txns";
        case BlockRejectCode::MISSING_PARENT: return "bad-prevblk";
        case BlockRejectCode::INVALID_PARENT_LINK: return "bad-chain";
        case BlockRejectCode::DUPLICATE: return "duplicate";
        case BlockRejectCode::CONCURRENT_IN_FLIGHT: return "concurrent-in-flight";
        case BlockRejectCode::CHECKPOINT_VIOLATION: return "checkpoint-mismatch";
        case BlockRejectCode::INVALID_UTREEXO_ROOT: return "bad-utreexo-root";
        case BlockRejectCode::SIGOPS_LIMIT_EXCEEDED: return "bad-blk-sigops";
        case BlockRejectCode::CONNECT_FAILED: return "db-error";
        case BlockRejectCode::PARSE_ERROR: return "bad-blk-length";
        // Phase B1: Stale template rejection codes
        case BlockRejectCode::STALE_TIP_CHANGED: return "stale-tip";
        case BlockRejectCode::STALE_MEMPOOL_CHANGED: return "stale-mempool";
        case BlockRejectCode::STALE_REORG: return "stale-reorg";
        case BlockRejectCode::STALE_TIMESTAMP: return "stale-time";
        default: return "unknown-reject-reason";
    }
}

} // namespace dinero

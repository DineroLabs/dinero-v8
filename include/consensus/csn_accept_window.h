#pragma once

#include <cstdint>

namespace dinero {
namespace consensus {

// #384: reconcile the CSN forward-validate cursor to the active chain tip.
//
// The OnUtxoBlock forward cursor (next_validate_height) is initialized to
// GetLocalTipHeight()+1 at wiring time — before the snapshot tip is known,
// so it starts at 1. It is meant to be corrected by the drain worker's
// active-tip sync, but that sync only runs after a block buffers and calls
// Notify(); the "too far ahead of cursor" window guard rejects every block
// ABOVE cursor+window BEFORE it can buffer. On a node that restarts
// mid-backfill (cursor re-inits to 1, and no block <= cursor+window is ever
// requested again to bootstrap the sync) this self-reinforces: every
// requested block is rejected forever as "too far ahead of cursor 1"
// (observed on the DineroDPI phone 2026-07-07; a fresh node dodged it only
// because backfill starts at height 1, so low blocks bootstrap the sync).
//
// Reconciling on every incoming block, before any cursor-dependent routing,
// removes the bootstrap dependency: the cursor always tracks the active tip
// (whose blocks are, by definition, already connected/validated), so
// forward blocks fall within the window and pre-base backfill bodies fall
// below the cursor and route to the store-only lane. Never regresses the
// cursor. active_tip_height < 0 means "no active tip yet" (leave as-is).
inline uint32_t ReconcileCsnCursorToTip(uint32_t current_cursor,
                                        int64_t active_tip_height) {
    if (active_tip_height < 0) {
        return current_cursor;  // no active tip yet — leave as-is
    }
    const uint64_t tip_next = static_cast<uint64_t>(active_tip_height) + 1;
    if (tip_next > static_cast<uint64_t>(current_cursor)) {
        return static_cast<uint32_t>(tip_next);
    }
    return current_cursor;  // never regress
}

}  // namespace consensus
}  // namespace dinero

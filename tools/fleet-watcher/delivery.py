# tools/fleet-watcher/delivery.py
"""Outbox drain.

Delivery reads only from the outbox, so an alert that was raised is always
recoverable even if every send fails. Backoff is bounded: a provider outage
must not become an unbounded retry storm, and must not silently drop the item.
"""
from __future__ import annotations

from notify import Notifier
from store import Store

BACKOFF_SECONDS = (30.0, 60.0, 300.0, 900.0)
MAX_BACKOFF = 1800.0

# How long a maintenance-silenced item waits before being reconsidered. One
# poll cycle: the window is re-evaluated on the next drain, so delivery resumes
# promptly once maintenance ends.
MAINTENANCE_DEFER_SECONDS = 60.0

# A maintenance window may silence these. It may never silence safe_mode,
# tip_divergence or observer_divergence: a node on the wrong chain is not
# excused by someone doing maintenance.
SILENCEABLE = {"node_behind", "majority_unreachable", "node_unreachable",
               "telemetry_degraded"}

# Recorded as incidents so the history is queryable, but never delivered. This
# is where "recorded but never notified" is enforced — the engine opens them
# like anything else.
NEVER_DELIVERED = {"node_restart"}


def _backoff(attempts: int) -> float:
    if attempts < len(BACKOFF_SECONDS):
        return BACKOFF_SECONDS[attempts]
    return MAX_BACKOFF


def drain(store: Store, notifier: Notifier, now: float,
          maintenance: bool = False) -> int:
    """Send every due outbox item. Returns the number delivered."""
    delivered = 0
    for item in store.pending_outbox(now=now):
        # The rule travels on the outbox row. Never derived from the title:
        # titles are presentation and must be free to change without altering
        # who gets paged.
        if item.rule in NEVER_DELIVERED:
            store.mark_sent(item.outbox_id)   # recorded, deliberately not sent
            continue
        if maintenance and item.rule in SILENCEABLE:
            # Deferred, NOT consumed. When the window ends, anything still
            # unresolved is delivered. Consuming it would mean the operator's
            # only message for the whole episode is the eventual "resolved",
            # for a condition they were never told about.
            store.defer(item.outbox_id, MAINTENANCE_DEFER_SECONDS)
            continue
        try:
            sent = notifier.send(item.title, item.message, item.priority)
        except Exception:
            # A transport that raises must not abort the cycle. Without this,
            # one unreachable provider head-of-line-blocks every later item —
            # including a tip_divergence page queued behind a safe_mode one —
            # and the raising item keeps attempts=0, so it retries with no
            # backoff at all.
            sent = False
        if sent:
            store.mark_sent(item.outbox_id)
            delivered += 1
        else:
            store.mark_failed(item.outbox_id, _backoff(item.attempts))
    return delivered

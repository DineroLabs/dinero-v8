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
            store.mark_sent(item.outbox_id)   # suppressed, not retried forever
            continue
        if notifier.send(item.title, item.message, item.priority):
            store.mark_sent(item.outbox_id)
            delivered += 1
        else:
            store.mark_failed(item.outbox_id, _backoff(item.attempts))
    return delivered

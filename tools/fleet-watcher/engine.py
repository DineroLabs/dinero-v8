# tools/fleet-watcher/engine.py
"""Confirmation thresholds between rule detection and incident storage.

Rules say "this is true right now". The engine decides whether that is worth an
incident. Opening needs persistence so ordinary propagation never pages;
closing needs persistence so nothing flaps shut on a single good response.
"""
from __future__ import annotations

from typing import Dict, Sequence, Tuple

from models import RuleHit
from store import Store

# Chain-integrity failures page at emergency priority: they must persist until
# acknowledged rather than scroll away among ordinary notifications.
EMERGENCY_RULES = {"safe_mode", "consensus_health", "tip_divergence"}

# safe_mode is a halt, not a lag. Delaying that page buys nothing.
IMMEDIATE_RULES = {"safe_mode"}

# Recorded, never notified.
SILENT_RULES = {"node_restart"}


class Engine:
    def __init__(self, store: Store, open_after: int = 3, close_after: int = 3) -> None:
        self.store = store
        self.open_after = open_after
        self.close_after = close_after
        self._present: Dict[Tuple[str, Tuple[str, ...]], int] = {}
        self._absent: Dict[str, int] = {}

    def process(self, hits: Sequence[RuleHit]) -> None:
        # Sort the node tuple when keying. The store persists nodes sorted, so
        # open_incidents() returns them sorted; comparing against an unsorted
        # RuleHit tuple would never match, the close countdown would never
        # start, and incidents would stay open forever.
        seen = {(h.rule, tuple(sorted(h.nodes))): h for h in hits}

        for key, hit in seen.items():
            if hit.rule in SILENT_RULES:
                continue
            self._present[key] = self._present.get(key, 0) + 1
            threshold = 1 if hit.rule in IMMEDIATE_RULES else self.open_after
            if self._present[key] >= threshold:
                severity = "emergency" if hit.rule in EMERGENCY_RULES else "normal"
                iid = self.store.open_incident(hit.rule, hit.nodes, severity, hit.detail)
                self._absent.pop(iid, None)   # recurrence cancels a close countdown

        for key in list(self._present):
            if key not in seen:
                del self._present[key]

        open_now = {(i.rule, i.nodes): i for i in self.store.open_incidents()}
        for key, incident in open_now.items():
            if key in seen:
                self._absent.pop(incident.incident_id, None)
                continue
            count = self._absent.get(incident.incident_id, 0) + 1
            self._absent[incident.incident_id] = count
            if count >= self.close_after:
                self.store.close_incident(incident.incident_id)
                del self._absent[incident.incident_id]

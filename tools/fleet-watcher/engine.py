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

# Recorded like any other incident, but never delivered. Suppression happens in
# the delivery worker, not here: "recorded but never notified" means the row
# exists and is queryable. Skipping it in the engine would leave no history at
# all, which is a different contract.
SILENT_RULES = {"node_restart"}


class Engine:
    def __init__(self, store: Store, open_after: int = 3, close_after: int = 3) -> None:
        self.store = store
        self.open_after = open_after
        self.close_after = close_after
        self._present: Dict[Tuple[str, Tuple[str, ...]], int] = {}
        self._absent: Dict[str, int] = {}

    def process(self, hits: Sequence[RuleHit]) -> None:
        # Keyed on the RULE alone, never on the node set. Membership drifts
        # between cycles, and keying on it meant a persistent condition whose
        # affected nodes kept changing never reached the open threshold at all.
        seen = {h.rule: h for h in hits}

        for rule, hit in seen.items():
            self._present[rule] = self._present.get(rule, 0) + 1
            threshold = 1 if rule in IMMEDIATE_RULES else self.open_after
            if self._present[rule] >= threshold:
                severity = "emergency" if rule in EMERGENCY_RULES else "normal"
                iid = self.store.open_incident(rule, hit.nodes, severity, hit.detail)
                self._absent.pop(iid, None)   # recurrence cancels a close countdown

        for rule in list(self._present):
            if rule not in seen:
                del self._present[rule]

        open_now = {i.rule: i for i in self.store.open_incidents()}
        for rule, incident in open_now.items():
            if rule in seen:
                self._absent.pop(incident.incident_id, None)
                continue
            count = self._absent.get(incident.incident_id, 0) + 1
            self._absent[incident.incident_id] = count
            if count >= self.close_after:
                self.store.close_incident(incident.incident_id)
                del self._absent[incident.incident_id]

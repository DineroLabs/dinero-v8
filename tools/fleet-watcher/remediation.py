"""Fleet-confirmed restart requests.

This module never restarts a service. It decides whether the already-confirmed
`node_behind` incident is safe to remediate, then emits a narrowly structured
request for the root-owned helper. Keeping policy and privilege separate means
the network-facing watcher never receives general root authority.
"""
from __future__ import annotations

import json
import os
import tempfile
from typing import Optional, Sequence

from models import Incident, Observation
from rules import AGREE, compatible, compute_quorum, disagreeing_voter_pairs


def eligible_incident(observations: Sequence[Observation],
                      incidents: Sequence[Incident], local_node: str,
                      lag_blocks: int) -> Optional[str]:
    """Return the incident that may request ONE local restart.

    Positive evidence is required: a unique quorum, no confirmed fork, the
    local voter still on the quorum chain, two other agreeing voters, and the
    configured lag threshold still exceeded in the current complete cycle.
    A quiet whole fleet therefore never qualifies.
    """
    if disagreeing_voter_pairs(observations):
        return None
    quorum = compute_quorum(observations)
    if quorum is None or local_node not in quorum.members:
        return None
    by_node = {o.node: o for o in observations}
    local = by_node.get(local_node)
    if local is None or not local.reachable or local.height is None:
        return None
    agreeing_others = [
        name for name in quorum.members if name != local_node
        and compatible(local, by_node[name]) == AGREE
    ]
    if len(agreeing_others) < 2:
        return None
    if quorum.median_height - local.height < lag_blocks:
        return None
    for incident in incidents:
        if (incident.rule == "node_behind" and local_node in incident.nodes
                and incident.closed_at is None):
            return incident.incident_id
    return None


def emit_pending(store, request_path: str) -> int:
    """Atomically emit durable requests; return the number submitted."""
    parent = os.path.dirname(request_path)
    if not parent or not os.path.isabs(request_path):
        raise ValueError("remediation request path must be absolute")
    submitted = 0
    for incident_id, node, created_at in store.pending_remediations():
        payload = {
            "version": 1,
            "incident_id": incident_id,
            "node": node,
            "rule": "node_behind",
            "requested_at": created_at,
        }
        fd, temporary = tempfile.mkstemp(prefix=".restart-dinero-", dir=parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                json.dump(payload, handle, sort_keys=True)
                handle.write("\n")
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, request_path)
            store.mark_remediation_submitted(incident_id)
            submitted += 1
        finally:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
    return submitted

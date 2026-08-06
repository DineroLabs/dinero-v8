"""Two-stage poll. Pure I/O — contains no rules.

Stage 1 establishes position. Stage 2 fetches the block hashes needed to
compare nodes at shared heights. Stage 2 exists because quorum cannot be
computed from current tips: comparing tips either demands identical heights
(pages on ordinary propagation) or compares mismatched heights (hides a fork).
"""
from __future__ import annotations

from itertools import combinations
from typing import Any, Dict, List, Optional, Sequence, Set, Tuple

from models import Observation

CORE_TIMEOUT = 10.0


def parse_safe_mode(response: Optional[Dict[str, Any]]) -> Tuple[str, Optional[str]]:
    """Tri-state. A daemon answering -32601 is 'unknown', NEVER 'inactive':
    reporting an unmonitorable node as healthy is worse than saying nothing."""
    if not response or response.get("error"):
        return ("unknown", None)
    result = response.get("result")
    if not isinstance(result, dict) or not isinstance(result.get("active"), bool):
        return ("unknown", None)
    if result["active"]:
        return ("active", result.get("reason") or None)
    return ("inactive", None)


def comparison_heights(voter_heights: Dict[str, int],
                       observer_heights: Dict[str, int],
                       quorum_median: Optional[int]) -> Dict[str, Set[int]]:
    """Which heights each node must be asked about.

    Voters: min(height_a, height_b) for every pair they participate in.
    Observers: min(observer_height, quorum_median) — omitting these would leave
    observer_divergence unimplementable for the same reason.
    """
    needed: Dict[str, Set[int]] = {n: set() for n in voter_heights}
    for a, b in combinations(sorted(voter_heights), 2):
        h = min(voter_heights[a], voter_heights[b])
        needed[a].add(h)
        needed[b].add(h)
    for name, height in observer_heights.items():
        needed.setdefault(name, set())
        if quorum_median is not None:
            needed[name].add(min(height, quorum_median))
    return needed


def poll_cycle(nodes: Sequence[Dict[str, Any]], rpc: Any, cycle_id: str,
               now_iso: str) -> List[Observation]:
    """One complete cycle. A node that fails a core RPC still yields an
    Observation with reachable=False: absence is data, not a gap."""
    stage1: Dict[str, Dict[str, Any]] = {}
    for node in nodes:
        name = node["name"]
        entry: Dict[str, Any] = {"role": node["role"], "reachable": False,
                                 "height": None, "tip_hash": None,
                                 "peers_in": None, "peers_out": None,
                                 "synced": None, "safe_mode": "unknown",
                                 "safe_mode_reason": None, "restart_id": None}
        try:
            status = rpc.call(name, "getdaemonstatus")
            tip = rpc.call(name, "blockchain.getbestblockhash")
            entry["height"] = (status or {}).get("result", {}).get("height")
            entry["tip_hash"] = (tip or {}).get("result")
            entry["reachable"] = entry["height"] is not None and entry["tip_hash"] is not None
        except Exception:
            stage1[name] = entry
            continue

        # Optional fields never affect `reachable`.
        try:
            node_status = rpc.call(name, "node.status") or {}
            peers = node_status.get("result", {}).get("peers", {})
            entry["peers_in"] = peers.get("in")
            entry["peers_out"] = peers.get("out")
            entry["synced"] = node_status.get("result", {}).get("sync", {}).get("synced")
        except Exception:
            pass
        try:
            entry["safe_mode"], entry["safe_mode_reason"] = parse_safe_mode(
                rpc.call(name, "safemode.status"))
        except Exception:
            entry["safe_mode"] = "unknown"
        try:
            entry["restart_id"] = rpc.restart_id(name)
        except Exception:
            entry["restart_id"] = None
        stage1[name] = entry

    voter_heights = {n: e["height"] for n, e in stage1.items()
                     if e["role"] == "voting" and e["reachable"]}
    observer_heights = {n: e["height"] for n, e in stage1.items()
                        if e["role"] == "observer" and e["reachable"]}
    median = None
    if voter_heights:
        ordered = sorted(voter_heights.values())
        median = ordered[len(ordered) // 2]

    needed = comparison_heights(voter_heights, observer_heights, median)
    hashes: Dict[str, Dict[int, str]] = {n: {} for n in stage1}
    for name, heights in needed.items():
        for height in sorted(heights):
            try:
                response = rpc.call(name, "blockchain.getblockhash", [height])
                value = (response or {}).get("result")
                if isinstance(value, str):
                    hashes[name][height] = value
            except Exception:
                continue   # a missing hash is never agreement

    return [Observation(
        cycle_id=cycle_id, timestamp=now_iso, node=name, role=e["role"],
        reachable=e["reachable"], height=e["height"], tip_hash=e["tip_hash"],
        hashes_at=hashes.get(name, {}), peers_in=e["peers_in"],
        peers_out=e["peers_out"], synced=e["synced"], safe_mode=e["safe_mode"],
        safe_mode_reason=e["safe_mode_reason"], restart_id=e["restart_id"],
    ) for name, e in stage1.items()]

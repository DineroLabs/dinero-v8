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
                       observer_heights: Dict[str, int]) -> Dict[str, Set[int]]:
    """Which heights each node must be asked about.

    PAIRWISE, always. `rules.compatible()` compares two nodes at
    `min(height_a, height_b)`, so both sides of every pair the rules will
    evaluate need a hash at exactly that height.

    Voters pair with each other. Observers pair with every voter — NOT with a
    quorum median. An earlier version asked observers for
    `min(height, quorum_median)`, which made `observer_divergence` unable to
    fire at all: no voter was ever asked for a hash at that height, so every
    observer-vs-member comparison came back UNDETERMINED and a fully forked
    observer was invisible. The median was also computed before the quorum
    existed, so it was not even the median the rules would use.

    Observers still never vote. Being comparable and being counted are
    different things.
    """
    needed: Dict[str, Set[int]] = {n: set() for n in voter_heights}
    for name in observer_heights:
        needed.setdefault(name, set())

    for a, b in combinations(sorted(voter_heights), 2):
        h = min(voter_heights[a], voter_heights[b])
        needed[a].add(h)
        needed[b].add(h)

    for observer in sorted(observer_heights):
        for voter in sorted(voter_heights):
            h = min(observer_heights[observer], voter_heights[voter])
            needed[observer].add(h)
            needed[voter].add(h)

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
    needed = comparison_heights(voter_heights, observer_heights)
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

"""Pure rule functions. No I/O, no clock, no config reads.

Everything here is a function of the observations passed in, which is what makes
the false-positive discipline testable.
"""
from __future__ import annotations

from itertools import combinations
from typing import List, Optional, Sequence

from models import Observation, Quorum

QUORUM_MIN = 2


def _voters(observations: Sequence[Observation]) -> List[Observation]:
    return [o for o in observations if o.role == "voting" and o.reachable]


def compatible(a: Observation, b: Observation) -> bool:
    """True when two nodes agree at the deepest height BOTH have reached.

    Compared at min(height_a, height_b). A node one block ahead is compatible.
    A node on a different chain is incompatible once both have reached the
    divergence height — below that point they share the same ancestor and a
    matching hash proves nothing.

    A missing hash is never agreement. Absence of evidence must not become
    evidence of agreement.
    """
    if not (a.reachable and b.reachable):
        return False
    if a.height is None or b.height is None:
        return False
    h = min(a.height, b.height)
    ha, hb = a.hashes_at.get(h), b.hashes_at.get(h)
    if ha is None or hb is None:
        return False
    return ha == hb


def compute_quorum(observations: Sequence[Observation]) -> Optional[Quorum]:
    """The unique largest mutually compatible group of >= QUORUM_MIN voters.

    Returns None when no such group exists, or when two equally sized groups
    compete — both are consensus_health failures, not quorums.
    """
    voters = _voters(observations)
    by_node = {o.node: o for o in voters}

    groups: List[List[str]] = []
    for size in range(len(voters), QUORUM_MIN - 1, -1):
        for combo in combinations(sorted(by_node), size):
            if all(compatible(by_node[x], by_node[y])
                   for x, y in combinations(combo, 2)):
                groups.append(list(combo))
        if groups:
            break  # only the largest size matters

    if len(groups) != 1:
        return None  # none found, or a tie between competing groups

    members = tuple(groups[0])
    heights = sorted(by_node[n].height for n in members)
    median = heights[len(heights) // 2]
    return Quorum(members=members, median_height=median)

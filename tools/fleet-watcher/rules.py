"""Pure rule functions. No I/O, no clock, no config reads.

Everything here is a function of the observations passed in, which is what makes
the false-positive discipline testable.
"""
from __future__ import annotations

from itertools import combinations
from typing import List, Optional, Sequence, Tuple

from models import Observation, Quorum, RuleHit

QUORUM_MIN = 2


def _voters(observations: Sequence[Observation]) -> List[Observation]:
    return [o for o in observations if o.role == "voting" and o.reachable]


AGREE = "agree"
DISAGREE = "disagree"
UNDETERMINED = "undetermined"


def compatible(a: Observation, b: Observation) -> str:
    """Compare two nodes at the deepest height BOTH have reached.

    Returns AGREE, DISAGREE, or UNDETERMINED — three states, not two.

    Compared at min(height_a, height_b). A node one block ahead AGREEs. A node
    on a different chain DISAGREEs once both have reached the divergence
    height; below that point they share the same ancestor and a matching hash
    proves nothing.

    UNDETERMINED is the important one. A missing hash is not agreement — but it
    is not a fork either, and collapsing the two produces a false consensus
    page on a healthy fleet. Worked example: a@100{100:X}, b@100{99:X,100:X},
    c@99{99:X} are all on the same chain, yet a~c has no shared hash. Under a
    binary rule that yields two tied cliques and no quorum, i.e. an emergency
    page caused by one missing RPC response.

    Missing data raises telemetry_degraded. Only DISAGREE evidence raises a
    consensus alarm.
    """
    if not (a.reachable and b.reachable):
        return UNDETERMINED
    if a.height is None or b.height is None:
        return UNDETERMINED
    h = min(a.height, b.height)
    ha, hb = a.hashes_at.get(h), b.hashes_at.get(h)
    if ha is None or hb is None:
        return UNDETERMINED
    return AGREE if ha == hb else DISAGREE


def compute_quorum(observations: Sequence[Observation]) -> Optional[Quorum]:
    """The unique largest mutually compatible group of >= QUORUM_MIN voters.

    Returns None when no such group exists, or when two equally sized groups
    compete — both are consensus_health failures, not quorums.
    """
    voters = _voters(observations)
    by_node = {o.node: o for o in voters}

    # Exhaustive over subsets. Cost is exponential in voter count and peaks
    # during a genuine fork; measured at ~0.3s for 20 voters. The intended
    # fleet is a handful of nodes, so this is deliberate simplicity, not an
    # oversight. Revisit above ~16 voters.
    groups: List[List[str]] = []
    for size in range(len(voters), QUORUM_MIN - 1, -1):
        for combo in combinations(sorted(by_node), size):
            if all(compatible(by_node[x], by_node[y]) == AGREE
                   for x, y in combinations(combo, 2)):
                groups.append(list(combo))
        if groups:
            break  # only the largest size matters

    if len(groups) != 1:
        return None  # none found, or a tie between competing groups

    members = tuple(groups[0])
    heights = sorted(by_node[n].height for n in members)
    # Upper median on an even count: integer, no interpolation. It biases the
    # lag baseline high, which is the safe direction — a node is called behind
    # slightly sooner rather than slightly later.
    median = heights[len(heights) // 2]
    return Quorum(members=members, median_height=median)


def undetermined_voter_pairs(observations: Sequence[Observation]) -> List[Tuple[str, str]]:
    """Voting pairs that could not be compared at all.

    Used to tell "we cannot see" apart from "they disagree": the first is a
    telemetry problem, the second is a consensus problem, and only the second
    justifies an emergency page.
    """
    voters = _voters(observations)
    by_node = {o.node: o for o in voters}
    return [(x, y) for x, y in combinations(sorted(by_node), 2)
            if compatible(by_node[x], by_node[y]) == UNDETERMINED]


NODE_BEHIND_BLOCKS = 10

# compatible() returns AGREE / DISAGREE / UNDETERMINED and is defined above;
# call sites must compare against those constants, never truthiness.


def evaluate(observations: Sequence[Observation],
             voting_total: int,
             previous: Optional[Sequence[Observation]] = None) -> List[RuleHit]:
    """Detect every rule for one complete cycle. Order is not significant."""
    hits: List[RuleHit] = []
    voting = [o for o in observations if o.role == "voting"]
    unreachable = [o.node for o in voting if not o.reachable]

    # safe_mode — any node, voting or observer. Active only; unknown is separate.
    active = [o for o in observations if o.safe_mode == "active"]
    if active:
        hits.append(RuleHit("safe_mode", tuple(o.node for o in active),
                            "; ".join(f"{o.node}: {o.safe_mode_reason or 'no reason given'}"
                                      for o in active)))

    # telemetry_degraded — unknown must never read as inactive.
    unknown = [o.node for o in observations if o.reachable and o.safe_mode == "unknown"]
    if unknown:
        hits.append(RuleHit("telemetry_degraded", tuple(unknown),
                            "safe-mode state unavailable"))

    quorum = compute_quorum(observations)

    # majority_unreachable names the cause; it suppresses consensus_health below.
    majority_gone = len(unreachable) >= (voting_total - QUORUM_MIN + 1)
    if majority_gone:
        hits.append(RuleHit("majority_unreachable", tuple(unreachable),
                            f"{len(unreachable)} of {voting_total} voting nodes unreachable"))
    elif quorum is None:
        # No quorum has two very different causes. If any voting pair could not
        # be compared at all, we cannot see — that is a telemetry gap, not a
        # fork, and must not raise an emergency consensus page. Only genuine
        # disagreement between comparable nodes is a consensus failure.
        blind = undetermined_voter_pairs(observations)
        if blind:
            hits.append(RuleHit("telemetry_degraded",
                                tuple(sorted({n for pair in blind for n in pair})),
                                f"voting pairs not comparable: {blind}"))
        else:
            hits.append(RuleHit("consensus_health", tuple(o.node for o in voting),
                                "no unique largest compatible group of voting nodes"))

    if quorum is not None:
        # tip_divergence — a reachable voter outside the quorum disagrees.
        outside = [o.node for o in voting
                   if o.reachable and o.node not in quorum.members]
        if outside:
            hits.append(RuleHit("tip_divergence", tuple(outside),
                                f"disagrees with quorum {quorum.members}"))

        # node_behind — measured against the quorum's median tip height.
        behind = [o.node for o in observations
                  if o.reachable and o.height is not None
                  and quorum.median_height - o.height >= NODE_BEHIND_BLOCKS]
        if behind:
            hits.append(RuleHit("node_behind", tuple(behind),
                                f">= {NODE_BEHIND_BLOCKS} blocks below quorum median "
                                f"{quorum.median_height}"))

        # observer_divergence — same comparison, never affects quorum.
        by_node = {o.node: o for o in observations}
        # DISAGREE with every quorum member, not merely "not AGREE": an
        # observer we cannot compare is a telemetry gap, not a wrong chain.
        diverged = [o.node for o in observations
                    if o.role == "observer" and o.reachable
                    and quorum.members
                    and all(compatible(o, by_node[m]) == DISAGREE
                            for m in quorum.members)]
        if diverged:
            hits.append(RuleHit("observer_divergence", tuple(diverged),
                                "observer disagrees with quorum at shared height"))

    # node_restart — logged only; needs the prior cycle.
    if previous:
        prev_ids = {o.node: o.restart_id for o in previous}
        restarted = [o.node for o in observations
                     if o.restart_id and prev_ids.get(o.node)
                     and o.restart_id != prev_ids[o.node]]
        if restarted:
            hits.append(RuleHit("node_restart", tuple(restarted), "restart id changed"))

    return hits

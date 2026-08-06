# Fleet Watcher (Sub-project B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an external fleet watcher that polls node RPCs on a cycle, detects chain-integrity problems, persists every observation, and pages only for genuinely dangerous conditions.

**Architecture:** Four isolated units — a two-stage poller (pure I/O), pure rule functions over complete cycles, a SQLite store with atomic cycles and a durable notification outbox, and a notifier driven only from that outbox. An engine applies confirmation thresholds and ties them together. A heartbeat pings an external dead-man only when the whole alarm path is provably healthy.

**Tech Stack:** Python 3, **standard library only** (`sqlite3`, `urllib.request`, `json`, `dataclasses`, `subprocess`, `unittest`). systemd for service management.

**Spec:** `docs/superpowers/specs/2026-08-06-fleet-watcher-design.md` (status: approved). Read it before Task 1.

## Global Constraints

- **Standard library only.** No third-party packages. This matches `tools/check_seed_consistency.py` and means no venv on the watcher host.
- **Rules are pure functions over complete cycles.** No I/O, no clock reads, no config lookups inside rule functions. Time and config are passed in.
- **A cycle is atomic.** All observations for one `cycle_id` commit in a single transaction. Rules never see a partial cycle.
- **Incident creation and notification enqueue are one transaction.** Never open an incident without enqueuing its notification in the same commit.
- **`safe_mode` is tri-state:** `"active"` / `"inactive"` / `"unknown"`. A daemon answering -32601 is `unknown`. **Unknown must never be treated as `inactive`.**
- **`reachable` means the daemon answered a core RPC** (height and tip hash), not that every optional field succeeded.
- **Only voting nodes count toward quorum.** Observers are polled and evaluated but never affect quorum.
- **Secrets never in Git and never logged:** Pushover token/user key and the heartbeat URL come from environment supplied by systemd credentials.
- **`incidents` holds one row per incident**, updated in place — not one row per transition.
- Fleet inventory (hosts, addresses, roles) is configuration, never committed.
- Thresholds: cycle 60s; open after 3 consecutive cycles (`safe_mode` opens immediately); close after 3 consecutive healthy cycles; quorum 2 of 3 voting; `node_behind` ≥ 10 blocks below quorum median; `majority_unreachable` ≥ 2 of 3 voting unreachable.

## File Structure

```
tools/fleet-watcher/
  README.md               operator documentation, including the dead-man test
  models.py               Observation, Incident, OutboxItem, RuleHit, Quorum dataclasses
  config.py               config loading; node inventory and roles
  rules.py                PURE: quorum computation and all rule detection
  engine.py               confirmation thresholds; drives store from rule hits
  store.py                SQLite: observations, incidents, outbox
  notify.py               Notifier interface + PushoverNotifier
  delivery.py             outbox drain worker with bounded backoff
  poller.py               two-stage poll over SSH / local loopback
  heartbeat.py            dead-man ping, gated on the whole alarm path
  watcher.py              entrypoint: cycle loop wiring the above
  tests/
    test_rules.py         quorum + rule detection (the most valuable tests)
    test_engine.py        open/close thresholds
    test_store.py         atomic cycles, outbox durability
    test_notify.py        priority, dedup, recovery
    test_heartbeat.py     three-gate ping suppression
    test_poller.py        parsing against recorded fixtures
  deploy/
    fleet-watcher.service systemd unit
    config.example.json   inventory shape, no real hosts
```

Rules and store are deliberately separate: rules are the part worth testing exhaustively and must never need a database to test.

---

### Task 1: Data models

**Files:**
- Create: `tools/fleet-watcher/models.py`
- Test: `tools/fleet-watcher/tests/test_models.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `Observation`, `Quorum`, `RuleHit`, `Incident`, `OutboxItem` dataclasses used by every later task.

- [ ] **Step 1: Write the failing test**

```python
# tools/fleet-watcher/tests/test_models.py
import unittest
from models import Observation, RuleHit


class TestObservation(unittest.TestCase):
    def test_unreachable_observation_has_null_measurements(self):
        obs = Observation(
            cycle_id="c1", timestamp="2026-08-06T00:00:00Z", node="n1",
            role="voting", reachable=False, height=None, tip_hash=None,
            hashes_at={}, peers_in=None, peers_out=None, synced=None,
            safe_mode="unknown", safe_mode_reason=None, restart_id=None,
        )
        self.assertFalse(obs.reachable)
        self.assertIsNone(obs.height)
        self.assertEqual(obs.safe_mode, "unknown")

    def test_safe_mode_rejects_unknown_values(self):
        with self.assertRaises(ValueError):
            Observation(
                cycle_id="c1", timestamp="t", node="n1", role="voting",
                reachable=True, height=1, tip_hash="aa", hashes_at={},
                peers_in=0, peers_out=0, synced=True,
                safe_mode="maybe", safe_mode_reason=None, restart_id=None,
            )

    def test_role_rejects_unknown_values(self):
        with self.assertRaises(ValueError):
            Observation(
                cycle_id="c1", timestamp="t", node="n1", role="auditor",
                reachable=True, height=1, tip_hash="aa", hashes_at={},
                peers_in=0, peers_out=0, synced=True,
                safe_mode="inactive", safe_mode_reason=None, restart_id=None,
            )


class TestRuleHit(unittest.TestCase):
    def test_rule_hit_is_hashable_for_set_comparison(self):
        a = RuleHit(rule="safe_mode", nodes=("n1",), detail="x")
        b = RuleHit(rule="safe_mode", nodes=("n1",), detail="x")
        self.assertEqual({a}, {b})


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_models -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'models'`

- [ ] **Step 3: Implement the models**

```python
# tools/fleet-watcher/models.py
"""Data models shared by every watcher component.

Frozen dataclasses: an Observation is a fact about a moment and must never be
edited after the fact. RuleHit is hashable so a cycle's hits can be compared as
a set, which is how the engine detects "same condition still present".
"""
from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType
from typing import Mapping, Optional, Tuple

SAFE_MODE_VALUES = ("active", "inactive", "unknown")
ROLES = ("voting", "observer")


@dataclass(frozen=True)
class Observation:
    """One node, one cycle. Unreachable nodes still produce an Observation:
    absence is data, never a gap in the table."""
    cycle_id: str
    timestamp: str
    node: str
    role: str
    reachable: bool
    height: Optional[int]
    tip_hash: Optional[str]
    hashes_at: Mapping[int, str]
    peers_in: Optional[int]
    peers_out: Optional[int]
    synced: Optional[bool]
    safe_mode: str
    safe_mode_reason: Optional[str]
    restart_id: Optional[str]

    def __post_init__(self) -> None:
        # Defensive copy BEFORE wrapping: proxying the caller's own dict would
        # leave them a live handle, and the "frozen" observation would change
        # underneath us. frozen=True only stops attribute reassignment.
        object.__setattr__(self, "hashes_at",
                           MappingProxyType(dict(self.hashes_at)))
        if self.safe_mode not in SAFE_MODE_VALUES:
            raise ValueError(f"safe_mode must be one of {SAFE_MODE_VALUES}")
        if self.role not in ROLES:
            raise ValueError(f"role must be one of {ROLES}")


@dataclass(frozen=True)
class Quorum:
    members: Tuple[str, ...]
    median_height: int


@dataclass(frozen=True)
class RuleHit:
    rule: str
    nodes: Tuple[str, ...]
    detail: str


@dataclass(frozen=True)
class Incident:
    incident_id: str
    rule: str
    nodes: Tuple[str, ...]
    severity: str
    detail: str
    opened_at: str
    closed_at: Optional[str] = None


@dataclass(frozen=True)
class OutboxItem:
    """Carries every immutable routing fact needed to deliver itself, so it
    stays deliverable after its incident closes or the watcher restarts.
    `rule` is stored here rather than looked up: silencing must never depend on
    parsing a human-readable title, which is presentation and may change."""
    outbox_id: int
    incident_id: str
    rule: str
    kind: str          # "open" | "recovery"
    priority: str      # "emergency" | "normal"
    title: str
    message: str
    attempts: int
    next_attempt_at: float
    sent_at: Optional[str]
```

- [ ] **Step 4: Run the tests**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_models -v`
Expected: PASS (report the actual count)

- [ ] **Step 5: Commit**

```bash
git add tools/fleet-watcher/models.py tools/fleet-watcher/tests/test_models.py
git commit -m "feat(watcher): data models with tri-state safe mode"
```

---

### Task 2: Quorum computation

This is the heart of the design and the task most worth getting right. Pairwise comparison at `min(height_a, height_b)`; quorum is the unique largest mutually compatible group of at least 2 voters.

**Files:**
- Create: `tools/fleet-watcher/rules.py`
- Test: `tools/fleet-watcher/tests/test_rules.py`

**Interfaces:**
- Consumes: `Observation`, `Quorum` from Task 1.
- Produces: `compute_quorum(observations) -> Quorum | None` and `compatible(a, b) -> bool`, used by Task 3.

- [ ] **Step 1: Write the failing test**

```python
# tools/fleet-watcher/tests/test_rules.py
import unittest
from models import Observation
from rules import (AGREE, DISAGREE, UNDETERMINED, compatible, compute_quorum,
                   undetermined_voter_pairs)


def obs(node, role="voting", height=100, tip="tip", hashes=None, reachable=True):
    return Observation(
        cycle_id="c", timestamp="t", node=node, role=role, reachable=reachable,
        height=height, tip_hash=tip, hashes_at=hashes or {}, peers_in=1,
        peers_out=1, synced=True, safe_mode="inactive", safe_mode_reason=None,
        restart_id="r1",
    )


class TestCompatible(unittest.TestCase):
    def test_same_hash_at_shared_height_agrees(self):
        a = obs("a", height=100, hashes={100: "H100"})
        b = obs("b", height=101, hashes={100: "H100"})
        self.assertEqual(compatible(a, b), AGREE)

    def test_one_block_ahead_agrees(self):
        """The false positive this whole design exists to avoid."""
        a = obs("a", height=100, tip="H100", hashes={100: "H100"})
        b = obs("b", height=101, tip="H101", hashes={100: "H100"})
        self.assertEqual(compatible(a, b), AGREE)

    def test_different_hash_at_shared_height_disagrees(self):
        a = obs("a", height=100, hashes={100: "H100"})
        b = obs("b", height=101, hashes={100: "FORK"})
        self.assertEqual(compatible(a, b), DISAGREE)

    def test_missing_comparison_hash_is_undetermined_not_agreement(self):
        """A missing signal must never synthesise agreement — but it is also
        not a fork, and calling it one pages on a healthy fleet."""
        a = obs("a", height=100, hashes={})
        b = obs("b", height=101, hashes={100: "H100"})
        self.assertEqual(compatible(a, b), UNDETERMINED)

    def test_unreachable_node_is_undetermined(self):
        a = obs("a", reachable=False, height=None, hashes={})
        b = obs("b", height=100, hashes={100: "H100"})
        self.assertEqual(compatible(a, b), UNDETERMINED)


class TestComputeQuorum(unittest.TestCase):
    def test_three_agreeing_voters_form_quorum(self):
        o = [obs(n, height=100 + i, hashes={100: "H100", 101: "H101"})
             for i, n in enumerate(("a", "b", "c"))]
        q = compute_quorum(o)
        self.assertEqual(set(q.members), {"a", "b", "c"})
        self.assertEqual(q.median_height, 101)

    def test_two_of_three_form_quorum_when_one_forks(self):
        o = [
            obs("a", height=100, hashes={100: "H100"}),
            obs("b", height=100, hashes={100: "H100"}),
            obs("c", height=100, hashes={100: "FORK"}),
        ]
        q = compute_quorum(o)
        self.assertEqual(set(q.members), {"a", "b"})

    def test_observers_never_count_toward_quorum(self):
        o = [
            obs("a", height=100, hashes={100: "H100"}),
            obs("obs1", role="observer", height=100, hashes={100: "H100"}),
        ]
        self.assertIsNone(compute_quorum(o), "one voter cannot form a quorum")

    def test_no_unique_largest_group_means_no_quorum(self):
        """2 v 2 — two equally sized competing groups."""
        o = [
            obs("a", height=100, hashes={100: "X"}),
            obs("b", height=100, hashes={100: "X"}),
            obs("c", height=100, hashes={100: "Y"}),
            obs("d", height=100, hashes={100: "Y"}),
        ]
        self.assertIsNone(compute_quorum(o))

    def test_all_voters_disagree_means_no_quorum(self):
        o = [obs(n, height=100, hashes={100: h})
             for n, h in (("a", "X"), ("b", "Y"), ("c", "Z"))]
        self.assertIsNone(compute_quorum(o))

    def test_majority_of_three_beats_minority_of_two(self):
        """A larger group wins outright — this is not a tie."""
        o = [obs(n, height=100, hashes={100: "X"}) for n in ("a", "b", "c")]
        o += [obs(n, height=100, hashes={100: "Y"}) for n in ("d", "e")]
        q = compute_quorum(o)
        self.assertEqual(set(q.members), {"a", "b", "c"})

    def test_unreachable_voter_is_excluded_from_the_group_search(self):
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"}),
             obs("c", reachable=False, height=None, hashes={})]
        q = compute_quorum(o)
        self.assertEqual(set(q.members), {"a", "b"})

    def test_agreeing_observer_is_not_added_to_the_quorum(self):
        """Stronger than a bare count check: two voters DO form a quorum here,
        so passing requires the observer to be excluded by role, not by size."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"}),
             obs("w", role="observer", height=100, hashes={100: "X"})]
        q = compute_quorum(o)
        self.assertEqual(set(q.members), {"a", "b"})

    def test_median_height_on_an_even_member_count(self):
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=200, hashes={100: "X"})]
        self.assertEqual(compute_quorum(o).median_height, 200)

    def test_string_keyed_hashes_do_not_silently_agree(self):
        """Guards a JSON round-trip that forgets to coerce keys back to int."""
        a = obs("a", height=100, hashes={"100": "X"})
        b = obs("b", height=100, hashes={100: "X"})
        self.assertEqual(compatible(a, b), UNDETERMINED)


class TestUndeterminedPairs(unittest.TestCase):
    def test_the_non_transitive_chain_reports_undetermined_not_a_fork(self):
        """a, b, c are all on the SAME chain, but a and c share no hash.

        Under a binary rule this produced two tied cliques and therefore an
        emergency consensus page on a healthy fleet. It must be reported as a
        telemetry gap instead.
        """
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={99: "W", 100: "X"}),
             obs("c", height=99, hashes={99: "W"})]
        self.assertIsNone(compute_quorum(o))
        self.assertIn(("a", "c"), undetermined_voter_pairs(o))

    def test_a_genuine_fork_reports_no_undetermined_pairs(self):
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"}),
             obs("c", height=100, hashes={100: "FORK"})]
        self.assertEqual(undetermined_voter_pairs(o), [])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_rules -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'rules'`

- [ ] **Step 3: Implement quorum**

```python
# tools/fleet-watcher/rules.py
"""Pure rule functions. No I/O, no clock, no config reads.

Everything here is a function of the observations passed in, which is what makes
the false-positive discipline testable.
"""
from __future__ import annotations

from itertools import combinations
from typing import List, Optional, Sequence, Tuple

from models import Observation, Quorum

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
```

- [ ] **Step 4: Run the tests**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_rules -v`
Expected: PASS (report the actual count)

- [ ] **Step 5: Commit**

```bash
git add tools/fleet-watcher/rules.py tools/fleet-watcher/tests/test_rules.py
git commit -m "feat(watcher): quorum by pairwise comparison at shared height"
```

---

### Task 3: Rule detection

**Files:**
- Modify: `tools/fleet-watcher/rules.py`
- Modify: `tools/fleet-watcher/tests/test_rules.py`

**Interfaces:**
- Consumes: `compute_quorum`, `compatible` from Task 2.
- Produces: `evaluate(observations, voting_total) -> list[RuleHit]`, consumed by Task 5.

- [ ] **Step 1: Write the failing test (append to tests/test_rules.py)**

```python
from rules import evaluate


def rules_fired(hits):
    return {h.rule for h in hits}


class TestEvaluate(unittest.TestCase):
    def _healthy(self):
        return [obs(n, height=100, hashes={100: "H100"}) for n in ("a", "b", "c")]

    def test_healthy_fleet_fires_nothing(self):
        self.assertEqual(rules_fired(evaluate(self._healthy(), 3)), set())

    def test_one_block_apart_fires_nothing(self):
        o = [
            obs("a", height=100, hashes={100: "H100"}),
            obs("b", height=101, hashes={100: "H100"}),
            obs("c", height=100, hashes={100: "H100"}),
        ]
        self.assertEqual(rules_fired(evaluate(o, 3)), set())

    def test_safe_mode_fires_for_any_node(self):
        o = self._healthy()
        o[0] = Observation(**{**o[0].__dict__, "safe_mode": "active",
                              "safe_mode_reason": "bad-utreexo-root"})
        self.assertIn("safe_mode", rules_fired(evaluate(o, 3)))

    def test_unknown_safe_mode_fires_telemetry_not_safe_mode(self):
        o = self._healthy()
        o[0] = Observation(**{**o[0].__dict__, "safe_mode": "unknown"})
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("telemetry_degraded", fired)
        self.assertNotIn("safe_mode", fired)

    def test_tip_divergence_fires_on_fork(self):
        o = self._healthy()
        o[2] = Observation(**{**o[2].__dict__, "hashes_at": {100: "FORK"}})
        self.assertIn("tip_divergence", rules_fired(evaluate(o, 3)))

    def test_no_quorum_from_real_disagreement_fires_consensus_health(self):
        o = [obs(n, height=100, hashes={100: h})
             for n, h in (("a", "X"), ("b", "Y"), ("c", "Z"))]
        self.assertIn("consensus_health", rules_fired(evaluate(o, 3)))

    def test_no_quorum_from_missing_hashes_fires_telemetry_not_consensus(self):
        """The healthy-fleet false page. All three are on the same chain."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={99: "W", 100: "X"}),
             obs("c", height=99, hashes={99: "W"})]
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("telemetry_degraded", fired)
        self.assertNotIn("consensus_health", fired)

    def test_uncomparable_observer_is_not_reported_as_diverged(self):
        o = self._healthy()
        o.append(obs("watch", role="observer", height=50, hashes={}))
        self.assertNotIn("observer_divergence", rules_fired(evaluate(o, 3)))

    def test_majority_unreachable_suppresses_consensus_health(self):
        """It names the cause rather than the symptom."""
        o = [obs("a", height=100, hashes={100: "H100"}),
             obs("b", reachable=False, height=None, hashes={}),
             obs("c", reachable=False, height=None, hashes={})]
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("majority_unreachable", fired)
        self.assertNotIn("consensus_health", fired)

    def test_node_behind_fires_beyond_threshold(self):
        o = [obs("a", height=200, hashes={100: "H100", 200: "H200"}),
             obs("b", height=200, hashes={100: "H100", 200: "H200"}),
             obs("c", height=100, hashes={100: "H100"})]
        self.assertIn("node_behind", rules_fired(evaluate(o, 3)))

    def test_observer_divergence_fires_without_touching_quorum(self):
        o = self._healthy()
        o.append(obs("watch", role="observer", height=100, hashes={100: "FORK"}))
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("observer_divergence", fired)
        self.assertNotIn("consensus_health", fired)
        self.assertNotIn("tip_divergence", fired)

    def test_observer_down_never_affects_quorum(self):
        o = self._healthy()
        o.append(obs("watch", role="observer", reachable=False,
                     height=None, hashes={}))
        self.assertEqual(rules_fired(evaluate(o, 3)), set())

    def test_uncomparable_voter_does_not_fire_tip_divergence(self):
        """A quorum exists and one voter simply cannot be compared. That is a
        telemetry gap, not a fork, and must not raise an emergency page."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"}),
             obs("c", height=99, hashes={99: "W"})]
        fired = rules_fired(evaluate(o, 3))
        self.assertNotIn("tip_divergence", fired)
        self.assertIn("telemetry_degraded", fired)

    def test_voting_node_absent_from_cycle_does_not_fire_consensus_health(self):
        """A collector gap must not masquerade as a fork."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", reachable=False, height=None, hashes={})]
        fired = rules_fired(evaluate(o, 3))
        self.assertNotIn("consensus_health", fired)
        self.assertIn("telemetry_degraded", fired)

    def test_single_unreachable_voter_fires_node_unreachable(self):
        o = self._healthy()
        o[2] = obs("c", reachable=False, height=None, hashes={})
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("node_unreachable", fired)
        self.assertNotIn("majority_unreachable", fired)

    def test_majority_unreachable_suppresses_node_unreachable(self):
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", reachable=False, height=None, hashes={}),
             obs("c", reachable=False, height=None, hashes={})]
        fired = rules_fired(evaluate(o, 3))
        self.assertIn("majority_unreachable", fired)
        self.assertNotIn("node_unreachable", fired)

    def test_three_of_five_unreachable_is_a_majority(self):
        """The older threshold stayed silent here — 2 survivors formed a
        'quorum' that was not a majority of the fleet."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"})]
        o += [obs(n, reachable=False, height=None, hashes={})
              for n in ("c", "d", "e")]
        self.assertIn("majority_unreachable", rules_fired(evaluate(o, 5)))

    def test_observer_diverging_from_a_subset_of_quorum_still_fires(self):
        """DISAGREE with one member and uncomparable with another is still a
        different chain — quorum members agree with each other by construction."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={100: "X"}),
             obs("c", height=100, hashes={100: "X"})]
        o.append(obs("w", role="observer", height=100, hashes={100: "FORK"}))
        self.assertIn("observer_divergence", rules_fired(evaluate(o, 3)))

    def test_node_behind_boundary_is_inclusive_at_ten(self):
        base = [obs(n, height=110, hashes={100: "X", 110: "X"})
                for n in ("a", "b")]
        at_ten = base + [obs("c", height=100, hashes={100: "X"})]
        self.assertIn("node_behind", rules_fired(evaluate(at_ten, 3)))
        at_nine = base + [obs("c", height=101, hashes={100: "X", 101: "X"})]
        self.assertNotIn("node_behind", rules_fired(evaluate(at_nine, 3)))

    def test_telemetry_degraded_is_emitted_at_most_once_per_cycle(self):
        """Two keys for one condition would let a shifting node set reset the
        engine's counter, so a persistent gap would never open an incident."""
        o = [obs("a", height=100, hashes={100: "X"}),
             obs("b", height=100, hashes={99: "W", 100: "X"}),
             obs("c", height=99, hashes={99: "W"})]
        o[0] = Observation(**{**o[0].__dict__, "safe_mode": "unknown"})
        hits = [h for h in evaluate(o, 3) if h.rule == "telemetry_degraded"]
        self.assertEqual(len(hits), 1)

    def test_node_restart_detected_from_changed_restart_id(self):
        prev = self._healthy()
        cur = [Observation(**{**x.__dict__, "restart_id": "r2"}) for x in prev]
        self.assertIn("node_restart",
                      rules_fired(evaluate(cur, 3, previous=prev)))
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_rules -v`
Expected: FAIL — `ImportError: cannot import name 'evaluate'`

- [ ] **Step 3: Implement evaluate (append to rules.py)**

```python
from models import RuleHit

NODE_BEHIND_BLOCKS = 10

# compatible() returns AGREE / DISAGREE / UNDETERMINED and is defined above;
# call sites must compare against those constants, never truthiness.


def evaluate(observations: Sequence[Observation],
             voting_total: int,
             previous: Optional[Sequence[Observation]] = None,
             node_behind_blocks: int = NODE_BEHIND_BLOCKS) -> List[RuleHit]:
    """Detect every rule for one complete cycle. Order is not significant."""
    hits: List[RuleHit] = []
    voting = [o for o in observations if o.role == "voting"]
    by_node = {o.node: o for o in observations}
    unreachable = [o.node for o in voting if not o.reachable]

    # A configured voting node absent from the cycle entirely is neither
    # reachable nor unreachable — it is invisible. Counting only what we
    # observed would let a collector gap masquerade as a healthy small fleet.
    missing = max(0, voting_total - len(voting))
    blind_count = len(unreachable) + missing

    # safe_mode — any node, voting or observer. Active only; unknown is separate.
    active = [o for o in observations if o.safe_mode == "active"]
    if active:
        hits.append(RuleHit("safe_mode", tuple(o.node for o in active),
                            "; ".join(f"{o.node}: {o.safe_mode_reason or 'no reason given'}"
                                      for o in active)))

    quorum = compute_quorum(observations)
    blind_pairs = undetermined_voter_pairs(observations)

    # telemetry_degraded is emitted at most ONCE per cycle. Emitting it twice
    # with different node tuples would give the engine two keys for one
    # condition, and a persistent gap whose node set shifts between cycles
    # would never accumulate to the open threshold — a silent false negative.
    degraded_nodes = {o.node for o in observations
                      if o.reachable and o.safe_mode == "unknown"}
    degraded_why: List[str] = []
    if degraded_nodes:
        degraded_why.append("safe-mode state unavailable")
    if blind_pairs:
        degraded_nodes.update(n for pair in blind_pairs for n in pair)
        degraded_why.append(f"voting pairs not comparable: {blind_pairs}")
    if missing:
        degraded_why.append(f"{missing} configured voting node(s) absent from the cycle")
    if degraded_why:
        hits.append(RuleHit("telemetry_degraded", tuple(sorted(degraded_nodes)),
                            "; ".join(degraded_why)))

    # A true majority, so the rule matches its name at any fleet size. The
    # older form fired only when fewer than QUORUM_MIN nodes remained, which
    # at 5 voters stayed silent with 3 of them down.
    majority_gone = blind_count * 2 > voting_total
    if majority_gone:
        hits.append(RuleHit("majority_unreachable", tuple(unreachable),
                            f"{blind_count} of {voting_total} voting nodes unavailable"))
    elif unreachable:
        # Named separately so one dead node is visible immediately rather than
        # only once a second failure costs the fleet its quorum.
        hits.append(RuleHit("node_unreachable", tuple(unreachable),
                            f"{len(unreachable)} of {voting_total} voting nodes unreachable"))

    if quorum is None and not majority_gone and not blind_pairs and not missing:
        # Only genuine disagreement between comparable nodes is a consensus
        # failure. Every "we cannot see" path above already reported itself.
        hits.append(RuleHit("consensus_health", tuple(o.node for o in voting),
                            "no unique largest compatible group of voting nodes"))

    if quorum is not None:
        # tip_divergence requires positive DISAGREE evidence against a quorum
        # member. "Not in the quorum" is not enough: a node we merely could not
        # compare would otherwise raise an emergency fork page.
        diverging = [o.node for o in voting
                     if o.reachable and o.node not in quorum.members
                     and any(compatible(o, by_node[m]) == DISAGREE
                             for m in quorum.members)]
        if diverging:
            hits.append(RuleHit("tip_divergence", tuple(diverging),
                                f"disagrees with quorum {quorum.members}"))

        # node_behind — measured against the quorum's median tip height.
        behind = [o.node for o in observations
                  if o.reachable and o.height is not None
                  and quorum.median_height - o.height >= node_behind_blocks]
        if behind:
            hits.append(RuleHit("node_behind", tuple(behind),
                                f">= {node_behind_blocks} blocks below quorum median "
                                f"{quorum.median_height}"))

        # observer_divergence — at least one DISAGREE and no AGREE. Requiring
        # DISAGREE with EVERY member would miss a real fork whenever one pair
        # happened to be uncomparable; requiring merely "not AGREE" would page
        # on blindness. This is the predicate that means "on another chain".
        diverged = []
        for o in observations:
            if o.role != "observer" or not o.reachable or not quorum.members:
                continue
            verdicts = [compatible(o, by_node[m]) for m in quorum.members]
            if DISAGREE in verdicts and AGREE not in verdicts:
                diverged.append(o.node)
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
```

- [ ] **Step 4: Run the tests**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_rules -v`
Expected: PASS (report the actual count)

- [ ] **Step 5: Commit**

```bash
git add tools/fleet-watcher/rules.py tools/fleet-watcher/tests/test_rules.py
git commit -m "feat(watcher): rule detection over complete cycles"
```

---

### Task 4: Store — atomic cycles, incidents, outbox

**Files:**
- Create: `tools/fleet-watcher/store.py`
- Test: `tools/fleet-watcher/tests/test_store.py`

**Interfaces:**
- Consumes: models from Task 1.
- Produces: `Store` with `write_cycle`, `open_incident`, `close_incident`, `open_incidents`, `pending_outbox`, `mark_sent`, `mark_failed`, `has_overdue_critical`. Used by Tasks 5, 6, 7.

- [ ] **Step 1: Write the failing test**

```python
# tools/fleet-watcher/tests/test_store.py
import os
import tempfile
import unittest

from models import Observation
from store import Store


def obs(node, cycle="c1"):
    return Observation(
        cycle_id=cycle, timestamp="2026-08-06T00:00:00Z", node=node,
        role="voting", reachable=True, height=100, tip_hash="H",
        hashes_at={100: "H100"}, peers_in=1, peers_out=1, synced=True,
        safe_mode="inactive", safe_mode_reason=None, restart_id="r1",
    )


class TestStore(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        # Frozen clock: the store's timestamps and the tests' synthetic `now`
        # values must live in ONE time domain, or assertions about deadlines
        # test nothing.
        self.store = Store(self.path, clock=lambda: 0.0)

    def tearDown(self):
        os.unlink(self.path)

    def test_cycle_writes_all_observations(self):
        self.store.write_cycle([obs("a"), obs("b"), obs("c")])
        self.assertEqual(len(self.store.cycle("c1")), 3)

    def test_partial_cycle_is_not_visible_when_write_fails(self):
        """Rules must never evaluate half a cycle."""
        bad = [obs("a"), "not-an-observation"]
        with self.assertRaises(Exception):
            self.store.write_cycle(bad)
        self.assertEqual(self.store.cycle("c1"), [])

    def test_rollback_happens_inside_the_transaction_not_before_it(self):
        """The type-check test above fails BEFORE the transaction opens, so it
        proves nothing about atomicity. This one passes validation and dies at
        SQLite bind time, inside executemany, which is the real rollback path."""
        good = obs("a")
        unbindable = Observation(
            cycle_id="c1", timestamp="2026-08-06T00:00:00Z", node="b",
            role="voting", reachable=True, height=object(), tip_hash="H",
            hashes_at={100: "H100"}, peers_in=1, peers_out=1, synced=True,
            safe_mode="inactive", safe_mode_reason=None, restart_id="r1",
        )
        with self.assertRaises(Exception):
            self.store.write_cycle([good, unbindable])
        self.assertEqual(self.store.cycle("c1"), [],
                         "row one must roll back with row two")

    def test_hashes_at_round_trips(self):
        self.store.write_cycle([obs("a")])
        self.assertEqual(self.store.cycle("c1")[0].hashes_at, {100: "H100"})

    def test_open_incident_enqueues_notification_atomically(self):
        iid = self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.assertEqual(len(self.store.open_incidents()), 1)
        pending = self.store.pending_outbox(now=0.0)
        self.assertEqual(len(pending), 1)
        self.assertEqual(pending[0].incident_id, iid)
        self.assertEqual(pending[0].kind, "open")

    def test_close_incident_enqueues_recovery(self):
        iid = self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.store.mark_sent(self.store.pending_outbox(now=0.0)[0].outbox_id)
        self.store.close_incident(iid)
        self.assertEqual(self.store.open_incidents(), [])
        self.assertEqual(self.store.pending_outbox(now=0.0)[0].kind, "recovery")

    def test_only_one_open_incident_per_rule(self):
        a = self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        b = self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.assertEqual(a, b)
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_a_different_node_set_reuses_the_open_incident_and_updates_it(self):
        """Membership is a mutable fact about an open incident, not identity."""
        a = self.store.open_incident("node_unreachable", ("a",), "normal", "a down")
        b = self.store.open_incident("node_unreachable", ("a", "b"), "normal", "a,b down")
        self.assertEqual(a, b)
        only = self.store.open_incidents()[0]
        self.assertEqual(only.nodes, ("a", "b"))
        self.assertEqual(only.detail, "a,b down")

    def test_overdue_critical_detects_stuck_emergency_item(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.assertFalse(self.store.has_overdue_critical(now=0.0, deadline=300.0))
        self.assertTrue(self.store.has_overdue_critical(now=1000.0, deadline=300.0))

    def test_sent_item_is_never_overdue(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.store.mark_sent(self.store.pending_outbox(now=0.0)[0].outbox_id)
        self.assertFalse(self.store.has_overdue_critical(now=1e9, deadline=300.0))

    def test_closing_twice_enqueues_only_one_recovery(self):
        iid = self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.store.mark_sent(self.store.pending_outbox(now=0.0)[0].outbox_id)
        self.store.close_incident(iid)
        self.store.close_incident(iid)
        recoveries = [i for i in self.store.pending_outbox(now=0.0)
                      if i.kind == "recovery"]
        self.assertEqual(len(recoveries), 1)

    def test_open_incidents_nodes_round_trip_sorted(self):
        """open_incident stores sorted nodes, so open_incidents() returns them
        sorted. Any consumer keying on (rule, nodes) must sort too, or its keys
        never match and incidents never close."""
        self.store.open_incident("node_behind", ("c", "a"), "normal", "x")
        self.assertEqual(self.store.open_incidents()[0].nodes, ("a", "c"))

    def test_failed_delivery_is_hidden_until_its_backoff_elapses(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        item = self.store.pending_outbox(now=0.0)[0]
        self.store.mark_failed(item.outbox_id, backoff_seconds=30.0)
        self.assertEqual(self.store.pending_outbox(now=10.0), [])
        due = self.store.pending_outbox(now=40.0)
        self.assertEqual(len(due), 1)
        self.assertEqual(due[0].attempts, 1)

    def test_overdue_deadline_actually_depends_on_the_deadline(self):
        """Guards the regression where created_at and now lived in different
        time domains, making this gate a constant."""
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        self.assertFalse(self.store.has_overdue_critical(now=100.0, deadline=300.0))
        self.assertTrue(self.store.has_overdue_critical(now=400.0, deadline=300.0))

    def test_outbox_survives_reopen(self):
        """The crash window: incident opened, process dies before delivery."""
        self.store.open_incident("safe_mode", ("a",), "emergency", "x")
        reopened = Store(self.path, clock=lambda: 0.0)
        self.assertEqual(len(reopened.pending_outbox(now=0.0)), 1)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_store -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'store'`

- [ ] **Step 3: Implement the store**

```python
# tools/fleet-watcher/store.py
"""SQLite persistence.

Two invariants matter more than anything else here:

1. A cycle commits atomically. Rules must never evaluate a partial cycle — a
   half-written cycle can look exactly like lost quorum.
2. Opening an incident and enqueuing its notification happen in ONE
   transaction. Without that there is a silent failure window: open the
   incident, crash before contacting the provider, and on restart dedup sees an
   open incident and never notifies. The alert is lost precisely because the
   system believes it was already sent.
"""
from __future__ import annotations

import json
import sqlite3
import time
import uuid
from datetime import datetime, timezone
from typing import Callable, List, Optional, Sequence, Tuple

from models import Incident, Observation, OutboxItem

SCHEMA = """
CREATE TABLE IF NOT EXISTS observations (
    cycle_id TEXT NOT NULL, timestamp TEXT NOT NULL, node TEXT NOT NULL,
    role TEXT NOT NULL, reachable INTEGER NOT NULL, height INTEGER,
    tip_hash TEXT, hashes_at TEXT NOT NULL, peers_in INTEGER,
    peers_out INTEGER, synced INTEGER, safe_mode TEXT NOT NULL,
    safe_mode_reason TEXT, restart_id TEXT,
    PRIMARY KEY (cycle_id, node)
);
CREATE INDEX IF NOT EXISTS idx_obs_time ON observations(timestamp);

CREATE TABLE IF NOT EXISTS incidents (
    incident_id TEXT PRIMARY KEY, rule TEXT NOT NULL, nodes TEXT NOT NULL,
    severity TEXT NOT NULL, detail TEXT NOT NULL, opened_at TEXT NOT NULL,
    closed_at TEXT
);
CREATE INDEX IF NOT EXISTS idx_inc_open ON incidents(rule, closed_at);

CREATE TABLE IF NOT EXISTS outbox (
    outbox_id INTEGER PRIMARY KEY AUTOINCREMENT, incident_id TEXT NOT NULL,
    rule TEXT NOT NULL, kind TEXT NOT NULL, priority TEXT NOT NULL, title TEXT NOT NULL,
    message TEXT NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
    created_at REAL NOT NULL, next_attempt_at REAL NOT NULL, sent_at TEXT
);
CREATE INDEX IF NOT EXISTS idx_outbox_pending ON outbox(sent_at, next_attempt_at);
"""


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class Store:
    """The store owns exactly one clock, injected.

    Reading `time.time()` internally while callers pass their own `now` created
    three coexisting time domains and made `has_overdue_critical` a constant:
    with `created_at` seeded at 0.0 and a real `now` of ~1.78e9, the deadline
    term stopped mattering entirely and the gate returned True the moment any
    emergency item was unsent — before a single delivery attempt. A gate that
    carries no information is worse than no gate, because it trains the
    operator to ignore it.
    """

    def __init__(self, path: str, clock: Callable[[], float] = time.time) -> None:
        self._clock = clock
        self.conn = sqlite3.connect(path, isolation_level=None)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA foreign_keys=ON")
        self.conn.executescript(SCHEMA)

    # ---- observations -------------------------------------------------
    def write_cycle(self, observations: Sequence[Observation]) -> None:
        """All-or-nothing. A validation failure rolls the whole cycle back."""
        rows = []
        for o in observations:
            if not isinstance(o, Observation):
                raise TypeError(f"not an Observation: {o!r}")
            rows.append((o.cycle_id, o.timestamp, o.node, o.role,
                         int(o.reachable), o.height, o.tip_hash,
                         json.dumps({str(k): v for k, v in dict(o.hashes_at).items()}),
                         o.peers_in, o.peers_out,
                         None if o.synced is None else int(o.synced),
                         o.safe_mode, o.safe_mode_reason, o.restart_id))
        with self.conn:
            self.conn.execute("BEGIN")
            self.conn.executemany(
                "INSERT OR REPLACE INTO observations "
                "(cycle_id, timestamp, node, role, reachable, height, tip_hash,"
                " hashes_at, peers_in, peers_out, synced, safe_mode,"
                " safe_mode_reason, restart_id)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)", rows)

    def cycle(self, cycle_id: str) -> List[Observation]:
        rows = self.conn.execute(
            "SELECT * FROM observations WHERE cycle_id=? ORDER BY node",
            (cycle_id,)).fetchall()
        return [Observation(
            cycle_id=r["cycle_id"], timestamp=r["timestamp"], node=r["node"],
            role=r["role"], reachable=bool(r["reachable"]), height=r["height"],
            tip_hash=r["tip_hash"],
            hashes_at={int(k): v for k, v in json.loads(r["hashes_at"]).items()},
            peers_in=r["peers_in"], peers_out=r["peers_out"],
            synced=None if r["synced"] is None else bool(r["synced"]),
            safe_mode=r["safe_mode"], safe_mode_reason=r["safe_mode_reason"],
            restart_id=r["restart_id"]) for r in rows]

    # ---- incidents + outbox -------------------------------------------
    def open_incident(self, rule: str, nodes: Tuple[str, ...], severity: str,
                      detail: str) -> str:
        """One open incident per RULE. Enqueues its notification in the same txn.

        Deliberately NOT keyed on the node set. A fleet condition's membership
        drifts between cycles — a node recovers, another degrades — and keying
        on it produced two failures at once: the confirmation counter reset on
        every change so a persistent problem never opened, and when a set
        widened after opening, a "resolved" notification fired for the narrower
        incident while the condition was getting worse.

        Membership and detail are therefore mutable facts about an open
        incident, refreshed on every call, not part of its identity.
        """
        key = json.dumps(sorted(nodes))
        existing = self.conn.execute(
            "SELECT incident_id FROM incidents WHERE rule=? "
            "AND closed_at IS NULL", (rule,)).fetchone()
        if existing:
            with self.conn:
                self.conn.execute(
                    "UPDATE incidents SET nodes=?, detail=? WHERE incident_id=?",
                    (key, detail, existing["incident_id"]))
            return existing["incident_id"]

        incident_id = uuid.uuid4().hex
        priority = "emergency" if severity == "emergency" else "normal"
        with self.conn:
            self.conn.execute("BEGIN")
            self.conn.execute(
                "INSERT INTO incidents VALUES (?,?,?,?,?,?,NULL)",
                (incident_id, rule, key, severity, detail, _now_iso()))
            self.conn.execute(
                "INSERT INTO outbox (incident_id, rule, kind, priority, title,"
                " message, attempts, created_at, next_attempt_at, sent_at)"
                " VALUES (?,?,?,?,?,?,0,?,?,NULL)",
                (incident_id, rule, "open", priority, f"[dinero] {rule}",
                 f"{rule}: {detail}", self._clock(), 0.0))
        return incident_id

    def close_incident(self, incident_id: str) -> None:
        # `closed_at IS NULL` is not decoration: without it, closing twice
        # enqueues two recovery notifications and the operator is told the same
        # incident resolved twice.
        row = self.conn.execute(
            "SELECT rule, detail FROM incidents WHERE incident_id=? "
            "AND closed_at IS NULL", (incident_id,)).fetchone()
        if row is None:
            return
        with self.conn:
            self.conn.execute("BEGIN")
            self.conn.execute("UPDATE incidents SET closed_at=? WHERE incident_id=?",
                              (_now_iso(), incident_id))
            self.conn.execute(
                "INSERT INTO outbox (incident_id, rule, kind, priority, title,"
                " message, attempts, created_at, next_attempt_at, sent_at)"
                " VALUES (?,?,?,?,?,?,0,?,?,NULL)",
                (incident_id, row["rule"], "recovery", "normal",
                 f"[dinero] resolved: {row['rule']}",
                 f"{row['rule']} cleared", self._clock(), 0.0))

    def open_incidents(self) -> List[Incident]:
        rows = self.conn.execute(
            "SELECT * FROM incidents WHERE closed_at IS NULL").fetchall()
        return [Incident(incident_id=r["incident_id"], rule=r["rule"],
                         nodes=tuple(json.loads(r["nodes"])), severity=r["severity"],
                         detail=r["detail"], opened_at=r["opened_at"],
                         closed_at=None) for r in rows]

    # ---- delivery -----------------------------------------------------
    def pending_outbox(self, now: float) -> List[OutboxItem]:
        rows = self.conn.execute(
            "SELECT * FROM outbox WHERE sent_at IS NULL AND next_attempt_at<=? "
            "ORDER BY outbox_id", (now,)).fetchall()
        return [OutboxItem(outbox_id=r["outbox_id"], incident_id=r["incident_id"],
                           rule=r["rule"], kind=r["kind"], priority=r["priority"],
                           title=r["title"],
                           message=r["message"], attempts=r["attempts"],
                           next_attempt_at=r["next_attempt_at"],
                           sent_at=r["sent_at"]) for r in rows]

    def mark_sent(self, outbox_id: int) -> None:
        with self.conn:
            self.conn.execute("UPDATE outbox SET sent_at=? WHERE outbox_id=?",
                              (_now_iso(), outbox_id))

    def mark_failed(self, outbox_id: int, backoff_seconds: float) -> None:
        with self.conn:
            self.conn.execute(
                "UPDATE outbox SET attempts=attempts+1, next_attempt_at=? "
                "WHERE outbox_id=?", (self._clock() + backoff_seconds, outbox_id))

    def defer(self, outbox_id: int, seconds: float) -> None:
        """Postpone delivery WITHOUT consuming the item or counting an attempt.

        Distinct from mark_failed: nothing went wrong, policy simply says not
        yet. Distinct from mark_sent: the notification has not been delivered
        and must still be. Marking a suppressed item as sent loses it forever —
        the operator would later receive a resolution for a condition they were
        never told about.
        """
        with self.conn:
            self.conn.execute("UPDATE outbox SET next_attempt_at=? WHERE outbox_id=?",
                              (self._clock() + seconds, outbox_id))

    def enqueue_canary(self) -> None:
        """Queue a synthetic low-priority notification.

        The overdue gate is reactive: it can only test a path that already had
        traffic. With a bad credential and no emergencies yet, nothing is ever
        overdue, so the heartbeat pings happily while alerting is dead. A canary
        exercises the real credentials against the real provider on a schedule,
        so the alert path is known good BEFORE it is needed rather than
        discovered broken during an incident.
        """
        with self.conn:
            self.conn.execute(
                "INSERT INTO outbox (incident_id, rule, kind, priority, title,"
                " message, attempts, created_at, next_attempt_at, sent_at)"
                " VALUES (?,?,?,?,?,?,0,?,?,NULL)",
                ("canary", "canary", "canary", "normal",
                 "[dinero] watcher alive",
                 "Scheduled canary: the alert path is working.",
                 self._clock(), 0.0))

    def has_stale_canary(self, now: float, max_age: float) -> bool:
        """True when ANY canary is still UNSENT past `max_age`.

        Deliberately not "the newest": an older stuck canary keeps the gate
        tripped even if a later one delivered. That over-reports slightly and
        fails in the safe direction for a dead-man, which is the direction to
        fail in.

        This is what makes the canary self-verifying. Without it the canary
        exercises the alert path but nothing reads the result: if delivery is
        broken, the canary fails silently, the heartbeat keeps pinging, and the
        dead-man stays quiet — the exact failure the canary was added to catch.

        Wired into should_ping, a stuck canary suppresses the heartbeat, the
        external dead-man fires, and the operator learns the alert path is dead
        BEFORE an incident needs it.
        """
        row = self.conn.execute(
            "SELECT 1 FROM outbox WHERE rule='canary' AND sent_at IS NULL "
            "AND created_at + ? < ? LIMIT 1", (max_age, now)).fetchone()
        return row is not None

    def last_canary_at(self) -> Optional[float]:
        row = self.conn.execute(
            "SELECT MAX(created_at) AS t FROM outbox WHERE rule='canary'"
        ).fetchone()
        return row["t"] if row and row["t"] is not None else None

    def has_overdue_critical(self, now: float, deadline: float) -> bool:
        """An emergency notification still unsent past its deadline. This is one
        of the three heartbeat gates: alerting is broken even if polling works."""
        row = self.conn.execute(
            "SELECT 1 FROM outbox WHERE sent_at IS NULL AND priority='emergency' "
            "AND created_at + ? < ? LIMIT 1", (deadline, now)).fetchone()
        return row is not None
```

- [ ] **Step 4: Run the tests**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_store -v`
Expected: PASS (report the actual count)

- [ ] **Step 5: Commit**

```bash
git add tools/fleet-watcher/store.py tools/fleet-watcher/tests/test_store.py
git commit -m "feat(watcher): sqlite store with atomic cycles and durable outbox"
```

---

### Task 5: Engine — confirmation thresholds

**Files:**
- Create: `tools/fleet-watcher/engine.py`
- Test: `tools/fleet-watcher/tests/test_engine.py`

**Interfaces:**
- Consumes: `RuleHit` (Task 1), `Store` (Task 4).
- Produces: `Engine(store, open_after=3, close_after=3)` with `process(hits)`. Used by Task 9.

- [ ] **Step 1: Write the failing test**

```python
# tools/fleet-watcher/tests/test_engine.py
import os
import tempfile
import unittest

from engine import Engine
from models import RuleHit
from store import Store

SAFE = RuleHit("safe_mode", ("a",), "halted")
BEHIND = RuleHit("node_behind", ("c",), "12 blocks")


class TestEngine(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.store = Store(self.path)
        self.engine = Engine(self.store, open_after=3, close_after=3)

    def tearDown(self):
        os.unlink(self.path)

    def test_ordinary_rule_needs_three_cycles_to_open(self):
        self.engine.process([BEHIND]); self.engine.process([BEHIND])
        self.assertEqual(self.store.open_incidents(), [])
        self.engine.process([BEHIND])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_safe_mode_opens_immediately(self):
        self.engine.process([SAFE])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_intermittent_condition_never_opens(self):
        """One good cycle resets the count — this is the anti-flap property."""
        for _ in range(5):
            self.engine.process([BEHIND])
            self.engine.process([])
        self.assertEqual(self.store.open_incidents(), [])

    def test_incident_needs_three_healthy_cycles_to_close(self):
        self.engine.process([SAFE])
        self.engine.process([]); self.engine.process([])
        self.assertEqual(len(self.store.open_incidents()), 1)
        self.engine.process([])
        self.assertEqual(self.store.open_incidents(), [])

    def test_safe_mode_does_not_flap_closed_on_one_good_cycle(self):
        self.engine.process([SAFE])
        self.engine.process([])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_recurrence_during_close_countdown_keeps_incident_open(self):
        self.engine.process([SAFE])
        self.engine.process([]); self.engine.process([SAFE]); self.engine.process([])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_emergency_severity_for_chain_integrity_rules(self):
        self.engine.process([SAFE])
        self.assertEqual(self.store.open_incidents()[0].severity, "emergency")

    def test_drifting_node_set_still_opens_after_three_cycles(self):
        """A fleet always missing SOME node, never the same one three cycles
        running, previously opened nothing at all."""
        for nodes in (("a",), ("a", "b"), ("b",)):
            self.engine.process([RuleHit("telemetry_degraded", nodes, "gap")])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_widening_node_set_updates_membership_not_a_second_incident(self):
        for _ in range(3):
            self.engine.process([RuleHit("node_unreachable", ("a",), "a down")])
        self.engine.process([RuleHit("node_unreachable", ("a", "b"), "a,b down")])
        incidents = self.store.open_incidents()
        self.assertEqual(len(incidents), 1)
        self.assertEqual(incidents[0].nodes, ("a", "b"))
        self.assertEqual(incidents[0].detail, "a,b down")

    def test_unsorted_node_tuple_still_closes(self):
        """The store persists nodes sorted; a naive key comparison against the
        raw tuple would never match and the incident would never close."""
        hit = RuleHit("node_behind", ("b", "a"), "behind")
        for _ in range(3):
            self.engine.process([hit])
        self.assertEqual(len(self.store.open_incidents()), 1)
        for _ in range(3):
            self.engine.process([])
        self.assertEqual(self.store.open_incidents(), [])

    def test_emergency_but_not_immediate_rule_still_needs_three_cycles(self):
        hit = RuleHit("consensus_health", ("a", "b"), "no quorum")
        self.engine.process([hit]); self.engine.process([hit])
        self.assertEqual(self.store.open_incidents(), [])
        self.engine.process([hit])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_silent_rule_is_recorded_as_an_incident(self):
        """'Recorded but never notified' — the engine records it; the delivery
        worker is what declines to send."""
        hit = RuleHit("node_restart", ("a",), "restart id changed")
        for _ in range(3):
            self.engine.process([hit])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_steady_condition_opens_exactly_one_incident_and_one_notification(self):
        hit = RuleHit("node_behind", ("c",), "12 blocks")
        for _ in range(7):
            self.engine.process([hit])
        self.assertEqual(len(self.store.open_incidents()), 1)
        opens = [i for i in self.store.pending_outbox(now=0.0) if i.kind == "open"]
        self.assertEqual(len(opens), 1)

    def test_close_then_recur_costs_the_full_open_threshold_again(self):
        hit = RuleHit("node_behind", ("c",), "12 blocks")
        for _ in range(3):
            self.engine.process([hit])
        for _ in range(3):
            self.engine.process([])
        self.assertEqual(self.store.open_incidents(), [])
        self.engine.process([hit]); self.engine.process([hit])
        self.assertEqual(self.store.open_incidents(), [])
        self.engine.process([hit])
        self.assertEqual(len(self.store.open_incidents()), 1)

    def test_normal_severity_for_observer_divergence(self):
        hit = RuleHit("observer_divergence", ("w",), "forked")
        for _ in range(3):
            self.engine.process([hit])
        self.assertEqual(self.store.open_incidents()[0].severity, "normal")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_engine -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'engine'`

- [ ] **Step 3: Implement the engine**

```python
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
        self._present: Dict[str, int] = {}   # keyed on rule, not on nodes
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
```

- [ ] **Step 4: Run the tests**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_engine -v`
Expected: PASS (report the actual count)

- [ ] **Step 5: Commit**

```bash
git add tools/fleet-watcher/engine.py tools/fleet-watcher/tests/test_engine.py
git commit -m "feat(watcher): confirmation thresholds for opening and closing incidents"
```

---

### Task 6: Notifier and delivery worker

**Files:**
- Create: `tools/fleet-watcher/notify.py`, `tools/fleet-watcher/delivery.py`
- Test: `tools/fleet-watcher/tests/test_notify.py`

**Interfaces:**
- Consumes: `Store` (Task 4), `OutboxItem` (Task 1).
- Produces: `Notifier` protocol with `send(title, message, priority) -> bool`; `PushoverNotifier`; `drain(store, notifier, now, maintenance)`. Used by Tasks 7 and 9.

- [ ] **Step 1: Write the failing test**

```python
# tools/fleet-watcher/tests/test_notify.py
import os
import tempfile
import unittest

from delivery import drain
from store import Store


class FakeNotifier:
    def __init__(self, fail_times=0):
        self.sent = []
        self.fail_times = fail_times

    def send(self, title, message, priority):
        if self.fail_times > 0:
            self.fail_times -= 1
            return False
        self.sent.append((title, message, priority))
        return True


class TestDelivery(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.store = Store(self.path)

    def tearDown(self):
        os.unlink(self.path)

    def test_pending_item_is_delivered_once(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        n = FakeNotifier()
        drain(self.store, n, now=1.0)
        drain(self.store, n, now=2.0)
        self.assertEqual(len(n.sent), 1)
        self.assertEqual(n.sent[0][2], "emergency")

    def test_failed_delivery_is_retried_later_not_lost(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        n = FakeNotifier(fail_times=1)
        drain(self.store, n, now=1.0)
        self.assertEqual(n.sent, [])
        self.assertEqual(len(self.store.pending_outbox(now=1e9)), 1)
        drain(self.store, n, now=1e9)
        self.assertEqual(len(n.sent), 1)

    def test_recovery_notification_is_sent_on_close(self):
        iid = self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        n = FakeNotifier()
        drain(self.store, n, now=1.0)
        self.store.close_incident(iid)
        drain(self.store, n, now=2.0)
        self.assertEqual(len(n.sent), 2)
        self.assertIn("resolved", n.sent[1][0])

    def test_maintenance_suppresses_silenceable_rules_only(self):
        self.store.open_incident("node_behind", ("c",), "normal", "12 blocks")
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        n = FakeNotifier()
        drain(self.store, n, now=1.0, maintenance=True)
        rules = [t for t, _, _ in n.sent]
        self.assertTrue(any("safe_mode" in r for r in rules))
        self.assertFalse(any("node_behind" in r for r in rules))

    def test_title_text_cannot_affect_silencing(self):
        """Silencing reads the stored rule, never the human-readable title."""
        self.store.open_incident("node_behind", ("c",), "normal", "12 blocks")
        self.store.conn.execute("UPDATE outbox SET title='completely unrelated'")
        n = FakeNotifier()
        drain(self.store, n, now=1.0, maintenance=True)
        self.assertEqual(n.sent, [], "still silenced despite the retitle")
        drain(self.store, FakeNotifier(), now=2.0, maintenance=False)

    def test_recovery_item_is_silenceable_after_its_incident_closed(self):
        """The outbox row must stand alone once the incident is gone."""
        iid = self.store.open_incident("node_behind", ("c",), "normal", "12")
        self.store.mark_sent(self.store.pending_outbox(now=0.0)[0].outbox_id)
        self.store.close_incident(iid)
        n = FakeNotifier()
        drain(self.store, n, now=1.0, maintenance=True)
        self.assertEqual(n.sent, [])

    def test_silent_rule_is_recorded_but_never_delivered(self):
        self.store.open_incident("node_restart", ("a",), "normal", "restarted")
        n = FakeNotifier()
        drain(self.store, n, now=1.0)
        self.assertEqual(n.sent, [])
        self.assertEqual(self.store.pending_outbox(now=1e9), [],
                         "consumed, not left retrying forever")

    def test_maintenance_never_suppresses_observer_divergence(self):
        self.store.open_incident("observer_divergence", ("w",), "normal", "forked")
        n = FakeNotifier()
        drain(self.store, n, now=1.0, maintenance=True)
        self.assertEqual(len(n.sent), 1)


class TestPushoverNotifier(unittest.TestCase):
    """notify.py had no coverage at all — half the deliverable. These pin the
    two things that decide whether a page actually arrives."""

    def _notifier(self):
        from notify import PushoverNotifier
        return PushoverNotifier("tok", "usr")

    class _Response:
        status = 200

        def __init__(self, body):
            self._body = body

        def read(self):
            return self._body

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

    def _with_urlopen(self, fake):
        import notify
        original = notify.urllib.request.urlopen
        notify.urllib.request.urlopen = fake
        self.addCleanup(setattr, notify.urllib.request, "urlopen", original)

    def test_emergency_sets_priority_retry_and_a_three_hour_expire(self):
        captured = {}

        def fake(request, timeout=None):
            captured["body"] = request.data.decode()
            return self._Response(b'{"status":1}')

        self._with_urlopen(fake)
        self.assertTrue(self._notifier().send("t", "m", "emergency"))
        self.assertIn("priority=2", captured["body"])
        self.assertIn("expire=10800", captured["body"])
        self.assertIn("retry=", captured["body"])

    def test_normal_priority_sets_no_retry_parameters(self):
        captured = {}

        def fake(request, timeout=None):
            captured["body"] = request.data.decode()
            return self._Response(b'{"status":1}')

        self._with_urlopen(fake)
        self._notifier().send("t", "m", "normal")
        self.assertNotIn("priority=2", captured["body"])
        self.assertNotIn("expire=", captured["body"])

    def test_transport_exceptions_return_false_rather_than_propagating(self):
        import http.client
        import ssl

        for exc in (ConnectionResetError("reset"),
                    http.client.IncompleteRead(b""),
                    ssl.SSLError("handshake"),
                    OSError("unreachable"),
                    ValueError("not json")):
            def boom(request, timeout=None, _e=exc):
                raise _e

            self._with_urlopen(boom)
            self.assertFalse(self._notifier().send("t", "m", "normal"),
                             f"{type(exc).__name__} must not propagate")

    def test_a_rejected_message_is_not_reported_as_sent(self):
        self._with_urlopen(lambda request, timeout=None:
                           self._Response(b'{"status":0}'))
        self.assertFalse(self._notifier().send("t", "m", "normal"))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_notify -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'delivery'`

- [ ] **Step 3: Implement notify.py**

```python
# tools/fleet-watcher/notify.py
"""Notification transports.

The interface is deliberately tiny so another provider can be added without
touching rules or storage. send() returns success rather than raising, because
delivery failure is an expected condition the outbox already handles.
"""
from __future__ import annotations

import http.client
import json
import urllib.parse
import urllib.request
from typing import Protocol

PUSHOVER_URL = "https://api.pushover.net/1/messages.json"


class Notifier(Protocol):
    def send(self, title: str, message: str, priority: str) -> bool: ...


class PushoverNotifier:
    """Pushover transport.

    Emergency priority repeats until acknowledged OR until `expire` elapses —
    three hours, Pushover's maximum. It is not indefinite re-paging, and this
    docstring deliberately does not claim otherwise: `send()` returns True when
    Pushover ACCEPTS the message, which is submission, not operator
    acknowledgement. After the window lapses unacknowledged nothing re-pages,
    though the incident stays open and queryable, and the normal recovery
    notification still fires when the condition clears.

    True indefinite escalation needs receipt polling and persisted
    acknowledgement state. That is a separate feature, not something a
    docstring should imply.
    """

    def __init__(self, token: str, user_key: str, timeout: float = 10.0,
                 retry: int = 60, expire: int = 10800) -> None:
        self._token = token
        self._user = user_key
        self._timeout = timeout
        self._retry = retry
        self._expire = expire

    def send(self, title: str, message: str, priority: str) -> bool:
        fields = {"token": self._token, "user": self._user,
                  "title": title, "message": message}
        if priority == "emergency":
            fields.update({"priority": "2", "retry": str(self._retry),
                           "expire": str(self._expire)})
        data = urllib.parse.urlencode(fields).encode()
        request = urllib.request.Request(PUSHOVER_URL, data=data, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=self._timeout) as response:
                return json.loads(response.read()).get("status") == 1
        except (OSError, http.client.HTTPException, ValueError):
            # urllib only wraps errors from h.request(); getresponse() and
            # read() propagate raw, so ConnectionResetError, IncompleteRead and
            # ssl.SSLError all reach here. urllib.error.URLError and
            # socket.timeout are both subclasses of OSError, so this set covers
            # them without naming them.
            return False
```

- [ ] **Step 4: Implement delivery.py**

```python
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

# How often the synthetic canary exercises the real alert path.
CANARY_INTERVAL_SECONDS = 24 * 60 * 60


def canary_due(store, now: float,
               interval: float = CANARY_INTERVAL_SECONDS) -> bool:
    """True when the alert path has not been exercised within `interval`."""
    last = store.last_canary_at()
    return last is None or (now - last) >= interval

# A maintenance window may silence these. It may never silence safe_mode,
# tip_divergence or observer_divergence: a node on the wrong chain is not
# excused by someone doing maintenance.
SILENCEABLE = {"node_behind", "majority_unreachable", "node_unreachable",
               "telemetry_degraded", "canary"}

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

```

- [ ] **Step 5: Run the tests**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_notify -v`
Expected: PASS (report the actual count)

- [ ] **Step 6: Commit**

```bash
git add tools/fleet-watcher/notify.py tools/fleet-watcher/delivery.py tools/fleet-watcher/tests/test_notify.py
git commit -m "feat(watcher): pushover notifier and outbox delivery worker"
```

---

### Task 7: Heartbeat with three gates

**Files:**
- Create: `tools/fleet-watcher/heartbeat.py`
- Test: `tools/fleet-watcher/tests/test_heartbeat.py`

**Interfaces:**
- Consumes: `Store` (Task 4).
- Produces: `should_ping(store, cycle_committed, worker_alive, now, deadline) -> bool` and `Heartbeat(url).ping()`. Used by Task 9.

- [ ] **Step 1: Write the failing test**

```python
# tools/fleet-watcher/tests/test_heartbeat.py
import os
import tempfile
import unittest

from heartbeat import should_ping
from store import Store


class TestShouldPing(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.store = Store(self.path)

    def tearDown(self):
        os.unlink(self.path)

    def test_pings_when_everything_is_healthy(self):
        self.assertTrue(should_ping(self.store, cycle_committed=True,
                                    worker_alive=True, now=1.0, deadline=300.0))

    def test_failed_cycle_suppresses_ping(self):
        self.assertFalse(should_ping(self.store, cycle_committed=False,
                                     worker_alive=True, now=1.0, deadline=300.0))

    def test_dead_delivery_worker_suppresses_ping(self):
        """The failure this gating exists for: collection fine, alerting dead."""
        self.assertFalse(should_ping(self.store, cycle_committed=True,
                                     worker_alive=False, now=1.0, deadline=300.0))

    def test_overdue_emergency_item_suppresses_ping(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.assertFalse(should_ping(self.store, cycle_committed=True,
                                     worker_alive=True, now=1e6, deadline=300.0))

    def test_an_emergency_within_its_deadline_does_not_suppress_ping(self):
        """Pins the deadline term. Neutering it to 0.0 left every other test in
        this file green — the gate-as-constant regression already fixed once in
        the store, undetected here."""
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.assertTrue(should_ping(self.store, cycle_committed=True,
                                    worker_alive=True, now=1.0, deadline=300.0))

    def test_a_bool_now_fails_closed(self):
        """`True` is an int subclass, so a plain isinstance check lets it
        through and silently means "1 second since the epoch"."""
        self.assertFalse(should_ping(self.store, cycle_committed=True,
                                     worker_alive=True, now=True, deadline=300.0))

    def test_an_undeliverable_canary_suppresses_the_ping(self):
        """Closes the loop: without this the canary exercises the alert path
        but nothing reads the result, so a dead path stays invisible."""
        self.store.enqueue_canary()
        self.assertTrue(should_ping(self.store, cycle_committed=True,
                                    worker_alive=True, now=1.0, deadline=300.0))
        self.assertFalse(should_ping(self.store, cycle_committed=True,
                                     worker_alive=True, now=1e7, deadline=300.0))

    def test_a_delivered_canary_does_not_suppress_the_ping(self):
        self.store.enqueue_canary()
        pending = [i for i in self.store.pending_outbox(now=0.0)
                   if i.rule == "canary"]
        self.store.mark_sent(pending[0].outbox_id)
        self.assertTrue(should_ping(self.store, cycle_committed=True,
                                    worker_alive=True, now=1e7, deadline=300.0))

    def test_a_non_numeric_now_fails_closed(self):
        """A None `now` makes the SQL comparison NULL, which reads as "nothing
        overdue" and pings. A dead-man must never fail open."""
        self.assertFalse(should_ping(self.store, cycle_committed=True,
                                     worker_alive=True, now=None, deadline=300.0))

    def test_unreachable_nodes_do_not_suppress_ping(self):
        """The heartbeat means the watcher works, not that the fleet is healthy."""
        self.store.open_incident("majority_unreachable", ("a", "b"), "normal", "down")
        self.assertTrue(should_ping(self.store, cycle_committed=True,
                                    worker_alive=True, now=1.0, deadline=300.0))


class TestHeartbeatPing(unittest.TestCase):
    """The Heartbeat class had no coverage; ping() is the half that talks to
    the network, and its exception set is exactly what Task 6 had to fix."""

    class _Response:
        def __init__(self, status):
            self.status = status

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

    def _with_urlopen(self, fake):
        import heartbeat as hb
        original = hb.urllib.request.urlopen
        hb.urllib.request.urlopen = fake
        self.addCleanup(setattr, hb.urllib.request, "urlopen", original)

    def test_a_2xx_response_is_a_successful_ping(self):
        from heartbeat import Heartbeat
        self._with_urlopen(lambda r, timeout=None: self._Response(200))
        self.assertTrue(Heartbeat("https://example.invalid/hb").ping())

    def test_transport_exceptions_return_false_rather_than_propagating(self):
        import http.client
        import ssl

        from heartbeat import Heartbeat
        for exc in (ConnectionResetError("reset"),
                    http.client.RemoteDisconnected("closed"),
                    ssl.SSLError("handshake"),
                    OSError("unreachable")):
            def boom(request, timeout=None, _e=exc):
                raise _e

            self._with_urlopen(boom)
            self.assertFalse(Heartbeat("https://example.invalid/hb").ping(),
                             f"{type(exc).__name__} must not propagate")

    def test_the_request_carries_no_body(self):
        """Liveness only — no node details, no secrets."""
        captured = {}

        def fake(request, timeout=None):
            captured["data"] = request.data
            captured["method"] = request.get_method()
            return self._Response(200)

        from heartbeat import Heartbeat
        self._with_urlopen(fake)
        Heartbeat("https://example.invalid/hb").ping()
        self.assertIsNone(captured["data"])
        self.assertEqual(captured["method"], "GET")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_heartbeat -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'heartbeat'`

- [ ] **Step 3: Implement the heartbeat**

```python
# tools/fleet-watcher/heartbeat.py
"""External dead-man ping.

Software on this host cannot report that this host has disappeared, so absence
detection lives at an external service.

The ping is gated on the WHOLE alarm path, not just collection. A watcher that
polls and persists happily while its delivery worker is dead is worse than one
that crashed: it looks healthy and will never tell you anything again.
Collection working is not the property worth monitoring — the ability to raise
an alarm is.
"""
from __future__ import annotations

import http.client
import urllib.request

from store import Store


CANARY_MAX_AGE_SECONDS = 2 * 24 * 60 * 60   # two canary intervals of grace


def should_ping(store: Store, cycle_committed: bool, worker_alive: bool,
                now: float, deadline: float,
                canary_max_age: float = CANARY_MAX_AGE_SECONDS) -> bool:
    """`now` MUST come from the same clock the Store was constructed with.

    Mixing domains silently disables the overdue gate — a monotonic `now`
    against wall-clock `created_at` compares nonsense, and a None `now` makes
    the SQL comparison NULL, which reads as "nothing overdue" and pings. Both
    fail OPEN, which is the wrong direction for a dead-man, so guard first.
    """
    if not isinstance(now, (int, float)) or isinstance(now, bool):
        return False
    if not cycle_committed:
        return False
    if not worker_alive:
        return False
    if store.has_overdue_critical(now=now, deadline=deadline):
        return False
    if store.has_stale_canary(now=now, max_age=canary_max_age):
        # The canary is the only proactive test of the alert path. If it cannot
        # be delivered, alerting is broken even though nothing has failed yet,
        # and the operator must find out from the dead-man rather than from the
        # next real incident going unreported.
        return False
    return True


class Heartbeat:
    """Carries no node details and no secrets — liveness only."""

    def __init__(self, url: str, timeout: float = 10.0) -> None:
        self._url = url
        self._timeout = timeout

    def ping(self) -> bool:
        try:
            request = urllib.request.Request(self._url, method="GET")
            with urllib.request.urlopen(request, timeout=self._timeout) as response:
                return 200 <= response.status < 300
        except (OSError, http.client.HTTPException, ValueError):
            # Same set as notify.py, for the same reason: urllib wraps only
            # errors from h.request(), so getresponse() propagates raw
            # ConnectionResetError, RemoteDisconnected and ssl.SSLError. A
            # routine keep-alive blip must not raise out of the last line of
            # the cycle and take the watcher down with it.
            return False
```

- [ ] **Step 4: Run the tests**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_heartbeat -v`
Expected: PASS (report the actual count)

- [ ] **Step 5: Commit**

```bash
git add tools/fleet-watcher/heartbeat.py tools/fleet-watcher/tests/test_heartbeat.py
git commit -m "feat(watcher): dead-man heartbeat gated on the whole alarm path"
```

---

### Task 8: Config and two-stage poller

**Files:**
- Create: `tools/fleet-watcher/config.py`, `tools/fleet-watcher/poller.py`
- Test: `tools/fleet-watcher/tests/test_poller.py`
- Test: `tools/fleet-watcher/tests/test_config.py`
- Create: `tools/fleet-watcher/deploy/config.example.json`
- Create: `tools/fleet-watcher/.gitignore` containing `deploy/config.json` — the real
  inventory must never be committed, and convention alone has not been enough anywhere else
  in this repo

**Interfaces:**
- Consumes: `Observation` (Task 1).
- **Requires of the RPC object** (implemented in Task 9): `call(node, method, params=None)`
  returning the parsed JSON-RPC reply, AND `restart_id(node)` returning a process-identity
  string or `None`. `restart_id` is easy to overlook because the poller wraps it in a bare
  except — if Task 9 omits it, `restart_id` stays `None` forever and `node_restart` silently
  never fires. It is part of the contract, not an optional extra.
- Produces: `load_config(path) -> Config` with `.nodes` (each `name`, `role`, `transport`, `target`); `poll_cycle(nodes, rpc, cycle_id, now_iso) -> list[Observation]`; `parse_safe_mode(response) -> str`. Used by Task 9.

- [ ] **Step 1: Write the failing test**

```python
# tools/fleet-watcher/tests/test_poller.py
import unittest

from poller import parse_safe_mode, comparison_heights, poll_cycle


class FakeRPC:
    """Records calls and replays canned responses keyed by (node, method)."""

    def __init__(self, responses):
        self.responses = responses
        self.calls = []

    def call(self, node, method, params=None):
        self.calls.append((node, method, tuple(params or ())))
        value = self.responses.get((node, method))
        if isinstance(value, Exception):
            raise value
        return value


class TestParseSafeMode(unittest.TestCase):
    def test_active_is_active(self):
        self.assertEqual(
            parse_safe_mode({"result": {"active": True, "reason": "bad root"}}),
            ("active", "bad root"))

    def test_inactive_is_inactive(self):
        self.assertEqual(
            parse_safe_mode({"result": {"active": False, "reason": ""}}),
            ("inactive", None))

    def test_method_not_found_is_unknown_never_inactive(self):
        """An older daemon must not read as healthy."""
        response = {"error": {"code": -32601, "message": "Method not found"}}
        self.assertEqual(parse_safe_mode(response), ("unknown", None))

    def test_malformed_response_is_unknown(self):
        self.assertEqual(parse_safe_mode({"result": {}}), ("unknown", None))
        self.assertEqual(parse_safe_mode(None), ("unknown", None))


class TestComparisonHeights(unittest.TestCase):
    def test_pairwise_minimum_for_voters(self):
        heights = {"a": 100, "b": 105, "c": 90}
        self.assertEqual(comparison_heights(heights, {}), {
            "a": {90, 100}, "b": {90, 100}, "c": {90},
        })

    def test_observer_pairs_with_every_voter_on_both_sides(self):
        """The comparison the rules actually make is observer-vs-MEMBER at the
        pairwise minimum. Asking only the observer, or asking against a median,
        leaves every such comparison UNDETERMINED and makes a forked observer
        invisible."""
        result = comparison_heights({"a": 100, "b": 90}, {"w": 95})
        self.assertEqual(result["w"], {90, 95})
        self.assertIn(95, result["a"], "the voter needs the observer's height too")
        self.assertIn(90, result["b"])


class TestPollCycle(unittest.TestCase):
    def test_stage_two_fetches_both_sides_of_every_comparison(self):
        """Stage 2's entire purpose is WHICH (node, height) pairs get fetched.
        Asserting only on the returned observations cannot see this."""
        nodes = [{"name": "a", "role": "voting"},
                 {"name": "b", "role": "voting"},
                 {"name": "w", "role": "observer"}]
        heights = {"a": 100, "b": 90, "w": 95}
        responses = {}
        for name, height in heights.items():
            responses[(name, "getdaemonstatus")] = {"result": {"height": height}}
            responses[(name, "blockchain.getbestblockhash")] = {"result": f"H{height}"}
        rpc = FakeRPC(responses)
        poll_cycle(nodes, rpc, "c1", "2026-08-06T00:00:00Z")

        asked = {(n, p[0]) for n, m, p in rpc.calls
                 if m == "blockchain.getblockhash"}
        # a-b pair at 90; w-a pair at 95; w-b pair at 90 — both sides each.
        for pair in (("a", 90), ("b", 90), ("w", 95), ("a", 95), ("w", 90)):
            self.assertIn(pair, asked, f"missing comparison hash for {pair}")


    def test_unreachable_node_still_produces_an_observation(self):
        nodes = [{"name": "a", "role": "voting"}]
        rpc = FakeRPC({("a", "getdaemonstatus"): TimeoutError("no route")})
        result = poll_cycle(nodes, rpc, "c1", "2026-08-06T00:00:00Z")
        self.assertEqual(len(result), 1)
        self.assertFalse(result[0].reachable)
        self.assertEqual(result[0].safe_mode, "unknown")

    def test_reachable_means_core_rpc_answered_not_every_field(self):
        nodes = [{"name": "a", "role": "voting"}]
        rpc = FakeRPC({
            ("a", "getdaemonstatus"): {"result": {"height": 100}},
            ("a", "blockchain.getbestblockhash"): {"result": "H100"},
            ("a", "safemode.status"): {"error": {"code": -32601}},
        })
        result = poll_cycle(nodes, rpc, "c1", "2026-08-06T00:00:00Z")
        self.assertTrue(result[0].reachable, "core RPCs answered")
        self.assertEqual(result[0].safe_mode, "unknown")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_poller -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'poller'`

- [ ] **Step 3: Implement config.py**

```python
# tools/fleet-watcher/config.py
"""Configuration loading.

Fleet inventory is configuration, never code, and is never committed.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any, Dict, List


@dataclass(frozen=True)
class Config:
    nodes: List[Dict[str, Any]]
    db_path: str
    cycle_seconds: int
    open_after: int
    close_after: int
    node_behind_blocks: int
    overdue_deadline_seconds: float

    @property
    def voting_total(self) -> int:
        return sum(1 for n in self.nodes if n["role"] == "voting")


TRANSPORTS = ("ssh", "local")


def load_config(path: str) -> Config:
    """Validate loudly at load time.

    A malformed inventory that starts anyway becomes a runtime failure on the
    one host that matters, and several of these mistakes are indistinguishable
    from real fleet problems once the watcher is running — a duplicate node
    name, for instance, makes voting_total exceed the observations and raises a
    permanent, unclosable telemetry_degraded.
    """
    with open(path, "r", encoding="utf-8") as handle:
        raw = json.load(handle)

    nodes = raw.get("nodes")
    if not isinstance(nodes, list) or not nodes:
        raise ValueError("config must define a non-empty 'nodes' list")

    seen = set()
    for node in nodes:
        for key in ("name", "role", "transport", "target"):
            if key not in node:
                raise ValueError(f"node missing required key {key!r}: {node!r}")
        if node["name"] in seen:
            raise ValueError(f"duplicate node name: {node['name']}")
        seen.add(node["name"])
        if node["role"] not in ("voting", "observer"):
            raise ValueError(f"bad role for {node['name']}: {node['role']}")
        if node["transport"] not in TRANSPORTS:
            raise ValueError(f"bad transport for {node['name']}: {node['transport']}")

    if not any(n["role"] == "voting" for n in nodes):
        raise ValueError("config defines no voting nodes; quorum is impossible")
    return Config(
        nodes=raw["nodes"],
        db_path=raw.get("db_path", "/var/lib/fleet-watcher/watcher.db"),
        cycle_seconds=raw.get("cycle_seconds", 60),
        open_after=raw.get("open_after", 3),
        close_after=raw.get("close_after", 3),
        node_behind_blocks=raw.get("node_behind_blocks", 10),
        overdue_deadline_seconds=raw.get("overdue_deadline_seconds", 300.0),
    )
```

- [ ] **Step 4: Implement poller.py**

```python
# tools/fleet-watcher/poller.py
"""Two-stage poll. Pure I/O — contains no rules.

Stage 1 establishes position. Stage 2 fetches the block hashes needed to
compare nodes at shared heights. Stage 2 exists because quorum cannot be
computed from current tips: comparing tips either demands identical heights
(pages on ordinary propagation) or compares mismatched heights (hides a fork).
"""
from __future__ import annotations

import time
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


CYCLE_BUDGET_SECONDS = 45.0


def poll_cycle(nodes: Sequence[Dict[str, Any]], rpc: Any, cycle_id: str,
               now_iso: str, budget: float = CYCLE_BUDGET_SECONDS,
               monotonic: Any = time.monotonic) -> List[Observation]:
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
    deadline = monotonic() + budget
    for name, heights in needed.items():
        for height in sorted(heights):
            if monotonic() >= deadline:
                # Abandoning stage 2 is safe: an absent hash reads as
                # UNDETERMINED, which raises telemetry_degraded rather than a
                # fork page. Overrunning the cycle is NOT safe — per-call
                # timeouts alone let one hung node push the cycle past the
                # overdue deadline and fire the dead-man from slowness.
                break
            try:
                response = rpc.call(name, "blockchain.getblockhash", [height])
                value = (response or {}).get("result")
                if isinstance(value, str):
                    hashes[name][height] = value
            except Exception:
                continue   # a missing hash is never agreement
        else:
            continue
        break

    return [Observation(
        cycle_id=cycle_id, timestamp=now_iso, node=name, role=e["role"],
        reachable=e["reachable"], height=e["height"], tip_hash=e["tip_hash"],
        hashes_at=hashes.get(name, {}), peers_in=e["peers_in"],
        peers_out=e["peers_out"], synced=e["synced"], safe_mode=e["safe_mode"],
        safe_mode_reason=e["safe_mode_reason"], restart_id=e["restart_id"],
    ) for name, e in stage1.items()]
```

- [ ] **Step 4b: Write the config validation tests**

Validation that is never exercised is indistinguishable from validation that
does not work. Each case below is a real mistake with a real downstream effect —
a duplicate node name in particular makes `voting_total` exceed the observed
nodes and raises a permanent, unclosable `telemetry_degraded` that looks like a
fleet fault rather than a typo.

```python
# tools/fleet-watcher/tests/test_config.py
import json
import os
import tempfile
import unittest

from config import load_config

VALID = {
    "nodes": [
        {"name": "a", "role": "voting", "transport": "ssh", "target": "w@a"},
        {"name": "b", "role": "voting", "transport": "local", "target": "127.0.0.1:1"},
        {"name": "w", "role": "observer", "transport": "ssh", "target": "w@w"},
    ]
}


class TestLoadConfig(unittest.TestCase):
    def _write(self, payload):
        fd, path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as handle:
            json.dump(payload, handle)
        self.addCleanup(os.unlink, path)
        return path

    def _rejects(self, payload, fragment):
        with self.assertRaises(ValueError) as caught:
            load_config(self._write(payload))
        self.assertIn(fragment, str(caught.exception).lower())

    def test_a_valid_config_loads_and_counts_voters(self):
        config = load_config(self._write(VALID))
        self.assertEqual(len(config.nodes), 3)
        self.assertEqual(config.voting_total, 2)

    def test_duplicate_node_names_are_rejected(self):
        payload = {"nodes": [dict(VALID["nodes"][0]), dict(VALID["nodes"][0])]}
        self._rejects(payload, "duplicate")

    def test_an_empty_node_list_is_rejected(self):
        self._rejects({"nodes": []}, "non-empty")

    def test_a_config_with_no_voting_nodes_is_rejected(self):
        payload = {"nodes": [dict(VALID["nodes"][2])]}
        self._rejects(payload, "voting")

    def test_an_unknown_transport_is_rejected(self):
        payload = {"nodes": [{**VALID["nodes"][0], "transport": "carrier-pigeon"}]}
        self._rejects(payload, "transport")

    def test_an_unknown_role_is_rejected(self):
        payload = {"nodes": [{**VALID["nodes"][0], "role": "auditor"}]}
        self._rejects(payload, "role")

    def test_a_missing_required_key_is_rejected(self):
        payload = {"nodes": [{"name": "a", "role": "voting"}]}
        self._rejects(payload, "missing")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 5: Write the example config**

```json
{
  "nodes": [
    {"name": "node-a", "role": "voting", "transport": "ssh", "target": "watcher@example-a"},
    {"name": "node-b", "role": "voting", "transport": "ssh", "target": "watcher@example-b"},
    {"name": "node-c", "role": "voting", "transport": "local", "target": "127.0.0.1:20998"},
    {"name": "node-d", "role": "observer", "transport": "ssh", "target": "watcher@example-d"}
  ],
  "db_path": "/var/lib/fleet-watcher/watcher.db",
  "cycle_seconds": 60,
  "open_after": 3,
  "close_after": 3,
  "node_behind_blocks": 10,
  "overdue_deadline_seconds": 300.0
}
```

- [ ] **Step 6: Run the tests**

Run: `cd tools/fleet-watcher && python3 -m unittest tests.test_poller -v`
Expected: PASS (report the actual count)

- [ ] **Step 7: Commit**

```bash
git add tools/fleet-watcher/config.py tools/fleet-watcher/poller.py tools/fleet-watcher/tests/test_poller.py tools/fleet-watcher/deploy/config.example.json
git commit -m "feat(watcher): config and two-stage poller with comparison hashes"
```

---

### Task 9: RPC transport and main loop

**Files:**
- Create: `tools/fleet-watcher/watcher.py`
- Modify: `tools/fleet-watcher/poller.py` (add `SSHRPC`)

**Interfaces:**
- Consumes: everything from Tasks 1–8.
- Produces: `main(argv)` entrypoint. Consumed by the systemd unit in Task 10.

- [ ] **Step 1: Add the SSH transport to poller.py**

```python
import json
import shlex
import subprocess
import tempfile
import urllib.request


class SSHRPC:
    """Calls a node's loopback RPC through a forced-command read-only wrapper.

    No shell, no port/agent/X11 forwarding, strict host-key checking, and a hard
    timeout so one hung node cannot stall the cycle.
    """

    # ControlMaster reuses one connection per node for the whole cycle window.
    # Without it every RPC is a fresh handshake — ~30 per cycle for a small
    # fleet — which is most of the cycle's wall clock and all of its variance.
    SSH_BASE = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
                "-o", "ClearAllForwardings=yes", "-o", "ConnectTimeout=8",
                "-o", "ControlMaster=auto", "-o", "ControlPersist=90",
                "-T", "-n"]

    def __init__(self, nodes, timeout: float = CORE_TIMEOUT,
                 control_dir: Optional[str] = None) -> None:
        self._targets = {n["name"]: n for n in nodes}
        self._timeout = timeout
        self._control_dir = control_dir or tempfile.mkdtemp(prefix="fleet-watcher-ssh-")

    def _ssh_argv(self, target: str):
        return self.SSH_BASE + [
            "-o", f"ControlPath={self._control_dir}/%r@%h:%p", target]

    def call(self, node: str, method: str, params=None):
        entry = self._targets[node]
        payload = json.dumps({"jsonrpc": "2.0", "id": "w",
                              "method": method, "params": list(params or ())})
        if entry.get("transport") == "local":
            # stdlib, not curl. The watcher has no third-party dependencies and
            # should not acquire one through a subprocess either — a missing
            # curl would fail at runtime on the one host that matters.
            request = urllib.request.Request(
                f"http://{entry['target']}/", data=payload.encode(),
                headers={"Content-Type": "application/json"}, method="POST")
            with urllib.request.urlopen(request, timeout=self._timeout) as response:
                return json.loads(response.read())

        command = self._ssh_argv(entry["target"]) + ["rpc", shlex.quote(payload)]
        result = subprocess.run(command, capture_output=True, timeout=self._timeout)
        if result.returncode != 0:
            raise RuntimeError(f"{node}: transport failed")
        return json.loads(result.stdout.decode())

    def restart_id(self, node: str) -> Optional[str]:
        """systemd InvocationID — never locale-formatted ps output.

        A non-zero exit yields None, not stdout. Without that check any output
        on failure becomes a stable but bogus identity, which would mask every
        real restart forever — worse than having no signal, because
        `node_restart` would look like it was working.
        """
        entry = self._targets[node]
        if entry.get("transport") == "local":
            unit = entry.get("unit", "dinerod")
            command = ["systemctl", "show", unit, "-p", "InvocationID", "--value"]
        else:
            command = self._ssh_argv(entry["target"]) + ["invocation-id"]
        try:
            result = subprocess.run(command, capture_output=True,
                                    timeout=self._timeout)
        except (subprocess.SubprocessError, OSError):
            return None
        if result.returncode != 0:
            return None
        return result.stdout.decode().strip() or None
```

- [ ] **Step 2: Implement watcher.py**

```python
#!/usr/bin/env python3
"""Fleet watcher entrypoint.

One cycle: poll -> commit atomically -> evaluate rules -> apply thresholds ->
drain the outbox -> ping the dead-man only if the whole alarm path is healthy.
"""
from __future__ import annotations

import argparse
import os
import sys
import time
import uuid
from datetime import datetime, timezone

from config import load_config
from delivery import canary_due, drain
from engine import Engine
from heartbeat import Heartbeat, should_ping
from notify import PushoverNotifier
from poller import SSHRPC, poll_cycle
from rules import evaluate
from store import Store


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def run_once(config, store, engine, rpc, notifier, heartbeat, previous):
    cycle_id = uuid.uuid4().hex
    observations = poll_cycle(config.nodes, rpc, cycle_id, _now_iso())

    committed = True
    try:
        store.write_cycle(observations)
    except Exception as exc:            # noqa: BLE001 - must not kill the loop
        committed = False
        print(f"[watcher] cycle commit failed: {exc}", file=sys.stderr)

    if committed:
        engine.process(evaluate(observations, config.voting_total, previous,
                                node_behind_blocks=config.node_behind_blocks))

    worker_alive = True
    try:
        drain(store, notifier, now=time.time(),
              maintenance=os.environ.get("WATCHER_MAINTENANCE") == "1")
    except Exception as exc:            # noqa: BLE001
        worker_alive = False
        print(f"[watcher] delivery worker failed: {exc}", file=sys.stderr)

    # Exercise the real alert path on a schedule, so a bad credential is
    # discovered before an incident needs it rather than during one.
    try:
        if canary_due(store, time.time()):
            store.enqueue_canary()
    except Exception as exc:            # noqa: BLE001
        print(f"[watcher] canary enqueue failed: {exc}", file=sys.stderr)

    if heartbeat and should_ping(store, cycle_committed=committed,
                                 worker_alive=worker_alive, now=time.time(),
                                 deadline=config.overdue_deadline_seconds):
        heartbeat.ping()

    return observations if committed else previous


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Dinero fleet watcher")
    parser.add_argument("--config", required=True)
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args(argv)

    config = load_config(args.config)
    store = Store(config.db_path)
    engine = Engine(store, open_after=config.open_after,
                    close_after=config.close_after)
    rpc = SSHRPC(config.nodes)

    token = os.environ.get("PUSHOVER_TOKEN")
    user = os.environ.get("PUSHOVER_USER")
    if not token or not user:
        print("[watcher] PUSHOVER_TOKEN/PUSHOVER_USER not set", file=sys.stderr)
        return 2
    notifier = PushoverNotifier(token, user)

    hb_url = os.environ.get("HEARTBEAT_URL")
    heartbeat = Heartbeat(hb_url) if hb_url else None
    if heartbeat is None:
        print("[watcher] HEARTBEAT_URL not set — dead-man disabled", file=sys.stderr)

    previous = None
    while True:
        previous = run_once(config, store, engine, rpc, notifier, heartbeat, previous)
        if args.once:
            return 0
        time.sleep(config.cycle_seconds)


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2b: Test the cycle loop's three binding behaviours**

`run_once` had no tests at all, so the three constraints the loop exists to
enforce were unverified. Nothing would have caught a regression that pings the
dead-man after a failed commit.

```python
# append to tools/fleet-watcher/tests/test_watcher.py
import os
import tempfile
import unittest

import watcher
from config import Config
from engine import Engine
from store import Store


class _Rpc:
    def call(self, node, method, params=None):
        if method == "getdaemonstatus":
            return {"result": {"height": 100}}
        if method == "blockchain.getbestblockhash":
            return {"result": "H100"}
        if method == "blockchain.getblockhash":
            return {"result": "H100"}
        return {"result": {}}

    def restart_id(self, node):
        return "r1"


class _Notifier:
    def send(self, title, message, priority):
        return True


class _Heartbeat:
    def __init__(self):
        self.pings = 0

    def ping(self):
        self.pings += 1
        return True


class TestRunOnce(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.addCleanup(os.unlink, self.path)
        self.store = Store(self.path)
        self.engine = Engine(self.store, open_after=3, close_after=3)
        self.config = Config(
            nodes=[{"name": "a", "role": "voting", "transport": "ssh", "target": "t"}],
            db_path=self.path, cycle_seconds=60, open_after=3, close_after=3,
            node_behind_blocks=10, overdue_deadline_seconds=300.0)

    def _run(self, previous=None):
        hb = _Heartbeat()
        result = watcher.run_once(self.config, self.store, self.engine, _Rpc(),
                                  _Notifier(), hb, previous)
        return result, hb

    def test_a_healthy_cycle_pings_once(self):
        _, hb = self._run()
        self.assertEqual(hb.pings, 1)

    def test_a_failed_commit_does_not_ping(self):
        """A watcher that polls but cannot persist is broken, and the dead-man
        is exactly what should notice."""
        def explode(_observations):
            raise RuntimeError("disk full")
        self.store.write_cycle = explode
        _, hb = self._run()
        self.assertEqual(hb.pings, 0)

    def test_a_failed_commit_preserves_the_previous_cycle(self):
        sentinel = ["previous-cycle"]
        def explode(_observations):
            raise RuntimeError("disk full")
        self.store.write_cycle = explode
        result, _ = self._run(previous=sentinel)
        self.assertIs(result, sentinel)

    def test_a_dead_delivery_worker_does_not_ping(self):
        """Collection working is not the property worth monitoring; the
        ability to raise an alarm is."""
        original = watcher.drain
        watcher.drain = lambda *a, **k: (_ for _ in ()).throw(RuntimeError("boom"))
        self.addCleanup(setattr, watcher, "drain", original)
        _, hb = self._run()
        self.assertEqual(hb.pings, 0)
```

- [ ] **Step 3: Verify the whole suite still passes**

Run: `cd tools/fleet-watcher && python3 -m unittest discover -s tests -v`
Expected: PASS (report the actual count)

- [ ] **Step 4: Verify the entrypoint refuses to run without credentials**

Run: `cd tools/fleet-watcher && python3 watcher.py --config deploy/config.example.json --once; echo "exit=$?"`
Expected: `exit=2` and `PUSHOVER_TOKEN/PUSHOVER_USER not set` on stderr

- [ ] **Step 5: Commit**

```bash
git add tools/fleet-watcher/watcher.py tools/fleet-watcher/poller.py
git commit -m "feat(watcher): ssh transport and cycle loop"
```

---

### Task 10: Deployment unit and operator documentation

**Files:**
- Create: `tools/fleet-watcher/deploy/fleet-watcher.service`
- Create: `tools/fleet-watcher/README.md`

**Interfaces:**
- Consumes: `watcher.py` (Task 9).
- Produces: nothing consumed by later tasks. This is the final task.

- [ ] **Step 1: Write the systemd unit**

```ini
# tools/fleet-watcher/deploy/fleet-watcher.service
[Unit]
Description=Dinero fleet watcher
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=fleet-watcher
ExecStart=/usr/bin/python3 /opt/fleet-watcher/watcher.py --config /etc/fleet-watcher/config.json
Restart=always
RestartSec=30

# Secrets are supplied as credentials and never appear in the unit, in Git,
# or in the process environment of anything but this service.
LoadCredential=pushover:/etc/fleet-watcher/pushover.env
LoadCredential=heartbeat:/etc/fleet-watcher/heartbeat.env
EnvironmentFile=/etc/fleet-watcher/pushover.env
EnvironmentFile=/etc/fleet-watcher/heartbeat.env

NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/fleet-watcher

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 2: Write the README**

````markdown
# Fleet Watcher

External fleet observability. Polls each node's loopback RPC on a cycle, records
every observation, and pages only for genuinely dangerous conditions.

Design: `docs/superpowers/specs/2026-08-06-fleet-watcher-design.md`

## Install

```bash
install -d -m 0750 -o fleet-watcher -g fleet-watcher /opt/fleet-watcher /var/lib/fleet-watcher
install -m 0644 tools/fleet-watcher/*.py /opt/fleet-watcher/
install -d -m 0700 /etc/fleet-watcher
install -m 0600 tools/fleet-watcher/deploy/config.example.json /etc/fleet-watcher/config.json
```

Write the two credential files, both `0600` and root-owned:

```
/etc/fleet-watcher/pushover.env     PUSHOVER_TOKEN=... / PUSHOVER_USER=...
/etc/fleet-watcher/heartbeat.env    HEARTBEAT_URL=...
```

Neither belongs in Git. Then:

```bash
cp tools/fleet-watcher/deploy/fleet-watcher.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now fleet-watcher
```

## Node-side access

Each polled node needs an unprivileged account restricted to a forced command:

```
command="/usr/local/bin/fleet-watcher-rpc",no-port-forwarding,no-agent-forwarding,no-X11-forwarding,no-pty ssh-ed25519 AAAA...
```

The wrapper accepts only the read RPCs this tool uses and `invocation-id`. It
must never provide a shell.

**The quoting contract is load-bearing.** The watcher invokes
`rpc <shlex.quoted-json>`, so the wrapper MUST word-split `$SSH_ORIGINAL_COMMAND`
through the shell and read the payload as `$2`:

```sh
#!/bin/sh
# /usr/local/bin/fleet-watcher-rpc — forced command, no shell for the caller.
set -eu
set -- $SSH_ORIGINAL_COMMAND          # deliberate word-splitting: the payload
                                      # arrives shell-quoted from the watcher
case "${1:-}" in
  rpc)
    printf '%s' "$2" | curl -sS --max-time 10 -X POST \
        -H 'Content-Type: application/json' --data-binary @- \
        "http://127.0.0.1:${DINERO_RPC_PORT:?}/"
    ;;
  invocation-id)
    systemctl show "${DINERO_UNIT:-dinerod}" -p InvocationID --value
    ;;
  *)
    echo "refused: ${1:-<empty>}" >&2; exit 64
    ;;
esac
```

A wrapper that instead does `${SSH_ORIGINAL_COMMAND#rpc }` receives the single
quotes literally and every RPC fails to parse — so this must be verified once
at install time, not assumed. Verify with:

```bash
ssh -o BatchMode=yes watcher@<node> "rpc '{\"jsonrpc\":\"2.0\",\"id\":\"t\",\"method\":\"getdaemonstatus\",\"params\":[]}'"
```

Expected: a JSON reply containing `"height"`. Anything else means the quoting
contract is broken and the watcher will report every node unreachable.

## Tests

```bash
cd tools/fleet-watcher && python3 -m unittest discover -s tests -v
```

## Verifying the dead-man switch

**Do this at install time and after any change to the heartbeat path.** An
untested dead-man is indistinguishable from a dead one, and it is the failure
mode that hides all the others.

```bash
systemctl stop fleet-watcher
# wait ~6 minutes
```

Confirm an alert arrives from the dead-man service. Then:

```bash
systemctl start fleet-watcher
```

Confirm the check returns to healthy. If no alert arrived, the heartbeat is not
protecting you and must be fixed before relying on this tool.

## Maintenance windows

Set `WATCHER_MAINTENANCE=1` in the service environment to suppress **delivery**
of `node_behind`, `majority_unreachable` and `telemetry_degraded`.

It can never suppress `safe_mode`, `tip_divergence` or `observer_divergence`.
Incidents are still recorded normally either way.

## Reading the data

```bash
sqlite3 /var/lib/fleet-watcher/watcher.db \
  "SELECT opened_at, rule, nodes, closed_at FROM incidents ORDER BY opened_at DESC LIMIT 20;"
```
````

- [ ] **Step 3: Validate the unit file parses**

Run: `systemd-analyze verify tools/fleet-watcher/deploy/fleet-watcher.service 2>&1 | head`
Expected: no syntax errors (unresolved `User=`/paths are expected off-host)

- [ ] **Step 4: Run the full suite one final time**

Run: `cd tools/fleet-watcher && python3 -m unittest discover -s tests -v`
Expected: PASS (report the actual count)

- [ ] **Step 5: Commit**

```bash
git add tools/fleet-watcher/deploy/fleet-watcher.service tools/fleet-watcher/README.md
git commit -m "feat(watcher): systemd unit and operator documentation"
```

---

## Self-Review

**Spec coverage.** Two-stage poll (Task 8); quorum by pairwise minimum height (Task 2); all seven rules including `observer_divergence` and `node_restart` (Task 3); atomic cycles and the outbox (Task 4); open/close thresholds with `safe_mode` immediate (Task 5); Pushover emergency priority, dedup, recovery, bounded backoff, maintenance silencing (Task 6); three-gate heartbeat (Task 7); access contract in the unit and README (Tasks 9–10); tri-state `safe_mode` and core-RPC `reachable` (Task 8); `restart_id` from systemd (Task 9); inventory as config (Task 8).

**Placeholders.** None. Every step carries runnable code or an exact command.

**Type consistency.** `Observation`, `Quorum`, `RuleHit`, `Incident`, `OutboxItem` are defined once in Task 1 and used unchanged. `compute_quorum`/`compatible` (Task 2) feed `evaluate` (Task 3). `Store` methods used by Tasks 5–7 all exist in Task 4. `Notifier.send` returns `bool` consistently in Tasks 6 and 9.

**Known gap, deliberate:** the node-side forced-command wrapper (`fleet-watcher-rpc`) is specified in the README but not implemented here — it is deployed per node, not part of this repo's build, and its exact allowlist depends on the deployment. It must exist before the watcher can poll anything.

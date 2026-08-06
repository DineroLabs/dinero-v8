# tools/fleet-watcher/models.py
"""Data models shared by every watcher component.

Frozen dataclasses: an Observation is a fact about a moment and must never be
edited after the fact. RuleHit is hashable so a cycle's hits can be compared as
a set, which is how the engine detects "same condition still present".
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, Optional, Tuple

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
    hashes_at: Dict[int, str]
    peers_in: Optional[int]
    peers_out: Optional[int]
    synced: Optional[bool]
    safe_mode: str
    safe_mode_reason: Optional[str]
    restart_id: Optional[str]

    def __post_init__(self) -> None:
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

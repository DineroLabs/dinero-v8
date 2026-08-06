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
from typing import Callable, List, Sequence, Tuple

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

    def has_overdue_critical(self, now: float, deadline: float) -> bool:
        """An emergency notification still unsent past its deadline. This is one
        of the three heartbeat gates: alerting is broken even if polling works."""
        row = self.conn.execute(
            "SELECT 1 FROM outbox WHERE sent_at IS NULL AND priority='emergency' "
            "AND created_at + ? < ? LIMIT 1", (deadline, now)).fetchone()
        return row is not None

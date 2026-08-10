"""Configuration loading.

Fleet inventory is configuration, never code, and is never committed.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any, Dict, List, Optional


@dataclass(frozen=True)
class Config:
    nodes: List[Dict[str, Any]]
    db_path: str
    cycle_seconds: int
    open_after: int
    close_after: int
    node_behind_blocks: int
    overdue_deadline_seconds: float
    remediation_node: Optional[str] = None
    remediation_request_path: Optional[str] = None

    @property
    def voting_total(self) -> int:
        return sum(1 for n in self.nodes if n["role"] == "voting")


TRANSPORTS = ("ssh", "local")
REMEDIATION_REQUEST_PATH = "/run/fleet-watcher/restart-dinero.json"


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

    voters = sum(1 for n in nodes if n["role"] == "voting")
    if voters < 2:
        # A single voter can never reach QUORUM_MIN, so consensus_health would
        # fire every three cycles forever on a perfectly healthy fleet.
        raise ValueError(
            f"config defines {voters} voting node(s); at least 2 are required "
            "to form a quorum")
    remediation = raw.get("remediation")
    remediation_node = None
    remediation_request_path = None
    if remediation is not None:
        if not isinstance(remediation, dict):
            raise ValueError("remediation must be an object")
        remediation_node = remediation.get("local_node")
        remediation_request_path = remediation.get("request_path")
        if not remediation_node or not remediation_request_path:
            raise ValueError("remediation requires local_node and request_path")
        if voters < 3:
            raise ValueError("remediation requires at least 3 voting nodes")
        matches = [n for n in nodes if n["name"] == remediation_node]
        if not matches:
            raise ValueError("remediation local_node is not configured")
        if matches[0]["role"] != "voting" or matches[0]["transport"] != "local":
            raise ValueError("remediation local_node must be a local voting node")
        if remediation_request_path != REMEDIATION_REQUEST_PATH:
            raise ValueError(
                f"remediation request_path must be {REMEDIATION_REQUEST_PATH}")

    return Config(
        nodes=raw["nodes"],
        db_path=raw.get("db_path", "/var/lib/fleet-watcher/watcher.db"),
        cycle_seconds=raw.get("cycle_seconds", 60),
        open_after=raw.get("open_after", 3),
        close_after=raw.get("close_after", 3),
        node_behind_blocks=raw.get("node_behind_blocks", 10),
        overdue_deadline_seconds=raw.get("overdue_deadline_seconds", 300.0),
        remediation_node=remediation_node,
        remediation_request_path=remediation_request_path,
    )

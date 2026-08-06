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


def load_config(path: str) -> Config:
    with open(path, "r", encoding="utf-8") as handle:
        raw = json.load(handle)
    for node in raw["nodes"]:
        if node["role"] not in ("voting", "observer"):
            raise ValueError(f"bad role for {node['name']}: {node['role']}")
    return Config(
        nodes=raw["nodes"],
        db_path=raw.get("db_path", "/var/lib/fleet-watcher/watcher.db"),
        cycle_seconds=raw.get("cycle_seconds", 60),
        open_after=raw.get("open_after", 3),
        close_after=raw.get("close_after", 3),
        node_behind_blocks=raw.get("node_behind_blocks", 10),
        overdue_deadline_seconds=raw.get("overdue_deadline_seconds", 300.0),
    )

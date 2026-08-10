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
from typing import Optional

from config import load_config
from delivery import canary_due, drain
from engine import Engine
from heartbeat import Heartbeat, should_ping
from notify import PushoverNotifier
from poller import SSHRPC, poll_cycle
from remediation import eligible_incident, emit_pending
from rules import evaluate
from store import Store


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _secret(key: str, credential: str) -> Optional[str]:
    """Read a secret from systemd's credential directory, else the environment.

    Credentials are preferred because they are never placed in the process
    environment, so they never appear in /proc/PID/environ. The environment
    fallback keeps the tool runnable outside systemd — tests, a manual --once.

    The credential file uses `KEY=value` lines, so one file can carry several
    related secrets.
    """
    directory = os.environ.get("CREDENTIALS_DIRECTORY")
    if directory:
        try:
            with open(os.path.join(directory, credential), "r",
                      encoding="utf-8") as handle:
                for line in handle:
                    name, _, value = line.partition("=")
                    if name.strip() == key:
                        return value.strip().strip("\"'") or None
        except OSError:
            pass
    return os.environ.get(key)


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
        if config.remediation_node and config.remediation_request_path:
            incident_id = eligible_incident(
                observations, store.open_incidents(), config.remediation_node,
                config.node_behind_blocks)
            if incident_id:
                store.enqueue_remediation(incident_id, config.remediation_node)
            try:
                emit_pending(store, config.remediation_request_path)
            except Exception as exc:  # noqa: BLE001 - retry queued request next cycle
                print(f"[watcher] remediation request failed: {exc}", file=sys.stderr)

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

    # Checked before any persistent state is touched (Store opens/creates the
    # sqlite file+WAL as a side effect). A watcher that starts without a
    # notifier would page nobody, and that must be true even before it has
    # written anything to disk.
    token = _secret("PUSHOVER_TOKEN", "pushover")
    user = _secret("PUSHOVER_USER", "pushover")
    if not token or not user:
        print("[watcher] PUSHOVER_TOKEN/PUSHOVER_USER not set", file=sys.stderr)
        return 2
    notifier = PushoverNotifier(token, user)

    store = Store(config.db_path)
    engine = Engine(store, open_after=config.open_after,
                    close_after=config.close_after)
    rpc = SSHRPC(config.nodes)

    hb_url = _secret("HEARTBEAT_URL", "heartbeat")
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

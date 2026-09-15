#!/usr/bin/env python3
"""Run the real replay lifecycle while its requested source P2P port is busy."""

import os
from pathlib import Path
import random
import socket
import subprocess
import sys


def reserve_source_port():
    # Check the whole five-node port layout together, then retain the source
    # P2P socket. Do not accept connections: the daemon must advertise its
    # actual fallback listener through RPC, and the harness must use it.
    for _ in range(200):
        base = random.randrange(22000, 29000)
        sockets = {}
        try:
            for offset in (n + service for n in range(5) for service in (0, 100, 200)):
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sockets[offset] = sock
                # Match the daemon's wildcard bind. Some platforms can let a
                # wildcard listener coexist with a loopback-only reservation.
                sock.bind(("0.0.0.0", base + offset))
                sock.listen(16)
            source = sockets.pop(100)
            return base, source
        except OSError:
            pass
        finally:
            for sock in sockets.values():
                sock.close()
    raise RuntimeError("could not reserve the replay test's port layout")


def main():
    base, source = reserve_source_port()
    with source:
        env = dict(os.environ, BASE_PORT=str(base), RUN_SCENARIOS="AD")
        print(f"[INFO] occupying requested source P2P port {base + 100}", flush=True)
        result = subprocess.run(
            ["bash", str(Path(__file__).with_name("test_assumeutxo_replay_e2e.sh"))],
            env=env,
            check=False,
        )
    if result.returncode == 0:
        print("[PASS] real replay recovered through the source's fallback P2P listener")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())

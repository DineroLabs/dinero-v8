#!/usr/bin/env python3
"""Manual CSN invalidation must rewind and replay the committed forest.

Ordinary coinbase-only regtest blocks reproduce this independently of shielded
proof formats. Every assertion uses a named block and its committed root.
"""
import base64
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request

binary = os.environ.get(
    "DINEROD", str(Path(__file__).resolve().parents[2] / "build/dinerod")
)
assert Path(binary).is_file(), binary
root = Path(tempfile.mkdtemp(prefix="dinero-csn-disconnect-"))
port_sockets = []
for _ in range(6):
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port_sockets.append(sock)
ports = [sock.getsockname()[1] for sock in port_sockets]
for sock in port_sockets:
    sock.close()
processes = []
success = False


def rpc(which, method, params=None):
    cookie = (root / which / ".cookie").read_bytes().strip()
    port = ports[0 if which == "full" else 3]
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or []}
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/",
        json.dumps(payload).encode(),
        {
            "Content-Type": "application/json",
            "Authorization": "Basic " + base64.b64encode(cookie).decode(),
        },
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        result = json.load(response)
    assert result.get("error") in (None, False), result
    return result["result"]


def wait(predicate):
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, AssertionError):
            pass
        time.sleep(0.2)
    raise RuntimeError("timeout waiting for named tip / RPC readiness")


try:
    for which, index, peer in [("full", 0, 4), ("csn", 3, 1)]:
        datadir = root / which
        datadir.mkdir()
        command = [
            binary, "--regtest", f"--datadir={datadir}",
            f"--rpcport={ports[index]}", f"--port={ports[index + 1]}",
            f"--wallet-socket-port={ports[index + 2]}", "--listen=1", "--utreexo=1",
            f"--connect=127.0.0.1:{ports[peer]}",
            "--utreexo-stateless=1" if which == "csn" else "--utreexo-bridge=1",
        ]
        with (root / f"{which}.log").open("w") as log:
            processes.append(subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT))
        wait(lambda: rpc(which, "getblockcount") >= 0)

    address = rpc("full", "wallet.getnewaddress", ["taproot", "disconnect-repro"])
    if isinstance(address, dict):
        address = address["address"]
    rpc("full", "generatetoaddress", [5, address])
    tip = rpc("full", "getbestblockhash")
    wait(lambda: rpc("csn", "getbestblockhash") == tip)
    before = rpc("csn", "blockchain.getutreexocommitment")
    assert before["commitment"] == rpc("full", "blockchain.getutreexocommitment")["commitment"]

    for cycle, parent_height in enumerate((4, 2, 0)):
        parent = rpc("full", "getblockhash", [parent_height])
        invalidated = rpc("full", "getblockhash", [parent_height + 1])
        expected = rpc("full", "getblock", [parent, 1])["utreexocommitment_raw"]
        rpc("csn", "blockchain.invalidateblock", [invalidated])
        after = rpc("csn", "blockchain.getutreexocommitment")
        receipt = {
            "binary": binary, "cycle": cycle, "before": before, "after": after,
            "expected_parent_commitment": expected, "tip": rpc("csn", "getbestblockhash"),
        }
        (root / f"receipt-{cycle}.json").write_text(json.dumps(receipt, indent=2))
        print(json.dumps(receipt), flush=True)
        assert receipt["tip"] == parent
        assert after["verified_height"] == parent_height
        assert after["commitment"] == expected, "manual CSN rollback left accumulator ahead of tip"
        rpc("csn", "blockchain.reconsiderblock", [invalidated])
        wait(lambda: rpc("csn", "getbestblockhash") == tip)
        assert rpc("csn", "blockchain.getutreexocommitment")["commitment"] == before["commitment"], (
            "reconsider did not replay the forest"
        )
        print(f"PASS manual CSN rollback/replay cycle {cycle + 1}", flush=True)
    success = True
finally:
    for process in processes:
        process.terminate()
    for process in processes:
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    if success:
        shutil.rmtree(root)
    else:
        print("retained failure evidence", root, flush=True)

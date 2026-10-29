#!/usr/bin/env python3
"""Actual isolated daemon startup must reject an unavailable RPC listener."""
import base64
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

binary = str(Path(sys.argv[1]).resolve())
root = Path(tempfile.mkdtemp(prefix="dinero-rpc-listener-"))
process = None
success = False
sockets = []
try:
    for _ in range(3):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sock.listen(1)
        sockets.append(sock)
    rpc_port, p2p_port, wallet_port = [sock.getsockname()[1] for sock in sockets]
    # Keep the RPC listener occupied throughout the first startup. The other
    # two distinct ports are only hints; no port-contention retry masks failure.
    for sock in sockets[1:]:
        sock.close()
    datadir = root / "node"
    datadir.mkdir()
    config = datadir / "dinero.conf"
    config.write_text("p2p.offline=true\n")
    command = [binary, "--regtest", f"--datadir={datadir}", f"--conf={config}",
               f"--rpcport={rpc_port}", f"--port={p2p_port}",
               f"--wallet-socket-port={wallet_port}", "--listen=0", "--utreexo=1"]
    failed_log = root / "occupied.log"
    with failed_log.open("w") as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 90
        while process.poll() is None and time.monotonic() < deadline:
            text = failed_log.read_text(errors="replace")
            assert "RPC server ready at" not in text, "occupied RPC listener falsely reported ready"
            time.sleep(0.05)
        assert process.poll() is not None, "occupied RPC listener did not fail startup"
        assert process.returncode != 0, "occupied RPC listener returned successful startup"
    text = failed_log.read_text(errors="replace")
    assert "Failed to bind socket to 127.0.0.1:" in text, "missing actual bind failure"
    assert "[RPCService] Failed to start: RPC listener bind/listen failed" in text, "bind failure did not reach RPCService"
    assert "RPC server ready at" not in text, "occupied RPC listener falsely reported ready"
    assert "HTTP RPC server started on" not in text, "occupied listener announced startup"
    process = None
    print("PASS occupied RPC listener rejects actual daemon startup", flush=True)
    sockets[0].close()

    # The failed startup's actual wallet/chain stores must reopen normally once
    # the same configured port is available. Verify a real cookie-authenticated
    # request, then clean shutdown; do not treat a log line as readiness.
    ready_log = root / "available.log"
    def rpc(method):
        cookies = [datadir / ".cookie", datadir / "regtest" / ".cookie"]
        cookie = next(path.read_text().strip() for path in cookies if path.exists())
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": []}).encode()
        request = urllib.request.Request(f"http://127.0.0.1:{rpc_port}/", body,
            {"Authorization": "Basic " + base64.b64encode(cookie.encode()).decode(), "Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=2) as response:
            result = json.load(response)
        assert result.get("error") in (None, False), result
        return result["result"]
    with ready_log.open("w") as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 90
        ready = False
        while time.monotonic() < deadline:
            assert process.poll() is None, "available RPC listener exited before readiness"
            try:
                ready = rpc("getblockcount") == 0
            except (OSError, StopIteration, ValueError):
                pass
            if ready:
                break
            time.sleep(0.1)
        assert ready, "available RPC listener did not answer authenticated request"
        print("PASS same port and datadir reopen with authenticated RPC", flush=True)
        rpc("stop")
        assert process.wait(timeout=90) == 0, "available RPC listener failed clean shutdown"
    process = None
    success = True
    print("PASS RPC listener startup and clean shutdown", flush=True)
finally:
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    for sock in sockets:
        sock.close()
    if success and os.environ.get("DINERO_KEEP_RPC_STARTUP_EVIDENCE") != "1":
        shutil.rmtree(root)
    else:
        print(f"Retained RPC startup evidence: {root}", flush=True)

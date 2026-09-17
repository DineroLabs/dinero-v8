#!/usr/bin/env python3
"""Check CLI isolation and explicit rejection by a build without compact support."""
import base64
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

experimental, ordinary, raw_path = sys.argv[1:]
raw = Path(raw_path).read_text().strip()
assert raw.startswith("06000040"), "requires real compact wire bytes"
with tempfile.TemporaryDirectory(prefix="dinero-compact-gates-") as temporary:
    root = Path(temporary)
    cases = [
        ([], "124", "requires REGTEST"),
        (["--testnet"], "124", "requires REGTEST"),
        (["--regtest"], "124", "requires REGTEST"),
        (["--regtest", "--consensus-shielded-epoch-reset-height=1",
          "--consensus-shielded-spend-auth-height=2"], "2", "after the Auth reset"),
    ]
    cases += [(["--regtest"], value, "Invalid compact regtest height")
              for value in ("-1", "4294967295", "4294967296", "124x", "garbage")]
    for index, (network, height, expected) in enumerate(cases):
        command = [experimental, f"--datadir={root / str(index)}", *network,
                   f"--consensus-shielded-compact-height={height}"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=15)
        output = result.stdout + result.stderr
        assert result.returncode != 0 and expected in output, (command, output)

    disabled = subprocess.run(
        [ordinary, "--regtest", f"--datadir={root / 'disabled-flag'}",
         "--consensus-shielded-compact-height=124"],
        capture_output=True, text=True, timeout=15)
    assert disabled.returncode != 0 and "not compiled" in disabled.stderr, disabled

    # The normal build must reject a valid compact envelope independently of
    # an activation-height choice. Do not connect it to another network.
    sockets = [socket.socket() for _ in range(3)]
    for sock in sockets:
        sock.bind(("127.0.0.1", 0))
    ports = [sock.getsockname()[1] for sock in sockets]
    for sock in sockets:
        sock.close()
    datadir = root / "ordinary"
    datadir.mkdir()
    command = [ordinary, "--regtest", f"--datadir={datadir}", "--listen=0",
               f"--rpcport={ports[0]}", f"--port={ports[1]}",
               f"--wallet-socket-port={ports[2]}"]
    with (root / "ordinary.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    def rpc(method, params):
        cookie = (datadir / ".cookie").read_bytes().strip()
        request = urllib.request.Request(
            f"http://127.0.0.1:{ports[0]}/",
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                        "params": params}).encode(),
            {"Content-Type": "application/json",
             "Authorization": "Basic " + base64.b64encode(cookie).decode()})
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    try:
        deadline = time.monotonic() + 60
        while True:
            assert process.poll() is None, (root / "ordinary.log").read_text()
            try:
                before = rpc("getbestblockhash", [])
                if before.get("result"):
                    break
            except OSError:
                pass
            assert time.monotonic() < deadline, "ordinary node RPC timeout"
            time.sleep(0.2)
        response = rpc("sendrawtransaction", [raw])
        assert response.get("error"), response
        message = json.dumps(response["error"]).lower()
        assert "deserial" in message or "decode" in message or "version" in message, response
        assert rpc("getbestblockhash", []) == before
        assert rpc("getrawmempool", []).get("result") == []
        print("PASS ordinary build rejects real compact wire bytes:", response["error"])
    finally:
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError("ordinary node required forced cleanup")
    print("PASS compact CLI network, activation and integer bounds")

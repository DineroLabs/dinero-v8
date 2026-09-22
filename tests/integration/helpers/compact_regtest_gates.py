#!/usr/bin/env python3
"""Check CLI isolation and contextual rejection before compact activation."""
import argparse
import base64
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("experimental")
parser.add_argument("ordinary")
parser.add_argument("raw_path")
parser.add_argument("--ordinary-only", action="store_true",
                    help="test default-build support and dormant rejection without an override-enabled binary")
args = parser.parse_args()
experimental, ordinary, raw_path = args.experimental, args.ordinary, args.raw_path
raw = Path(raw_path).read_text().strip()
assert raw.startswith("06000000"), "requires real production-v6 compact wire bytes"
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
    for index, (network, height, expected) in enumerate([] if args.ordinary_only else cases):
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

    # An ordinary build recognizes the format, but no activation is scheduled
    # here. Reject by the consensus gate, not by omitting the wire decoder.
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
               f"--wallet-socket-port={ports[2]}", "--regtest-enforce-pow",
               "--consensus-sixty-second-height=4"]
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
        info = rpc("getconsensusinfo", [])["result"]
        assert info["shielded_compact_supported"] is True, info
        assert info["shielded_compact_version"] == 6, info
        assert info["shielded_compact_proof_encoding"] == "DZE1/v1", info
        assert info["shielded_compact_activation_height"] == 4294967295, info
        assert info["shielded_compact_active"] is False, info
        response = rpc("sendrawtransaction", [raw])
        assert response.get("error"), response
        message = json.dumps(response["error"]).lower()
        assert "compact-shielded-not-active-or-malformed" in message, response
        assert rpc("getbestblockhash", []) == before
        assert rpc("getrawmempool", []).get("result") == []
        print("PASS ordinary build decodes and contextually rejects pre-activation compact:", response["error"])
        # This exact block was accepted by an older ordinary node. Its
        # unreserved transparent version must never be reinterpreted as a
        # shielded transaction by the production build.
        fixture = Path(__file__).resolve().parents[3] / "tests/vectors/compact_v6_v1/legacy-version-block.hex"
        historical = fixture.read_text().strip()
        submitted = rpc("submitblock", [historical])
        assert not submitted.get("error"), submitted
        deadline = time.monotonic() + 10
        while rpc("getblockcount", []).get("result") != 1:
            assert time.monotonic() < deadline, (root / "ordinary.log").read_text()
            time.sleep(.1)
        import hashlib
        block_hash = hashlib.sha256(hashlib.sha256(bytes.fromhex(historical)[:128]).digest()).hexdigest()
        assert rpc("getbestblockhash", []).get("result") == block_hash
        assert rpc("getblock", [block_hash, 0]).get("result") == historical
        print("PASS historical transparent-version block connects and reads back byte-identically")
    finally:
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError("ordinary node required forced cleanup")
    print("PASS default-build activation isolation" if args.ordinary_only else
          "PASS compact CLI network, activation and integer bounds")

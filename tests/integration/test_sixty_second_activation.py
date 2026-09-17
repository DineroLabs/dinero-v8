#!/usr/bin/env python3
"""Exercise the dormant timing/reward upgrade through real full and CSN daemons.

Regtest bypasses ASERT: this qualifies activation/state transitions, not cadence.
The non-regtest DAAGoldenVectors test covers the actual difficulty paths.
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

binary = os.environ.get("DINEROD", str(Path(__file__).resolve().parents[2] / "build/dinerod"))
root = Path(tempfile.mkdtemp(prefix="dinero-sixty-second-"))
sockets = [socket.socket() for _ in range(6)]
for sock in sockets:
    sock.bind(("127.0.0.1", 0))
ports = [sock.getsockname()[1] for sock in sockets]
for sock in sockets:
    sock.close()
processes = {}
success = False


def rpc(which, method, params=None):
    cookie = (root / which / ".cookie").read_bytes().strip()
    port = ports[0 if which == "full" else 3]
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/",
        json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or []}).encode(),
        {"Content-Type": "application/json", "Authorization": "Basic " + base64.b64encode(cookie).decode()},
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        result = json.load(response)
    assert result.get("error") in (None, False), result
    return result["result"]


def wait(predicate):
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, AssertionError):
            pass
        time.sleep(0.2)
    raise RuntimeError("timeout waiting for RPC / named chain tip")


def start(which, extra=()):
    index, peer = (0, 4) if which == "full" else (3, 1)
    datadir = root / which
    datadir.mkdir(exist_ok=True)
    command = [binary, "--regtest", f"--datadir={datadir}",
               f"--rpcport={ports[index]}", f"--port={ports[index+1]}",
               f"--wallet-socket-port={ports[index+2]}", "--listen=1", "--utreexo=1",
               f"--connect=127.0.0.1:{ports[peer]}", "--consensus-sixty-second-height=4",
               "--utreexo-bridge=1" if which == "full" else "--utreexo-stateless=1", *extra]
    with (root / f"{which}.log").open("a") as log:
        processes[which] = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    wait(lambda: rpc(which, "getblockcount") >= 0)


def stop(which):
    process = processes.pop(which)
    process.terminate()
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise RuntimeError(f"{which} failed to stop cleanly")
    assert process.returncode == 0, (which, process.returncode)


def check(which, height, spacing):
    assert rpc(which, "getblockcount") == height
    info = rpc(which, "getconsensusinfo")
    assert info["sixty_second_activation_height"] == 4, info
    assert info["target_spacing_height"] == height + 1, info
    assert info["target_spacing_seconds"] == spacing, info
    assert info["tail_emission_una"] == (100_000_000 if spacing == 120 else 50_000_000), info
    economics = rpc(which, "economics.getinfo")
    assert economics["block_time_seconds"] == spacing, economics
    assert economics["halving_interval"] == 1_314_000, economics
    assert float(economics["next_block_reward_din"]) == 100, economics
    assert float(economics["current_supply_din"]) == (height + 1) * 100, economics
    return info["consensus_checksum"]


def roots(height):
    full = rpc("full", "blockchain.getutreexocommitment")
    csn = rpc("csn", "blockchain.getutreexocommitment")
    assert full["commitment"] == csn["commitment"], (full, csn)
    assert csn["verified_height"] == height, csn
    return csn["commitment"]


try:
    # Invalid values and attempts to override production must fail before startup.
    for network, value, error in ([("--regtest", v, "Invalid sixty-second activation height")
                                  for v in ("0", "-1", "4294967296", "1x", "")]
        + [("--mainnet", "4", "REGTEST-only"), ("--testnet", "4", "REGTEST-only")]):
        result = subprocess.run([binary, network, f"--datadir={root / 'rejected'}",
                                 f"--consensus-sixty-second-height={value}"],
                                capture_output=True, text=True, timeout=20)
        assert result.returncode != 0 and error in result.stderr, (network, value, result)
    start("full")
    start("csn")
    initial_checksum = check("full", 0, 120)
    address = rpc("full", "wallet.getnewaddress", ["taproot", "sixty-second"])
    if isinstance(address, dict):
        address = address["address"]
    rpc("full", "generatetoaddress", [2, address])
    parent = rpc("full", "getbestblockhash")
    wait(lambda: rpc("csn", "getbestblockhash") == parent)
    parent_root = roots(2)
    for which in ("full", "csn"):
        check(which, 2, 120)
        stop(which)
        start(which)
        check(which, 2, 120)
    rpc("full", "generatetoaddress", [4, address])
    tip = rpc("full", "getbestblockhash")
    wait(lambda: rpc("csn", "getbestblockhash") == tip)
    final_root = roots(6)
    for which in ("full", "csn"):
        assert check(which, 6, 60) == initial_checksum
    invalidated = rpc("full", "getblockhash", [3])
    # Roll back each independently; invalidity markers prevent peer re-admission.
    for which in ("csn", "full"):
        rpc(which, "blockchain.invalidateblock", [invalidated])
        assert rpc(which, "getbestblockhash") == parent
        check(which, 2, 120)
    assert roots(2) == parent_root
    for which in ("full", "csn"):
        stop(which)
        start(which)
        check(which, 2, 120)
        rpc(which, "blockchain.reconsiderblock", [invalidated])
        wait(lambda: rpc(which, "getbestblockhash") == tip)
        check(which, 6, 60)
    assert roots(6) == final_root
    stop("full")
    start("full", ["--reindex"])
    wait(lambda: rpc("full", "getbestblockhash") == tip)
    check("full", 6, 60)
    assert roots(6) == final_root
    print("PASS: activation, unchanged 100 DIN reward, CSN/full roots, rollback, restart and reindex", flush=True)
    success = True
finally:
    cleanup_errors = []
    for which in list(processes):
        try:
            stop(which)
        except Exception as error:
            cleanup_errors.append(str(error))
    if cleanup_errors:
        success = False
    if success:
        shutil.rmtree(root)
    else:
        print("Retained evidence:", root, flush=True)

    if cleanup_errors:
        raise RuntimeError("; ".join(cleanup_errors))

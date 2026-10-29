#!/usr/bin/env python3
"""Benign localhost handshakes against the actual ON/OFF daemon service."""
import base64
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

binary = str(Path(sys.argv[1]).resolve())
assert sys.argv[2] in ("0", "1")
expect_orchard = sys.argv[2] == "1"
root = Path(tempfile.mkdtemp(prefix="dinero-orchard-capability-"))
datadir = root / "node"
datadir.mkdir()
magic = 0xFABFB5DA
compact = 1 << 29
orchard = 1 << 30
process = None
output = None
connections = []
reservations = []
success = False
handshake_pool = None


def digest(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def send_message(connection, command, payload=b""):
    name = command.encode("ascii")
    assert len(name) <= 12
    connection.sendall(struct.pack("<I12sI4s", magic, name, len(payload), digest(payload)[:4]) + payload)


def receive_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        assert part, "Peer closed before completing the benign handshake"
        data.extend(part)
    return bytes(data)


def receive_message(connection):
    header = receive_exact(connection, 24)
    received_magic, command, size, checksum = struct.unpack("<I12sI4s", header)
    assert received_magic == magic and size <= 256 * 1024
    payload = receive_exact(connection, size)
    assert digest(payload)[:4] == checksum
    return command.rstrip(b"\x00").decode("ascii"), payload


def fixture_version(nonce):
    address = struct.pack("<Q", 1) + b"\x00" * 10 + b"\xff\xff\x7f\x00\x00\x01" + struct.pack(">H", 1)
    agent = b"/local-capability-fixture/"
    return (struct.pack("<iQq", 70016, 1, int(time.time())) + address + address +
            struct.pack("<Q", nonce) + bytes([len(agent)]) + agent + struct.pack("<iB", 0, 0))


def check_version(message):
    command, payload = message
    assert command == "version" and len(payload) >= 80
    flags = struct.unpack_from("<Q", payload, 4)[0]
    assert bool(flags & compact), "Daemon omitted its existing compact/timing capability"
    assert bool(flags & orchard) == expect_orchard, "Actual service advertised the wrong Orchard capability"
    return flags


def rpc(method):
    cookie = next(p.read_text().strip() for p in [datadir / ".cookie", datadir / "regtest" / ".cookie"] if p.exists())
    request = urllib.request.Request(f"http://127.0.0.1:{rpc_port}/",
        json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": []}).encode(),
        {"Content-Type": "application/json", "Authorization": "Basic " + base64.b64encode(cookie.encode()).decode()})
    with urllib.request.urlopen(request, timeout=15) as response:
        value = json.load(response)
    assert not value.get("error"), "Isolated daemon RPC failed"
    return value["result"]


try:
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(4)
    listener.settimeout(15)
    connections.append(listener)
    fixture_port = listener.getsockname()[1]
    for _ in range(3):
        reserved = socket.socket()
        reserved.bind(("127.0.0.1", 0))
        reservations.append(reserved)
    rpc_port, p2p_port, wallet_port = [s.getsockname()[1] for s in reservations]
    config = datadir / "dinero.conf"
    config.write_text(f"p2p.connect=127.0.0.1:{fixture_port}\np2p.stun.enabled=false\n"
                      "p2p.upnp=false\np2p.natpmp=false\np2p.portmap=off\n"
                      "p2p.tor_mode=disabled\np2p.listen_onion=false\np2p.relay=off\n")
    command = [binary, "--regtest", f"--datadir={datadir}", f"--conf={config}",
               f"--rpcport={rpc_port}", f"--port={p2p_port}",
               f"--wallet-socket-port={wallet_port}", "--listen=1", "--utreexo=1"]
    for reserved in reservations:
        reserved.close()
    reservations.clear()
    output = (root / "daemon.log").open("w")
    process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)

    def complete_outbound():
        outbound, address = listener.accept()
        connections.append(outbound)
        assert address[0] == "127.0.0.1"
        outbound.settimeout(15)
        flags = check_version(receive_message(outbound))
        send_message(outbound, "version", fixture_version(0x3141592653589793))
        assert receive_message(outbound) == ("verack", b"")
        send_message(outbound, "verack")
        return flags

    # Reply during daemon startup so its ordinary handshake deadline does not
    # depend on when the RPC listener becomes ready. Exceptions reach main.
    listener.settimeout(90)
    handshake_pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
    outbound_result = handshake_pool.submit(complete_outbound)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        assert process.poll() is None, "Daemon exited before RPC readiness"
        try:
            if rpc("getblockcount") == 0:
                break
        except (OSError, StopIteration, ValueError):
            pass
        time.sleep(0.1)
    else:
        raise AssertionError("Isolated daemon failed to reach authenticated RPC readiness")

    outbound_flags = outbound_result.result(timeout=15)
    print("PASS actual daemon outbound version advertises exact backend capability", flush=True)

    inbound = socket.create_connection(("127.0.0.1", p2p_port), timeout=15)
    connections.append(inbound)
    inbound.settimeout(15)
    send_message(inbound, "version", fixture_version(0x2718281828459045))
    inbound_flags = check_version(receive_message(inbound))
    send_message(inbound, "verack")
    assert receive_message(inbound) == ("verack", b"")
    assert inbound_flags == outbound_flags
    print("PASS actual daemon inbound version matches outbound capability", flush=True)
    assert rpc("getblockcount") == 0
    print("PASS dormant regtest handshake accepts ordinary peer without activation", flush=True)
    for connection in connections:
        connection.close()
    connections.clear()
    rpc("stop")
    assert process.wait(timeout=90) == 0, "Daemon failed clean shutdown"
    process = None
    output.close()
    output = None
    log = (root / "daemon.log").read_text(errors="replace")
    assert "Connect-only mode: skipping peers.dat bootstrap" in log
    assert "Reconnect targets armed: 1" in log
    assert "STUN discovery dispatched" not in log and "port mapping discovery dispatched" not in log
    print("PASS isolated capability daemon shuts down cleanly", flush=True)
    success = True
finally:
    for connection in connections + reservations:
        connection.close()
    if process is not None:
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
    if handshake_pool is not None:
        handshake_pool.shutdown(wait=True, cancel_futures=True)
    if output is not None:
        output.close()
    if os.environ.get("DINERO_KEEP_ORCHARD_CAPABILITY_EVIDENCE") == "1" or not success:
        # Export logs only; cookie/wallet databases remain inside the local fixture.
        print("Capability evidence directory:", root, flush=True)
    else:
        shutil.rmtree(root)

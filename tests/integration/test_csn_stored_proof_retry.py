#!/usr/bin/env python3
"""A stored CSN body must not suppress refetching its missing validation proof.

Relay real bridge-produced frames unchanged, but deliver descendants before the
fork-point bodies. Stop the CSN after those descendants are durably stored, then
restart against the bridge directly. No proxy, packet withholding, reconsider,
or offline database edits remain during the convergence assertion.

This uses ordinary regtest like RestartChurnBoringnessGate; it qualifies proof
receipt/restart scheduling, not PoW or an activation profile.
"""
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import threading

import test_csn_replay_metadata_recovery as fixture

WORK = fixture.WORK.with_name("dinero-csn-stored-proof-retry-" + fixture.WORK.name.rsplit("-", 1)[1])
fixture.WORK.rename(WORK)
fixture.WORK = WORK
require, wait = fixture.require, fixture.wait


class Node(fixture.Node):
    def start_at(self, peer_port):
        args = [str(fixture.BINARY), "--regtest", f"--datadir={self.path}",
                f"--rpcport={self.rpc}", f"--port={self.p2p}",
                f"--wallet-socket-port={self.wallet}", "--listen=1", "--utreexo=1",
                "--utreexo-stateless=1" if self.csn else "--utreexo-bridge=1",
                f"--connect=127.0.0.1:{peer_port}"]
        with (WORK / f"{self.name}.log").open("ab") as log:
            self.process = subprocess.Popen(args, stdout=log, stderr=log)

        def ready():
            require(self.process.poll() is None, f"{self.name} exited during startup")
            try:
                return self.call("getblockcount") >= 0
            except (FileNotFoundError, ConnectionError, fixture.urllib.error.URLError):
                return False
        wait(ready, f"{self.name} RPC ready", 30)


class ProofOrderProxy:
    """One connection; bounded capture of complete, unmodified P2P frames."""

    def __init__(self, target_port):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.target_port = target_port
        self.client = self.bridge = None
        self.stopping = threading.Event()
        self.armed = threading.Event()
        self.lock = threading.Lock()
        self.send_lock = threading.Lock()
        self.messages = {}
        self.errors = []
        self.threads = [threading.Thread(target=self.accept, daemon=True)]
        self.threads[0].start()

    @staticmethod
    def exact(sock, size):
        data = bytearray()
        while len(data) < size:
            chunk = sock.recv(size - len(data))
            if not chunk:
                raise EOFError("peer closed the proxy connection")
            data.extend(chunk)
        return bytes(data)

    def accept(self):
        try:
            self.client, _ = self.listener.accept()
            self.bridge = socket.create_connection(("127.0.0.1", self.target_port), timeout=10)
            self.bridge.settimeout(None)
            upstream = threading.Thread(target=self.forward,
                                        args=(self.client, self.bridge, False), daemon=True)
            self.threads.append(upstream)
            upstream.start()
            self.forward(self.bridge, self.client, True)
        except Exception as error:
            if not self.stopping.is_set():
                self.errors.append(repr(error))

    def forward(self, source, destination, inbound):
        try:
            while not self.stopping.is_set():
                header = self.exact(source, 24)
                size = struct.unpack_from("<I", header, 16)[0]
                require(size <= 10 * 1024 * 1024, "oversize fixture frame")
                payload = self.exact(source, size)
                frame = header + payload
                if inbound and header[4:16].rstrip(b"\0") == b"utxoblk" and self.armed.is_set():
                    require(len(payload) >= 37, "truncated fixture utxoblk envelope")
                    block_hash = payload[1:33][::-1].hex()
                    height = struct.unpack_from("<I", payload, 33)[0]
                    with self.lock:
                        self.messages[block_hash] = (height, frame)
                        require(len(self.messages) <= 64 and
                                sum(len(value[1]) for value in self.messages.values()) <= 32 * 1024 * 1024,
                                "fixture capture limit exceeded")
                    continue
                if inbound:
                    with self.send_lock:
                        destination.sendall(frame)
                else:
                    destination.sendall(frame)
        except Exception as error:
            if not self.stopping.is_set():
                self.errors.append(repr(error))

    def has_all(self, hashes):
        require(not self.errors, f"proxy failed: {self.errors}")
        with self.lock:
            return all(block_hash in self.messages for block_hash in hashes)

    def deliver(self, block_hash, expected_height):
        require(not self.errors, f"proxy failed: {self.errors}")
        with self.lock:
            height, frame = self.messages[block_hash]
        require(height == expected_height, "captured frame belongs to a different height")
        with self.send_lock:
            self.client.sendall(frame)

    def close(self):
        self.stopping.set()
        for sock in (self.client, self.bridge, self.listener):
            if sock:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                sock.close()
        for thread in self.threads:
            thread.join(timeout=5)
            require(not thread.is_alive(), "proxy thread did not stop")


def main(receipt):
    bridge = Node("bridge")
    bridge.start_at(9)  # Explicit localhost-only peer selection, no seed discovery.
    address = bridge.call("wallet.createhd", ["stored-proof-retry"])["first_address"]
    bridge.call("generatetoaddress", [112, address])
    initial_tip = bridge.call("getbestblockhash")
    coinbase = bridge.call("getblock", [bridge.call("getblockhash", [1]), 1])["tx"][0]
    proxy = ProofOrderProxy(bridge.p2p)
    csn = Node("csn", csn=True)
    try:
        csn.start_at(proxy.port)
        wait(lambda: csn.call("getbestblockhash") == initial_tip, "initial CSN convergence", 60)
        initial_root = fixture.root(csn)
        proxy.armed.set()
        bridge.call("invalidateblock", [bridge.call("getblockhash", [110])])
        require(bridge.call("getblockcount") == 109, "fork did not start at 109")
        bridge.call("generatetoaddress", [10, address])
        hashes = {height: bridge.call("getblockhash", [height]) for height in range(110, 120)}
        wait(lambda: proxy.has_all(hashes.values()), "capture actual competing branch proofs", 60)
        for height in range(113, 120):
            proxy.deliver(hashes[height], height)

        def stored_without_prefix():
            require(not proxy.errors, f"proxy failed: {proxy.errors}")
            log = (WORK / "csn.log").read_text(errors="replace")
            # The guarded worker now rejects a stale parent before attempting
            # to apply its proof. Both paths leave the same persisted bodies
            # without a usable proof receipt, which is the restart condition.
            deferred = ("Proof failed at height 113 during a reorg-lag window" in log or
                        "Discarding stale forward proof at height 113; active parent changed" in log)
            return (deferred and
                    all(f"persisted body position at height {height} {hashes[height][:16]}" in log
                        for height in range(113, 120)))

        wait(stored_without_prefix, "descendant bodies persisted before fork-point proofs", 15)
        require(csn.call("getbestblockhash") == initial_tip,
                "out-of-order unvalidated bodies must not replace the active chain")
        require(fixture.root(csn) == initial_root,
                "out-of-order unvalidated bodies must not change the canonical forest")
        receipt["stored_descendants"] = {str(h): hashes[h] for h in range(113, 120)}
        receipt["checks"].append("unvalidated stored descendants leave the active tip unchanged")
    finally:
        # End the interrupted connection before stopping the node. Buffered
        # packets belong only to this old connection and are never replayed.
        proxy.close()
        csn.stop()

    csn.start_at(bridge.p2p)
    wait(lambda: csn.call("getbestblockhash") == hashes[119],
         "restart must fetch proofs for stored descendants and adopt the heavier branch", 60)
    require(csn.call("getblockcount") == 119, "wrong converged height")
    require(fixture.root(csn) == fixture.root(bridge), "post-reorg Utreexo roots differ")
    fixture.proof(bridge, csn, coinbase)
    receipt["checks"].append("unrestricted restart converges with equal roots and a valid Utreexo proof")

    csn.stop()
    csn.start_at(bridge.p2p)
    require(csn.call("getbestblockhash") == hashes[119], "second restart changed the adopted tip")
    require(fixture.root(csn) == fixture.root(bridge), "second restart changed the Utreexo root")
    fixture.proof(bridge, csn, coinbase)
    receipt["checks"].append("adopted branch, root and proof survive a second clean restart")
    print("PASS CSN refetches missing proofs for stored bodies across restart and reorg", flush=True)


if __name__ == "__main__":
    receipt = {"binary": str(fixture.BINARY), "binary_sha256": fixture.sha256(fixture.BINARY),
               "test_sha256": fixture.sha256(Path(__file__)),
               "helper_sha256": fixture.sha256(Path(fixture.__file__)),
               "checks": [], "success": False}
    success = False
    print(f"Evidence: {WORK}", flush=True)
    try:
        main(receipt)
        success = True
    finally:
        errors = []
        for node in fixture.NODES:
            try:
                node.stop()
            except Exception as error:
                errors.append(str(error))
        receipt.update(success=success and not errors, cleanup_errors=errors)
        (WORK / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if receipt["success"] and os.environ.get("DINERO_TEST_KEEP_DATA") != "1":
            shutil.rmtree(WORK)
        else:
            print(f"Retained evidence: {WORK}", flush=True)
        if errors:
            raise AssertionError(errors)

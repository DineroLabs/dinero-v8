#!/usr/bin/env python3
"""A bad witness body cannot poison an honest header or advance CSN state.

A CSN peer first syncs to an actual bridge through a localhost proxy. The
proxy changes one byte of the first spend witness in the bridge's next block,
then passes subsequent frames unchanged. The altered body has the same header
and transaction IDs, but its witness commitment is wrong. The CSN must fetch
the real body and reach the next tip with the bridge's accumulator root.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import threading
import time

import test_csn_stored_proof_retry as base

WORK = base.WORK.with_name("dinero-csn-witness-replacement-" + base.WORK.name.rsplit("-", 1)[1])
base.WORK.rename(WORK)
base.WORK = WORK
base.fixture.WORK = WORK
require, wait = base.require, base.wait


def compact_size(data, pos):
    first = data[pos]
    pos += 1
    if first < 253:
        return first, pos
    count = {253: 2, 254: 4, 255: 8}[first]
    return int.from_bytes(data[pos:pos + count], "little"), pos + count


def scan_transaction(data, pos):
    pos += 4  # version
    segwit = data[pos:pos + 2] == b"\0\1"
    if segwit:
        pos += 2
    inputs, pos = compact_size(data, pos)
    for _ in range(inputs):
        pos += 36
        size, pos = compact_size(data, pos)
        pos += size + 4
    outputs, pos = compact_size(data, pos)
    for _ in range(outputs):
        pos += 8
        size, pos = compact_size(data, pos)
        pos += size
    witness_byte = None
    if segwit:
        for _ in range(inputs):
            items, pos = compact_size(data, pos)
            for _ in range(items):
                size, pos = compact_size(data, pos)
                if size and witness_byte is None:
                    witness_byte = pos
                pos += size
    return pos + 4, witness_byte  # locktime


def corrupt_spend_witness(body):
    position = 128  # Dinero's header
    count, position = compact_size(body, position)
    require(count >= 2, "fixture block must contain a spend transaction")
    position, _ = scan_transaction(body, position)  # coinbase
    _, witness_byte = scan_transaction(body, position)  # wallet spend
    require(witness_byte is not None, "fixture spend has no witness")
    altered = bytearray(body)
    altered[witness_byte] ^= 1
    require(altered[:128] == body[:128], "tamper changed the header")
    return bytes(altered), witness_byte


class CorruptOnceProxy(base.ProofOrderProxy):
    def __init__(self, target_port):
        self.armed = threading.Event()
        self.mutated = threading.Event()
        self.detail = None
        self.parent_hash = None
        self.commands = {}
        super().__init__(target_port)

    def forward(self, source, destination, inbound):
        try:
            while not self.stopping.is_set():
                header = self.exact(source, 24)
                size = struct.unpack_from("<I", header, 16)[0]
                require(size <= 10 * 1024 * 1024, "oversize fixture frame")
                payload = self.exact(source, size)
                command = header[4:16].rstrip(b"\0")
                if inbound:
                    self.commands[command.decode(errors="replace")] = \
                        self.commands.get(command.decode(errors="replace"), 0) + 1
                if inbound and self.armed.is_set() and not self.mutated.is_set() and \
                        command in (b"utxoblk", b"block"):
                    offset = 73 if command == b"utxoblk" else 0
                    if command == b"utxoblk":
                        require(len(payload) >= offset, "short utxoblk envelope")
                        height = struct.unpack_from("<I", payload, 33)[0]
                        if height != 111:
                            destination.sendall(header + payload)
                            continue
                        body_size = struct.unpack_from("<I", payload, 69)[0]
                        body = payload[offset:offset + body_size]
                        require(len(body) == body_size, "short utxoblk body")
                    else:
                        body = payload
                        if self.parent_hash is None or body[4:36] != \
                                bytes.fromhex(self.parent_hash)[::-1]:
                            destination.sendall(header + payload)
                            continue
                    altered, witness_byte = corrupt_spend_witness(body)
                    payload = payload[:offset] + altered + payload[offset + len(body):]
                    checksum = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
                    header = header[:20] + checksum
                    self.detail = {"command": command.decode(), "witness_offset": witness_byte,
                                   "header_hash": hashlib.sha256(
                                       hashlib.sha256(body[:128]).digest()).hexdigest()}
                    self.mutated.set()
                destination.sendall(header + payload)
        except Exception as error:
            if not self.stopping.is_set():
                self.errors.append(repr(error))


def main(receipt):
    bridge = base.Node("bridge")
    bridge.start_at(9)  # localhost only; no seed discovery
    address = bridge.call("wallet.createhd", ["witness-body-retry"])["first_address"]
    bridge.call("generatetoaddress", [110, address])
    require(bridge.call("getblockcount") == 110, "bridge bootstrap height")

    csn_proxy = CorruptOnceProxy(bridge.p2p)
    csn = base.Node("csn", csn=True)
    proxy_closed = False
    try:
        csn.start_at(csn_proxy.port)
        wait(lambda: csn.call("getblockcount") == 110,
             "CSN bootstrap to height 110", 120)
        initial_root = base.fixture.root(bridge)
        require(base.fixture.root(csn) == initial_root, "bootstrap roots differ")
        receipt["bootstrap"] = {"height": 110, "root": initial_root}
        parent_hash = bridge.call("getbestblockhash")
        csn_proxy.parent_hash = parent_hash

        recipient = bridge.call("wallet.getnewaddress")["address"]
        sent = bridge.call("wallet.sendtoaddress", [recipient, 1.0])
        require(sent["accepted"] and sent["status"] == "signed_and_submitted",
                "fixture spend was not accepted")
        csn_proxy.armed.set()
        bridge.call("generatetoaddress", [1, address])
        target_hash = bridge.call("getblockhash", [111])
        require(sent["txid"] in bridge.call("getblock", [target_hash, 1])["tx"],
                "target block did not include signed spend")
        wait(lambda: csn_proxy.mutated.is_set(),
             "bad witness frame delivered", 30)
        bridge.call("generatetoaddress", [1, address])
        tip = bridge.call("getbestblockhash")
        receipt["tampering"] = {"csn": csn_proxy.detail,
                                "target_hash": target_hash}

        wait(lambda: csn.call("getbestblockhash") == tip,
             "correct body replaces bad witness body on CSN", 120)
        require(csn.call("getblockcount") == 112,
                "wrong recovered height")
        final_root = base.fixture.root(bridge)
        require(base.fixture.root(csn) == final_root,
                "proof forest disagrees after replacement")
        require(not csn_proxy.errors, f"proxy errors: {csn_proxy.errors}")
        receipt["recovered"] = {"height": 112, "tip": tip, "root": final_root}
        csn_proxy.stopping.set()  # the intentional restart closes its socket
        csn.stop()
        csn_proxy.close()
        proxy_closed = True
        csn.start_at(bridge.p2p)
        wait(lambda: csn.call("getbestblockhash") == tip,
             "CSN retains honest tip after restart", 60)
        require(base.fixture.root(csn) == final_root,
                "CSN retains recovered forest root after restart")
        receipt["restart"] = {"height": csn.call("getblockcount"),
                              "tip": csn.call("getbestblockhash"),
                              "root": base.fixture.root(csn)}
        print("PASS bad witness body replaced; CSN reaches H112 and survives restart", flush=True)
    finally:
        receipt["proxies"] = {
            "csn": {"mutated": csn_proxy.mutated.is_set(), "detail": csn_proxy.detail,
                    "errors": csn_proxy.errors, "commands": csn_proxy.commands}}
        if not proxy_closed:
            csn_proxy.close()


if __name__ == "__main__":
    receipt = {"binary": str(base.fixture.BINARY),
               "binary_sha256": base.fixture.sha256(base.fixture.BINARY),
               "success": False}
    print(f"Evidence: {WORK}", flush=True)
    try:
        main(receipt)
        receipt["success"] = True
    finally:
        errors = []
        for node in base.fixture.NODES:
            try:
                node.stop()
            except Exception as error:
                errors.append(str(error))
        receipt["cleanup_errors"] = errors
        (WORK / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if receipt["success"] and not errors and os.environ.get("DINERO_TEST_KEEP_DATA") != "1":
            shutil.rmtree(WORK)
        else:
            print(f"Retained evidence: {WORK}", flush=True)
        if errors:
            raise AssertionError(errors)

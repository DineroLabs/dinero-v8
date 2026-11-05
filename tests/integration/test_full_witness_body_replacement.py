#!/usr/bin/env python3
"""A full node replaces a same-header body whose spend witness was changed.

The localhost proxy converts the H111 compact-block announcement to a normal
inventory announcement so the node asks for a full block body. It flips one
spend-witness byte in exactly that response; the header and txids stay intact.
The bridge then supplies the correct copy on retry. No invalidate/reconsider
RPC is used, so reaching H112 proves the honest hash was not marked failed.
"""
import hashlib
import json
import os
import shutil
import struct
import threading

import test_csn_witness_body_replacement as witness

WORK = witness.WORK.with_name(
    "dinero-full-witness-replacement-" + witness.WORK.name.rsplit("-", 1)[1])
witness.WORK.rename(WORK)
witness.WORK = WORK
witness.base.WORK = WORK
witness.base.fixture.WORK = WORK
require, wait = witness.require, witness.wait


def frame(magic, command, payload):
    checksum = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    return magic + command.ljust(12, b"\0") + struct.pack("<I", len(payload)) + checksum + payload


class FullBodyProxy(witness.CorruptOnceProxy):
    def __init__(self, target_port):
        self.compact_to_inv = threading.Event()
        self.outbound_commands = {}
        super().__init__(target_port)

    def forward(self, source, destination, inbound):
        try:
            while not self.stopping.is_set():
                header = self.exact(source, 24)
                size = struct.unpack_from("<I", header, 16)[0]
                require(size <= 10 * 1024 * 1024, "oversize fixture frame")
                payload = self.exact(source, size)
                command = header[4:16].rstrip(b"\0")
                counters = self.commands if inbound else self.outbound_commands
                name = command.decode(errors="replace")
                counters[name] = counters.get(name, 0) + 1
                if inbound and self.armed.is_set() and command == b"cmpctblock" and \
                        len(payload) >= 128 and self.parent_hash and \
                        payload[4:36] == bytes.fromhex(self.parent_hash)[::-1]:
                    # Replace the compact announcement with MSG_BLOCK inventory;
                    # this exercises the ordinary scheduler's full-body path.
                    block_hash_wire = hashlib.sha256(
                        hashlib.sha256(payload[:128]).digest()).digest()[::-1]
                    inv = b"\x01" + struct.pack("<I", 2) + block_hash_wire
                    destination.sendall(frame(header[:4], b"inv", inv))
                    self.compact_to_inv.set()
                    continue
                if inbound and self.armed.is_set() and not self.mutated.is_set() and \
                        command == b"block" and len(payload) >= 128 and \
                        self.parent_hash and payload[4:36] == bytes.fromhex(self.parent_hash)[::-1]:
                    altered, witness_byte = witness.corrupt_spend_witness(payload)
                    digest = hashlib.sha256(hashlib.sha256(payload[:128]).digest()).hexdigest()
                    self.detail = {"command": "block", "witness_offset": witness_byte,
                                   "header_hash": digest}
                    self.mutated.set()
                    payload = altered
                    header = frame(header[:4], b"block", payload)[:24]
                destination.sendall(header + payload)
        except Exception as error:
            if not self.stopping.is_set():
                self.errors.append(repr(error))


def main(receipt):
    bridge = witness.base.Node("bridge")
    bridge.start_at(9)
    address = bridge.call("wallet.createhd", ["full-witness-retry"])["first_address"]
    bridge.call("generatetoaddress", [110, address])
    require(bridge.call("getblockcount") == 110, "bridge bootstrap height")
    proxy = FullBodyProxy(bridge.p2p)
    full = witness.base.Node("full")
    proxy_closed = False
    try:
        full.start_at(proxy.port)
        wait(lambda: full.call("getblockcount") == 110,
             "full node bootstrap to H110", 120)
        receipt["bootstrap"] = {"height": 110,
                                "root": witness.base.fixture.root(full)}
        require(receipt["bootstrap"]["root"] == witness.base.fixture.root(bridge),
                "full and bridge bootstrap roots match")
        proxy.parent_hash = bridge.call("getbestblockhash")

        recipient = bridge.call("wallet.getnewaddress")["address"]
        sent = bridge.call("wallet.sendtoaddress", [recipient, 1.0])
        require(sent["accepted"] and sent["status"] == "signed_and_submitted",
                "fixture spend was accepted")
        proxy.armed.set()
        bridge.call("generatetoaddress", [1, address])
        target_hash = bridge.call("getblockhash", [111])
        require(sent["txid"] in bridge.call("getblock", [target_hash, 1])["tx"],
                "H111 includes the signed spend")
        wait(lambda: proxy.compact_to_inv.is_set(),
             "H111 compact announcement converted to full-body inventory", 30)
        wait(lambda: proxy.mutated.is_set(), "bad H111 full body delivered", 30)
        require(proxy.detail["header_hash"] == target_hash,
                "tampered body retains the honest H111 header")
        bridge.call("generatetoaddress", [1, address])
        tip = bridge.call("getbestblockhash")
        receipt["tampering"] = {"compact_to_inv": True, "body": proxy.detail,
                                "target_hash": target_hash}

        wait(lambda: full.call("getbestblockhash") == tip,
             "correct same-hash body replaces the bad copy", 120)
        require(full.call("getblockcount") == 112, "full node reaches H112")
        final_root = witness.base.fixture.root(bridge)
        require(witness.base.fixture.root(full) == final_root,
                "full node and bridge H112 roots agree")
        require(not proxy.errors, f"proxy errors: {proxy.errors}")
        receipt["recovered"] = {"height": 112, "tip": tip, "root": final_root}
        proxy.stopping.set()
        full.stop()
        proxy.close()
        proxy_closed = True
        full.start_at(bridge.p2p)
        wait(lambda: full.call("getbestblockhash") == tip,
             "full node retains honest tip after restart", 60)
        require(witness.base.fixture.root(full) == final_root,
                "full node retains honest root after restart")
        receipt["restart"] = {"height": full.call("getblockcount"),
                              "tip": full.call("getbestblockhash"),
                              "root": witness.base.fixture.root(full)}
        print("PASS full node replaced bad witness body and retained H112 after restart", flush=True)
    finally:
        receipt["proxy"] = {"mutated": proxy.mutated.is_set(),
                            "compact_to_inv": proxy.compact_to_inv.is_set(),
                            "detail": proxy.detail, "errors": proxy.errors,
                            "inbound_commands": proxy.commands,
                            "outbound_commands": proxy.outbound_commands}
        if not proxy_closed:
            proxy.close()


if __name__ == "__main__":
    receipt = {"binary": str(witness.base.fixture.BINARY),
               "binary_sha256": witness.base.fixture.sha256(witness.base.fixture.BINARY),
               "success": False}
    print(f"Evidence: {WORK}", flush=True)
    try:
        main(receipt)
        receipt["success"] = True
    finally:
        errors = []
        for node in witness.base.fixture.NODES:
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

#!/usr/bin/env python3
"""Regtest mempool RPC, restart, and reorg transcript for the library move.

The reference must be recorded from the exact pre-extraction base and reviewed
before check mode is used in CI. A failed RPC is preserved as a response; a
failed setup action or missing persistence file is a run error, never a pass.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("wallet_rpc_transcript",
                                              ROOT / "tests/separation/rpc_transcript.py")
rpc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rpc)


def varint(data, pos):
    if pos >= len(data):
        raise rpc.RunError("mempool.dat truncated varint")
    first = data[pos]
    pos += 1
    if first < 0xfd:
        return first, pos
    size = {0xfd: 2, 0xfe: 4, 0xff: 8}[first]
    if pos + size > len(data):
        raise rpc.RunError("mempool.dat truncated varint payload")
    return int.from_bytes(data[pos:pos + size], "little"), pos + size


def persisted(path):
    data = path.read_bytes()
    if len(data) < 13 or data[:8] != b"MEMPOOLV":
        raise rpc.RunError("mempool.dat missing or has wrong magic")
    version = struct.unpack_from("<I", data, 8)[0]
    count, pos = varint(data, 12)
    entries = []
    for _ in range(count):
        length, pos = varint(data, pos)
        if length > len(data) - pos - 20:
            raise rpc.RunError("mempool.dat truncated entry")
        body = data[pos:pos + length].hex()
        pos += length
        _arrival, fee, height = struct.unpack_from("<QQI", data, pos)
        pos += 20
        entries.append({"transaction_hex": body, "fee": fee, "height": height,
                        "arrival_time": "<T:int>"})
    if pos != len(data):
        raise rpc.RunError("mempool.dat trailing bytes")
    return {"magic": "MEMPOOLV", "version": version, "entries": sorted(entries,
            key=lambda row: row["transaction_hex"])}


def signed_spend(node, prevout, signing_coin, recipient, amount):
    replaceable = {**prevout, "sequence": 0xfffffffd}
    unsigned = node.ok("wallet.createrawtransaction", [[replaceable],
                       [{recipient: amount}]])["hex"]
    return node.ok("wallet.signrawtransaction", [unsigned, [signing_coin]])["hex"]


def expect_allowed(response, expected, step):
    entries = rpc.result_of(response)
    if (not isinstance(entries, list) or len(entries) != 1 or
            entries[0].get("allowed") is not expected):
        raise rpc.RunError(step + " gave an unexpected admission result: " + repr(response))


def expect_pool(response, expected, step):
    ids = rpc.result_of(response)
    if not isinstance(ids, list) or set(ids) != set(expected):
        raise rpc.RunError(step + " has unexpected mempool IDs: " + repr(response))


def expect_persisted_transaction(file_state, expected_hex):
    entries = file_state.get("entries", [])
    if len(entries) != 1 or entries[0].get("transaction_hex") != expected_hex.lower():
        raise rpc.RunError("mempool.dat did not retain the exact submitted transaction bytes")


def run(dinerod, config):
    work = tempfile.mkdtemp(prefix="sep_mp4_")
    logs = os.path.join(work, "logs")
    os.makedirs(logs)
    taken, pids, nodes = set(), [], []
    transcript = rpc.Transcript()
    try:
        node = rpc.Node(dinerod, work, "m", taken, logs, pids)
        nodes.append(node)
        (Path(node.datadir) / "dinero.conf").write_text("mempool.enable_rbf=1\n")
        node.start()
        if node.ok("rbf.config", []).get("enabled") is not True:
            raise rpc.RunError("regtest RBF configuration was not applied")
        node.ok("wallet.restore", {"name": "foreign", "mnemonic": rpc.FOREIGN_MNEMONIC,
                                   "rescan": False})
        foreign = node.ok("getnewaddress", [])["address"]
        node.ok("wallet.restore", {"name": "mp4", "mnemonic": rpc.MNEMONIC,
                                   "rescan": False})
        owner = node.ok("getnewaddress", [])["address"]
        node.ok("generatetoaddress", [1, owner])
        node.ok("generatetoaddress", [100, foreign])
        node.wait_synced()
        transcript.call(node, "empty.info", "getmempoolinfo", [])
        transcript.call(node, "empty.ids", "getrawmempool", [])
        coin = node.ok("listunspent", [0, 9999999])[0]
        recipient = node.ok("getnewaddress", [])["address"]
        prevout = {"txid": coin["txid"], "vout": coin["vout"]}
        signing_coin = {**prevout, "scriptPubKey": coin["scriptPubKey"],
                        "amount": coin["amount"]}
        signed = signed_spend(node, prevout, signing_coin, recipient, 99.9999)
        expect_allowed(transcript.call(node, "accept.valid", "testmempoolaccept", [signed]),
                       True, "accept.valid")
        sent = rpc.result_of(transcript.call(node, "sendrawtransaction", "sendrawtransaction",
                                                  [signed]))
        txid = (sent.get("txid") or sent.get("result")) if isinstance(sent, dict) else sent
        if not isinstance(txid, str) or not rpc.HEX64.fullmatch(txid):
            raise rpc.RunError("sendrawtransaction did not return a txid: " + repr(sent))
        expect_allowed(transcript.call(node, "accept.duplicate",
                                       "testmempoolaccept", [signed]), False, "accept.duplicate")
        transcript.call(node, "send.duplicate", "sendrawtransaction", [signed])
        transcript.call(node, "pending.info", "getmempoolinfo", [])
        transcript.call(node, "pending.ids", "getrawmempool", [])
        transcript.call(node, "pending.verbose", "getrawmempool", [True])
        transcript.call(node, "pending.entry", "getmempoolentry", [txid])
        transcript.call(node, "pending.ancestors", "getmempoolancestors", [txid])
        transcript.call(node, "pending.descendants", "getmempooldescendants", [txid])
        transcript.call(node, "pending.estimate", "estimatesmartfee", [6])
        transcript.call(node, "error.unknown_entry", "getmempoolentry", [rpc.UNKNOWN_TXID])
        transcript.call(node, "error.bad_raw", "testmempoolaccept", ["00"])
        low = signed_spend(node, prevout, signing_coin, recipient, 99.99991)
        expect_allowed(transcript.call(node, "rbf.reject_lower_fee",
                                       "testmempoolaccept", [low]), False,
                       "rbf.reject_lower_fee")
        high = signed_spend(node, prevout, signing_coin, recipient, 99.9998)
        expect_allowed(transcript.call(node, "rbf.accept_higher_fee",
                                       "testmempoolaccept", [high]), True,
                       "rbf.accept_higher_fee")
        replacement = rpc.result_of(transcript.call(node, "rbf.send_replacement",
                                                   "sendrawtransaction", [high]))
        replacement_txid = ((replacement.get("txid") or replacement.get("result"))
                            if isinstance(replacement, dict) else replacement)
        if not isinstance(replacement_txid, str) or not rpc.HEX64.fullmatch(replacement_txid):
            raise rpc.RunError("RBF replacement was not accepted: " +
                               repr(transcript.entries[-1]["body"]))
        transcript.call(node, "rbf.pool", "getrawmempool", [True])
        before_restart = node.ok("getrawmempool", [True])
        # On Windows the harness's SIGTERM helper terminates the process
        # immediately. Exercise the same graceful daemon shutdown everywhere
        # so mempool.dat is actually written before inspecting it.
        node.ok("stop", [])
        try:
            node.proc.wait(timeout=90)
        except subprocess.TimeoutExpired as error:
            raise rpc.RunError("daemon did not stop cleanly for the persistence check") from error
        node.stop()
        file_state = persisted(Path(node.datadir) / "mempool.dat")
        if len(file_state["entries"]) != 1:
            raise rpc.RunError("mempool.dat did not retain the pending transaction")
        # The normalized reference masks run-specific signatures and addresses.
        # Check actual disk bytes against the submitted body before that mask.
        expect_persisted_transaction(file_state, high)
        transcript.entries.append({"step": "persistence.file", "result": file_state})
        node.start()
        after_restart = node.ok("getrawmempool", [True])
        if after_restart != before_restart:
            # Verbose metadata may have non-authoritative arrival timing, but
            # the entry identity itself must survive restart.
            if set(after_restart) != set(before_restart):
                raise rpc.RunError("mempool transaction IDs changed after restart")
        transcript.call(node, "restart.verbose", "getrawmempool", [True])
        decoded = node.ok("wallet.decoderawtransaction", [high])
        child_prevout = {"txid": replacement_txid, "vout": 0}
        child_coin = {**child_prevout,
                      "scriptPubKey": decoded["vout"][0]["scriptPubKey"]["hex"],
                      "amount": 99.9998}
        child_recipient = node.ok("getnewaddress", [])["address"]
        child = signed_spend(node, child_prevout, child_coin,
                             child_recipient, 99.9995)
        expect_allowed(transcript.call(node, "cpfp.accept",
                                       "testmempoolaccept", [child]), True, "cpfp.accept")
        child_result = rpc.result_of(transcript.call(node, "cpfp.submit",
                                                    "sendrawtransaction", [child]))
        child_txid = ((child_result.get("txid") or child_result.get("result"))
                      if isinstance(child_result, dict) else child_result)
        if not isinstance(child_txid, str) or not rpc.HEX64.fullmatch(child_txid):
            raise rpc.RunError("CPFP child was not accepted: " +
                               repr(transcript.entries[-1]["body"]))
        transcript.call(node, "cpfp.ancestors", "getmempoolancestors", [child_txid])
        transcript.call(node, "cpfp.descendants", "getmempooldescendants",
                        [replacement_txid])
        transcript.call(node, "cpfp.pool", "getrawmempool", [True])
        node.ok("generatetoaddress", [1, foreign])
        block = node.ok("getbestblockhash", [])
        if not isinstance(block, str):
            raise rpc.RunError("generatetoaddress did not return a block hash")
        expect_pool(transcript.call(node, "confirmed.ids", "getrawmempool", []),
                    [], "confirmed.ids")
        node.ok("invalidateblock", [block])
        expect_pool(transcript.call(node, "reorg.ids", "getrawmempool", []),
                    [replacement_txid, child_txid], "reorg.ids")
        transcript.call(node, "reorg.verbose", "getrawmempool", [True])
        node.ok("reconsiderblock", [block])
        expect_pool(transcript.call(node, "reconsidered.ids", "getrawmempool", []),
                    [], "reconsidered.ids")
        for entry in transcript.entries:
            if (entry.get("method") == "getrawmempool" and
                    isinstance(entry.get("body", {}).get("result"), list)):
                # The plain RPC enumerates an unordered_map; membership, not
                # iteration order, is its contract.
                entry["body"]["result"].sort()
        normalized = rpc.Normalizer(work, taken, pids)
        # Wall time between submission and the RPC varies by runner. Preserve
        # the field and its JSON type while masking only its elapsed value.
        rpc.DURATION_KEYS.add("oldest_tx_age_seconds")
        entries = [normalized.value(e) for e in transcript.entries]
        for entry in entries:
            if (entry.get("method") == "getrawmempool" and
                    isinstance(entry.get("body", {}).get("result"), list)):
                entry["body"]["result"].sort()
        return {"config": config, "transcript": entries}
    except Exception:
        print("mempool_transcript: work dir kept at " + work, file=sys.stderr)
        raise
    finally:
        for node in nodes:
            node.stop()
        if sys.exc_info()[0] is None:
            shutil.rmtree(work, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("record", "check"))
    parser.add_argument("--dinerod", type=Path, required=True)
    parser.add_argument("--config", choices=("off", "on"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not os.access(args.dinerod, os.X_OK):
        print("mempool_transcript: dinerod unavailable", file=sys.stderr)
        return 2
    if args.mode == "check" and not args.out.is_file():
        print("mempool_transcript: missing reviewed reference", file=sys.stderr)
        return 2
    try:
        actual = run(str(args.dinerod.resolve()), args.config)
    except Exception as error:
        print("mempool_transcript: run error: " + str(error), file=sys.stderr)
        return 2
    text = json.dumps(actual, indent=1, sort_keys=True) + "\n"
    if args.mode == "record":
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text)
        print("mempool_transcript: recorded " + str(len(actual["transcript"])) + " steps")
        return 0
    expected = args.out.read_text()
    if text != expected:
        import difflib
        sys.stdout.writelines(difflib.unified_diff(expected.splitlines(True),
                              text.splitlines(True), "reference", "actual"))
        return 1
    print("mempool_transcript: PASS " + str(len(actual["transcript"])) + " steps")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Real DaemonApp/HTTP RPC shield, send, unshield, ordinary restart and reorg.

Uses only a process-local scheduled regtest profile. Never writes wallet/chain
SQLite, injects coins, calls recovery helpers, or fabricates selected progress.
Failure data stays in the isolated temporary directory for private collection.
"""
import base64
import hashlib
import json
from pathlib import Path
import socket
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request


def historical_template_block(template, genesis, anchor_bits, nonce_start, *, time_anchor=None):
    """Mine an empty historical regtest block with exact anchor-relative time.

    The daemon supplies the coinbase and its commitments. The ordinary submit
    path must independently accept the resulting time, difficulty and real PoW.
    This helper is restricted to the coupled fixture's preactivation prefix.
    """
    height = template["height"]
    assert type(height) is int and 1 <= height <= 101
    assert type(genesis["time"]) is int and 0 < genesis["time"] < 2**32
    # Regtest shares the canonical genesis header; its mining anchor differs.
    assert type(genesis["bits"]) is int and genesis["bits"] == 0x1d31ffce
    assert type(anchor_bits) is int and anchor_bits == 0x207fffff
    assert template["transactions"] == [], "historical fixture requires an empty template"
    assert type(nonce_start) is int and 0 <= nonce_start <= 2**32 - 1_000_000
    assert type(template["version"]) is int and 0 <= template["version"] < 2**32

    def hash_bytes(value):
        assert isinstance(value, str) and len(value) == 64
        assert all(c in "0123456789abcdef" for c in value)
        return bytes.fromhex(value)[::-1]

    # Historical consensus keeps anchor height zero but switches its TIME
    # source from genesis to the selected block-one header after height one.
    # The fixture must follow that existing rule, including the first gap.
    if height == 1:
        assert time_anchor is None or time_anchor == genesis
        time_anchor = genesis
    else:
        assert isinstance(time_anchor, dict)
        assert type(time_anchor["height"]) is int and time_anchor["height"] == 1
        assert type(time_anchor["time"]) is int and time_anchor["time"] == genesis["time"] + 120
        assert type(time_anchor["bits"]) is int and time_anchor["bits"] == anchor_bits
        assert time_anchor["previousblockhash"] == genesis["hash"]
        hash_bytes(time_anchor["hash"])

    coinbase = bytes.fromhex(template["coinbasetxn"]["data"])
    assert coinbase, "missing daemon-owned coinbase"
    # With one transaction, its txid is the merkle root. Preserve witness bytes.
    header = bytearray(template["version"].to_bytes(4, "little"))
    header += hash_bytes(template["previousblockhash"])
    header += hash_bytes(template["coinbasetxn"]["txid"])
    header += hash_bytes(template["utreexocommitment"])
    # Anchor HEIGHT remains zero even when its time comes from block one.
    # Exact ideal elapsed time keeps historical ASERT excess zero. The node
    # independently checks its own ancestry, difficulty and real PoW.
    timestamp = time_anchor["time"] + height * 120
    header += timestamp.to_bytes(8, "little")
    header += anchor_bits.to_bytes(4, "little")
    header += bytes(16)  # nonce followed by the required zero reserved bytes
    assert len(header) == 128
    target = 0x7fffff << (8 * (0x20 - 3))
    for nonce in range(nonce_start, nonce_start + 1_000_000):
        header[112:116] = nonce.to_bytes(4, "little")
        digest = hashlib.sha256(hashlib.sha256(header).digest()).digest()
        if int.from_bytes(digest, "big") <= target:
            # Canonical ordinary wire: header, CompactSize(1), exact coinbase,
            # then the absent optional Utreexo-data flag. Root remains bound.
            return bytes(header) + b"\x01" + coinbase + b"\x00", digest.hex()
    raise RuntimeError("No historical regtest nonce in explicit fixture bound")


class RpcError(RuntimeError):
    def __init__(self, description, *, method=None, error=None):
        super().__init__(description)
        self.method = method
        self.code = error.get("code") if isinstance(error, dict) else None
        self.rpc_message = error.get("message") if isinstance(error, dict) else None


class Node:
    def __init__(self, binary, migrator, *, coupled_release=False):
        self.binary = str(Path(binary).resolve())
        self.migrator = str(Path(migrator).resolve())
        self.root = Path(tempfile.mkdtemp(prefix="dinero-orchard-wallet-cycle-")).resolve()
        self.datadir = self.root / "original"
        self.datadir.mkdir()
        # Keep configuration outside the datadir: enforced-PoW admission
        # requires the initial datadir to contain only its actual lock/pid.
        self.config = self.root / "dinero.conf"
        self.pow_profile = None
        self.coupled_release = coupled_release
        self.config.write_text("p2p.offline=true\npool.accounting.enable=true\n"
                               "lightning.oracles.enable=false\nwallet.socket.enable=false\n")
        self.process = None
        self.starts = 0
        self.wallet = "orchard-cycle"
        self.password = "isolated-orchard-cycle-passphrase"

    def pace_rpc(self):
        # The actual HTTP server admits 50 requests/second with a 100-token
        # burst. This single-threaded fixture stays below that allowance and
        # keeps its schedule across daemon restarts. No request is retried.
        not_before = getattr(self, "_rpc_not_before", 0.0)
        while True:
            delay = not_before - time.monotonic()
            if delay <= 0:
                break
            time.sleep(delay)
        # Anchor to the actual send opportunity, not an old deadline: slow
        # responses must not create a later burst of accumulated slots.
        self._rpc_not_before = time.monotonic() + 0.04

    def raw(self, method, params=None):
        if self.process.poll() is not None:
            raise RuntimeError(f"daemon exited: {self.process.returncode}")
        cookie_path = next((p for p in [self.datadir / ".cookie", self.datadir / "regtest/.cookie"]
                            if p.exists()), None)
        if cookie_path is None:
            raise FileNotFoundError("RPC cookie not ready")
        cookie = cookie_path.read_bytes().strip()
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                           "params": [] if params is None else params}).encode()
        request = urllib.request.Request(f"http://127.0.0.1:{self.ports[0]}/", body,
                  {"Content-Type": "application/json",
                   "Authorization": "Basic " + base64.b64encode(cookie).decode()})
        self.pace_rpc()
        with urllib.request.urlopen(request, timeout=60) as response:
            reply = json.load(response)
        # Retain only explicit non-secret outcome fields. Never serialize
        # request parameters, credentials, mnemonics or signed transaction bytes.
        observed = {"process_start": self.starts, "method": method,
                    "transport_error": reply.get("error") not in (None, False)}
        result = reply.get("result")
        if method == "getconsensusinfo" and isinstance(result, dict):
            for key in ("regtest_pow_enforced", "consensus_checksum",
                        "sixty_second_activation_height", "target_spacing_seconds",
                        "target_spacing_height", "shielded_compact_activation_height",
                        "shielded_compact_active", "network", "genesis_hash", "genesis_time",
                        "genesis_bits", "pow_limit_bits"):
                if key in result:
                    observed[key] = result[key]
        if method == "getblockcount" and type(result) is int:
            observed["height"] = result
        elif method == "getbestblockhash" and isinstance(result, str):
            observed["block_hash"] = result
        elif method == "wallet.orchard.listoperations" and isinstance(result, dict):
            for key in ("account", "account_revision", "account_sequence", "account_digest",
                        "captured_source_sequence", "captured_source_digest"):
                if key in result:
                    observed[key] = result[key]
            if "operations" in result:
                observed["operations"] = []
                for entry in result["operations"]:
                    row = {key: entry[key] for key in ("operation_id", "durable_state", "txid")
                           if key in entry}
                    value = entry.get("chain_observation")
                    row["chain_observation"] = (None if value is None else
                        {key: value[key] for key in ("outcome", "height", "block_hash", "transaction_id")
                         if key in value})
                    observed["operations"].append(row)
        elif method in ("wallet.orchard.queueshield", "wallet.orchard.queuespend",
                        "wallet.orchard.finishshield", "wallet.orchard.finishspend") and isinstance(result, dict):
            for key in ("operation_id", "durable_state", "txid", "admitted", "already_in_mempool"):
                if key in result:
                    observed[key] = result[key]
        if isinstance(result, dict):
            observed["handler_error"] = "error" in result
        if method == "mining.submit":
            observed["result_is_empty_object"] = isinstance(result, dict) and result == {}
        with (self.root / "rpc-observations.jsonl").open("a") as trace:
            trace.write(json.dumps(observed, sort_keys=True) + "\n")
        if reply.get("error") not in (None, False):
            raise RpcError(f"{method}: {reply['error']}", method=method, error=reply["error"])
        return reply["result"]

    def rpc(self, method, params=None):
        result = self.raw(method, params)
        if isinstance(result, dict) and "error" in result:
            raise RpcError(f"{method}: {result['error']}")
        return result

    def wait(self, predicate, label, seconds=90):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.2)
        raise RuntimeError(f"Timeout: {label}")

    def start(self, extra=()):
        self.starts += 1
        holders = [socket.socket() for _ in range(3)]
        for sock in holders:
            sock.bind(("127.0.0.1", 0))
        self.ports = [sock.getsockname()[1] for sock in holders]
        for sock in holders:
            sock.close()
        args = [self.binary, "--regtest", f"--datadir={self.datadir}", f"--conf={self.config}",
                f"--rpcport={self.ports[0]}", f"--port={self.ports[1]}",
                f"--wallet-socket-port={self.ports[2]}", "--listen=0", "--p2p.offline=1",
                "--utreexo=1", "--utreexo-bridge=1", *extra]
        with (self.root / f"process-{self.starts}.log").open("wb") as log:
            self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=log, stderr=log)
        def listening():
            try:
                return isinstance(self.rpc("getblockcount"), int)
            except (OSError, ValueError, RpcError):
                return False
        self.wait(listening, "actual JSON-RPC listener")
        self.check_profile()

    def check_profile(self):
        height = self.rpc("getblockcount")
        assert type(height) is int and height >= 0
        profile = self.rpc("getconsensusinfo")
        assert profile["regtest_pow_enforced"] is True
        activation = 102 if self.coupled_release else 1
        next_height = height + 1
        assert type(profile["target_spacing_height"]) is int
        assert profile["target_spacing_height"] == next_height
        assert type(profile["sixty_second_activation_height"]) is int
        assert profile["sixty_second_activation_height"] == activation
        expected_spacing = 60 if next_height >= activation else 120
        assert type(profile["target_spacing_seconds"]) is int
        assert profile["target_spacing_seconds"] == expected_spacing
        if self.coupled_release:
            assert type(profile["shielded_compact_activation_height"]) is int
            assert profile["shielded_compact_activation_height"] == activation
            assert profile["shielded_compact_active"] is (next_height >= activation)
        checksum = profile["consensus_checksum"]
        assert isinstance(checksum, str) and len(checksum) == 64
        assert all(c in "0123456789abcdef" for c in checksum)
        assert (self.datadir / "regtest-pow-profile").read_text() == (
            "regtest-pow-profile-v1\n" + checksum + "\n")
        if self.pow_profile is None:
            self.pow_profile = checksum
        else:
            assert checksum == self.pow_profile, "PoW profile changed across restart or migration"
        # The historical genesis header is shared with mainnet, whereas the
        # non-mainnet ASERT anchor uses this selected network's PoW limit.
        assert profile["network"] == "regtest"
        assert profile["pow_limit_bits"] == "0x207fffff"
        assert type(profile["genesis_bits"]) is int and profile["genesis_bits"] == 0x1d31ffce
        assert type(profile["genesis_time"]) is int and profile["genesis_time"] == 1776384000
        genesis_hash = profile["genesis_hash"]
        assert isinstance(genesis_hash, str) and len(genesis_hash) == 64
        assert all(c in "0123456789abcdef" for c in genesis_hash)
        self.historical_pow_bits = int(profile["pow_limit_bits"], 16)
        self.historical_genesis = {"hash": genesis_hash, "time": profile["genesis_time"],
                                   "bits": profile["genesis_bits"]}
        return height, expected_spacing

    def stop(self):
        process = self.process
        if process is None:
            return
        if process.poll() is not None:
            raise RuntimeError(f"daemon exited before requested stop: {process.returncode}")
        process.stdin.write(b"stop\n")
        process.stdin.flush()
        process.stdin.close()
        assert process.wait(timeout=45) == 0, "daemon did not stop cleanly"
        self.process = None
        log = (self.root / f"process-{self.starts}.log").read_text(errors="replace")
        assert log.count("PASS Orchard wallet cycle daemon stopped normally") == 1
        assert log.count("Typed runtime notifications installed; consumer recovery remains asynchronous") == 1

    def restart(self, reindex=False, accounts=(3, 17)):
        tip = self.rpc("getbestblockhash")
        self.stop()
        self.start(["--reindex"] if reindex else [])
        self.rpc("wallet.open", [self.wallet])
        assert self.rpc("wallet.unlock", [self.password, 3600])["success"] is True
        assert self.rpc("getbestblockhash") == tip
        for account in accounts:
            self.synced(account)

    def migrate_storage(self):
        # Explicit stopped original/candidate operation through the reviewed
        # native qualification tool. No direct database edits or layout fence.
        tip = self.rpc("getbestblockhash")
        height = self.rpc("getblockcount")
        self.stop()
        original = self.datadir
        candidate = self.root / "separated"
        assert not candidate.exists()

        def preserved_files():
            result = {}
            for path in original.rglob("*"):
                assert not path.is_symlink(), "isolated datadir must not contain symlinks"
                if path.is_file() and path.name not in ("LOCK", "dinerod.lock") and not path.name.startswith("LOG"):
                    result[str(path.relative_to(original))] = hashlib.sha256(path.read_bytes()).hexdigest()
            return result

        before = preserved_files()
        shutil.copytree(original, candidate)
        with (self.root / "migration.log").open("wb") as output:
            migrated = subprocess.run([self.migrator, str(original), str(candidate), "--apply"],
                                      stdout=output, stderr=subprocess.STDOUT, timeout=120)
        assert preserved_files() == before, "migration changed preserved original files"
        assert migrated.returncode == 0, "native copied-datadir migration refused; see private migration.log"
        lines = (self.root / "migration.log").read_text().splitlines()
        assert sum(line.startswith("ok=true ready=true ") for line in lines) == 1
        self.datadir = candidate
        self.start()
        self.rpc("wallet.open", [self.wallet])
        assert self.rpc("wallet.unlock", [self.password, 3600])["success"] is True
        assert self.rpc("getblockcount") == height and self.rpc("getbestblockhash") == tip
        print("PASS JSON-RPC explicit copied storage migration preserved original and selected tip", flush=True)

    def synced(self, account):
        def caught_up():
            view = self.rpc("wallet.orchard.listoperations", {"account": account})
            return (view["account_sequence"] == view["captured_source_sequence"] and
                    view["account_digest"] == view["captured_source_digest"])
        self.wait(caught_up, f"account {account} actual provider delivery")
        return self.rpc("wallet.orchard.listoperations", {"account": account})

    def status(self, account, request_id):
        view = self.synced(account)
        entries = [o for o in view["operations"] if o["operation_id"] == request_id]
        assert len(entries) == 1, "original durable operation missing or duplicated"
        return entries[0]

    def confirmed(self, account, request_id, txid, block_hash, height):
        entry = self.status(account, request_id)
        assert entry["durable_state"] == "signed" and entry["txid"] == txid
        assert entry["chain_observation"] == {
            "outcome": "confirmed", "height": height,
            "block_hash": block_hash, "transaction_id": txid}

    def generate(self, count, address):
        before = self.rpc("getblockcount")
        if self.coupled_release and before < 101:
            assert type(count) is int and 1 <= count <= 101 - before
            result = {"blocks": [self.mine_historical(address) for _ in range(count)]}
        else:
            result = self.rpc("generatetoaddress", [count, address])
        assert isinstance(result, dict), "generation response must be an object"
        blocks = result["blocks"]
        assert isinstance(blocks, list) and len(blocks) == count
        assert all(isinstance(value, str) and len(value) == 64 and
                   all(c in "0123456789abcdef" for c in value) for value in blocks)
        assert len(set(blocks)) == count, "generation returned duplicate block hashes"
        assert self.rpc("getblockcount") == before + count
        assert self.rpc("getbestblockhash") == blocks[-1]
        return blocks

    def mine_historical(self, address):
        assert self.coupled_release
        before = self.rpc("getblockcount")
        assert type(before) is int and 0 <= before < 101
        genesis_hash = self.rpc("getblockhash", [0])
        genesis = self.rpc("getblockheader", [genesis_hash])
        assert type(genesis["height"]) is int and genesis["height"] == 0
        assert genesis["hash"] == genesis_hash == self.historical_genesis["hash"]
        assert {key: genesis[key] for key in ("hash", "time", "bits")} == self.historical_genesis
        time_anchor_height = 0 if before == 0 else 1
        time_anchor_hash = genesis_hash if before == 0 else self.rpc("getblockhash", [1])
        time_anchor = genesis if before == 0 else self.rpc("getblockheader", [time_anchor_hash])
        assert time_anchor["hash"] == time_anchor_hash
        assert type(time_anchor["height"]) is int and time_anchor["height"] == time_anchor_height
        template = self.rpc("getblocktemplate", {"address": address})
        assert template["height"] == before + 1
        assert template["previousblockhash"] == self.rpc("getbestblockhash")
        # Each ordinary restart uses a disjoint nonce range so replacement
        # cannot resubmit the invalidated historical header with the same body.
        wire, block_hash = historical_template_block(template, genesis, self.historical_pow_bits,
            self.starts * 1_000_000, time_anchor=time_anchor)
        assert self.rpc("submitblock", [wire.hex()]) == {}
        assert self.rpc("getblockcount") == before + 1
        assert self.rpc("getbestblockhash") == block_hash
        accepted = self.rpc("getblockheader", [block_hash])
        assert accepted["time"] == time_anchor["time"] + (before + 1) * 120
        assert type(accepted["bits"]) is int and accepted["bits"] == self.historical_pow_bits
        assert accepted["hash"] == block_hash and accepted["height"] == before + 1
        observation = {
            "process_start": self.starts, "height": before + 1,
            "genesis_hash": genesis_hash, "genesis_time": genesis["time"],
            "genesis_bits": genesis["bits"], "anchor_bits": self.historical_pow_bits,
            "asert_time_anchor_height": time_anchor_height,
            "asert_time_anchor_hash": time_anchor_hash, "asert_time_anchor_time": time_anchor["time"],
            "consensus_checksum": self.pow_profile, "header_hex": wire[:128].hex(),
            "accepted": {key: accepted[key] for key in (
                "hash", "height", "time", "bits", "nonce", "previousblockhash",
                "merkleroot", "utreexo_root")}}
        # Only public header facts; never archive wallet keys or RPC credentials.
        with (self.root / "historical-proof-observations.jsonl").open("a") as trace:
            trace.write(json.dumps(observation, sort_keys=True) + "\n")
        return block_hash

    def mine(self, address):
        before = self.rpc("getblockcount")
        job = self.rpc("mining.getjob", {"address": address})
        assert job["height"] == before + 1
        header = bytearray.fromhex(job["header_hex"])
        offset, size = job["nonce_offset"], job["nonce_size"]
        assert size == 4 and 0 <= offset <= len(header) - size
        target = int(job["target"], 16)
        for nonce in range(1_000_000):
            header[offset:offset + size] = nonce.to_bytes(size, "little")
            digest = hashlib.sha256(hashlib.sha256(header).digest()).digest()
            if int.from_bytes(digest, "big") <= target:
                # The HTTP adapter normalizes this handler's successful null
                # to an empty object; canonical height/hash checks follow.
                assert self.rpc("mining.submit", {"job_id": job["job_id"], "nonce": nonce}) == {}
                assert self.rpc("getblockcount") == before + 1
                assert self.rpc("getbestblockhash") == digest.hex()
                return digest.hex()
        raise RuntimeError("No regtest nonce in explicit fixture bound")

    def operation(self, account, request_id, payments, outputs=None):
        request = {"account": account, "request_id": request_id,
                   "expected_revision": self.synced(account)["account_revision"],
                   "payments": payments, "fee_una": 10000}
        if outputs is not None:
            request["outputs"] = outputs
        return request

    def ready(self, kind, request):
        queued = self.rpc("wallet.orchard.queue" + kind, request)
        assert queued["operation_id"] == request["request_id"] and queued["durable_state"] == "reserved"
        result = None
        pending = {"shield": "Owned shield proof is not complete",
                   "spend": "Owned Orchard proof is not available; reservation retained"}[kind]
        method = "wallet.orchard.finish" + kind
        def finished():
            nonlocal result
            try:
                value = self.raw(method, request)
            except RpcError as error:
                # The HTTP dispatcher promotes the handler's string error.
                # Retry only the exact expected unfinished-proof response.
                if (error.method == method and type(error.code) is int and
                        error.code == -32603 and error.rpc_message == pending):
                    return False
                raise
            if isinstance(value, dict) and "error" in value:
                if value["error"] == pending:
                    return False
                raise RpcError(f"finish{kind}: {value['error']}")
            result = value
            return True
        self.wait(finished, f"real {kind} proof and admission", seconds=180)
        assert result["admitted"] is True and result["durable_state"] == "signed"
        txid = result["txid"]
        entry = self.status(request["account"], request["request_id"])
        assert entry["durable_state"] == "signed" and entry["txid"] == txid
        assert entry["chain_observation"] is None
        # Process restart must retain exact signed identity; retry cannot prove a replacement.
        self.restart()
        retry = self.rpc("wallet.orchard.finish" + kind, request)
        assert retry["txid"] == txid and (retry["admitted"] or retry["already_in_mempool"])
        assert self.status(request["account"], request["request_id"]) == entry
        return txid


def main():
    assert len(sys.argv) in (3, 4)
    coupled_release = len(sys.argv) == 4
    if coupled_release:
        assert sys.argv[3] == "--coupled-release"
    node = Node(sys.argv[1], sys.argv[2], coupled_release=coupled_release)
    print(f"Private isolated evidence: {node.root}", flush=True)
    try:
        node.start()
        print("PASS JSON-RPC enforced PoW profile bound before wallet history", flush=True)
        created = node.rpc("wallet.createhd", [node.wallet, 12, "", node.password])
        assert created["success"] is True
        created.clear()  # Never print/archive the returned mnemonic.
        assert node.rpc("wallet.unlock", [node.password, 3600])["success"] is True
        address = node.rpc("wallet.getnewaddress", ["taproot", "cycle funding"])["address"]
        ordinary = node.generate(2, address)
        assert len(ordinary) == 2 and node.rpc("getblockcount") == 2
        node.restart(accounts=())  # No Orchard accounts exist before activation.
        node.rpc("blockchain.invalidateblock", [ordinary[-1]])
        assert node.rpc("getblockcount") == 1 and node.rpc("getbestblockhash") == ordinary[0]
        node.restart(accounts=())
        assert len(node.generate(100, address)) == 100
        assert node.rpc("getblockcount") == 101
        print("PASS JSON-RPC ordinary validation tip survived restart disconnect and replacement", flush=True)
        node.migrate_storage()
        activation_parent = node.rpc("getbestblockhash")
        if coupled_release:
            assert node.check_profile() == (101, 60)  # NEXT block selects the boundary rules.
        activation = node.mine(address)  # Actual selected-parent activation at regtest height102.
        if coupled_release:
            assert node.check_profile() == (102, 60)
            print("PASS JSON-RPC coupled 120-to-60 compact and Orchard activation", flush=True)
        first = node.rpc("wallet.orchard.createaccount", {"account": 3})
        second = node.rpc("wallet.orchard.createaccount", {"account": 17})
        node.synced(3)
        node.synced(17)
        shield = node.operation(3, "81" + "00" * 31,
                               [{"address": second["address"], "amount_una": 200000, "memo_hex": "0100"}])
        shield_txid = node.ready("shield", shield)
        shield_block = node.mine(address)
        node.confirmed(3, shield["request_id"], shield_txid, shield_block, node.rpc("getblockcount"))
        node.synced(17)
        print("PASS JSON-RPC shield survived restart and confirmed through actual provider", flush=True)
        send = node.operation(17, "82" + "00" * 31,
                              [{"address": first["address"], "amount_una": 100000}], [])
        send_txid = node.ready("spend", send)
        assert send_txid != shield_txid
        send_block = node.mine(address)
        node.confirmed(17, send["request_id"], send_txid, send_block, node.rpc("getblockcount"))
        node.synced(3)
        print("PASS JSON-RPC shielded send survived restart and confirmed through actual provider", flush=True)
        destination = node.rpc("wallet.getnewaddress", ["taproot", "cycle unshield"])["address"]
        unshield = node.operation(3, "83" + "00" * 31, [],
                                 [{"address": destination, "amount_una": 90000}])
        unshield_txid = node.ready("spend", unshield)
        parent = node.rpc("getbestblockhash")
        block = node.mine(address)
        node.confirmed(3, unshield["request_id"], unshield_txid, block, node.rpc("getblockcount"))
        def coin_present():
            coins = [c for c in node.rpc("wallet.listunspent", [0]) if c["txid"] == unshield_txid]
            return len(coins) == 1 and coins[0]["amount_una"] == 90000 and coins[0]["spendable"] is True
        node.wait(coin_present, "exact confirmed unshield coin")
        node.restart()
        assert coin_present()
        node.rpc("blockchain.invalidateblock", [block])
        assert node.rpc("getbestblockhash") == parent
        node.restart()
        assert not any(c["txid"] == unshield_txid for c in node.rpc("wallet.listunspent", [1]))
        undone = node.status(3, unshield["request_id"])
        assert undone["durable_state"] == "signed" and undone["txid"] == unshield_txid
        assert undone["chain_observation"] is None
        replay = node.rpc("wallet.orchard.finishspend", unshield)
        assert replay["txid"] == unshield_txid and (replay["admitted"] or replay["already_in_mempool"])
        replacement = node.mine(address)
        node.confirmed(3, unshield["request_id"], unshield_txid, replacement, node.rpc("getblockcount"))
        node.wait(coin_present, "same signed unshield restored after reorg")
        node.restart()
        assert coin_present()
        print("PASS JSON-RPC unshield exact identity survived restart reorg and readmission", flush=True)
        before_reindex = {a: node.synced(a) for a in (3, 17)}
        node.restart(reindex=True)
        assert coin_present()
        for account, before in before_reindex.items():
            after = node.synced(account)
            assert after["operations"] == before["operations"]
            assert after["account_sequence"] == before["account_sequence"]
            assert after["account_digest"] == before["account_digest"]
        print("PASS JSON-RPC full reindex preserved confirmed unshield and account progress", flush=True)
        expected_signed = {3: {shield["request_id"]: shield_txid, unshield["request_id"]: unshield_txid},
                           17: {send["request_id"]: send_txid}}

        def retained_reindex(expect_unconfirmed=True):
            before = {a: node.synced(a) for a in (3, 17)}
            for account, view in before.items():
                actual = {o["operation_id"]: o for o in view["operations"]}
                assert set(actual) == set(expected_signed[account])
                for request_id, txid in expected_signed[account].items():
                    assert actual[request_id]["txid"] == txid
                    assert actual[request_id]["durable_state"] == "signed"
                    if expect_unconfirmed:
                        assert actual[request_id]["chain_observation"] is None
            node.restart(reindex=True)
            for account, original in before.items():
                after = node.synced(account)
                assert after["operations"] == original["operations"]
                assert after["account_sequence"] == original["account_sequence"]
                assert after["account_digest"] == original["account_digest"]

        node.rpc("blockchain.invalidateblock", [activation])
        assert node.rpc("getblockcount") == 101
        assert node.rpc("getbestblockhash") == activation_parent
        if coupled_release:
            assert node.check_profile() == (101, 60)
        retained_reindex()
        print("PASS JSON-RPC reindex below activation retained exact signed operations and source progress", flush=True)
        node.rpc("blockchain.invalidateblock", [activation_parent])
        assert node.rpc("getblockcount") == 100
        if coupled_release:
            assert node.check_profile() == (100, 120)
        retained_reindex()
        print("PASS JSON-RPC historical disconnect reindex retained delivery chronology", flush=True)
        replacement_parent = node.generate(1, address)[0]
        assert replacement_parent != activation_parent
        replacement_activation = node.mine(address)
        assert replacement_activation != activation and node.rpc("getblockcount") == 102
        if coupled_release:
            assert node.check_profile() == (102, 60)
        retained_reindex(expect_unconfirmed=False)
        if coupled_release:
            assert node.check_profile() == (102, 60)
            print("PASS JSON-RPC coupled timing rewind reactivation and reindex retained profile", flush=True)
        print("PASS JSON-RPC historical replacement and reactivation reindex retained original accounts", flush=True)
        node.stop()
    finally:
        # Stop only this isolated child. A timed-out graceful stop is a failure;
        # do not call it a successful shutdown or remove its evidence.
        if node.process is not None and node.process.poll() is None:
            try:
                node.stop()
            except Exception:
                node.process.kill()
                node.process.wait()
                raise


if __name__ == "__main__":
    main()

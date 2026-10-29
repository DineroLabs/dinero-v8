#!/usr/bin/env python3
"""Actual isolated daemon: explicit vault creation, attachment and restart."""
import base64
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

binary = str(Path(sys.argv[1]).resolve())
root = Path(tempfile.mkdtemp(prefix="dinero-vault-explicit-startup-"))
datadir = root / "node"
datadir.mkdir()
config = datadir / "dinero.conf"
config.write_text("p2p.offline=true\n")
process = None
output = None
success = False
wallet = "explicit-vault-owner"
password = "isolated-vault-startup-qualification"
sockets = []
try:
    for _ in range(3):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
    rpc_port, p2p_port, wallet_port = [s.getsockname()[1] for s in sockets]
    for sock in sockets:
        sock.close()
    command = [binary, "--regtest", f"--datadir={datadir}", f"--conf={config}",
               f"--rpcport={rpc_port}", f"--port={p2p_port}",
               f"--wallet-socket-port={wallet_port}", "--listen=0", "--utreexo=1"]

    def rpc(method, params=None, expect_error=False):
        cookie = next(p.read_text().strip() for p in
                      [datadir / ".cookie", datadir / "regtest" / ".cookie"] if p.exists())
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                           "params": [] if params is None else params}).encode()
        request = urllib.request.Request(f"http://127.0.0.1:{rpc_port}/", body,
            {"Authorization": "Basic " + base64.b64encode(cookie.encode()).decode(),
             "Content-Type": "application/json"})
        try:
            response = urllib.request.urlopen(request, timeout=15)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            envelope = json.load(response)
        result = envelope.get("result")
        failed = bool(envelope.get("error")) or (isinstance(result, dict) and bool(result.get("error")))
        # Do not print returned wallet recovery phrases or any credential.
        assert failed == expect_error, f"Unexpected RPC outcome for {method}"
        if expect_error and envelope.get("error") is not None:
            # The HTTP adapter promotes handler errors to the JSON-RPC envelope
            # and clears result. Preserve the message for the refusal assertions.
            error = envelope["error"]
            assert isinstance(error, dict) and isinstance(error.get("message"), str)
            return {"error": error["message"]}
        return result

    def start(label):
        global process, output
        output = (root / (label + ".log")).open("w")
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            assert process.poll() is None, f"Daemon exited during {label}"
            try:
                if rpc("getblockcount") == 0:
                    return
            except (OSError, StopIteration, ValueError):
                pass
            time.sleep(0.1)
        raise AssertionError(f"No authenticated RPC readiness during {label}")

    def stop():
        global process, output
        rpc("stop")
        assert process.wait(timeout=90) == 0, "Daemon failed clean shutdown"
        process = None
        output.close()
        output = None

    def detached():
        operator = rpc("vault.getoperator")
        assert operator["enabled"] is False and operator["address"] == "" and operator["account"] == ""
        rpc("vault.metrics", expect_error=True)

    def owner_database():
        matches = []
        for path in datadir.rglob("*.db"):
            with sqlite3.connect(path.as_uri() + "?mode=ro", uri=True) as db:
                if db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='wallet_vault_states'").fetchone():
                    matches.append(path)
        assert len(matches) == 1, "Expected exactly one isolated wallet with vault owners"
        return matches[0]

    def owner_rows(path):
        with sqlite3.connect(path.as_uri() + "?mode=ro", uri=True) as db:
            return (db.execute("SELECT vault_id,revision,predecessor,sealed FROM wallet_vault_states ORDER BY vault_id").fetchall(),
                    db.execute("SELECT COUNT(*) FROM addresses").fetchone()[0])

    def selected_wallet_rows(name):
        paths = list(datadir.rglob("wallet_" + name + ".db"))
        assert len(paths) == 1, "Expected exact selected wallet database"
        with sqlite3.connect(paths[0].as_uri() + "?mode=ro", uri=True) as db:
            db.execute("BEGIN")
            # Keep complete row/schema data local; never print recovery material.
            return tuple(db.iterdump())

    def refuse_existing_creation(name):
        before = selected_wallet_rows(name)
        info = rpc("wallet.getinfo")
        selection = {key: info[key] for key in
                     ("wallet_name", "encrypted", "locked", "available_wallets")}
        attempts = [[name, 12, "", "replacement-password", "bip86", True],
                    [name, 12, "", "replacement-password", "bip86", "true"],
                    {"name": name, "password": "replacement-password", "replace_existing": True},
                    [name, 12, "", "", "bip86"]]
        for params in attempts:
            result = rpc("wallet.createhd", params, expect_error=True)
            assert "new wallet name" in result["error"]
            assert selected_wallet_rows(name) == before, "Rejected create changed wallet owner rows"
            after = rpc("wallet.getinfo")
            assert {key: after[key] for key in selection} == selection

    start("fresh")
    detached()
    assert not list(datadir.rglob("ledger.jsonl")), "Startup created a legacy vault ledger"
    assert not list(datadir.rglob("idempotency.jsonl")), "Startup created legacy vault identities"
    print("PASS default daemon startup leaves vault detached", flush=True)
    created_wallet = rpc("wallet.createhd", [wallet, 12, "", password])
    assert created_wallet["success"] is True
    created_wallet.clear()
    assert rpc("wallet.unlock", [password, 3600])["success"] is True
    detached()
    rpc("vault.list", expect_error=True)
    address = rpc("wallet.getnewaddress", ["taproot", "explicit vault operator"])["address"]
    policy = {"operator_address": address, "account_id": "owned-account", "shadow_mode": True,
              "k_observe": 1, "k_credit": 2, "k_settle": 6, "withdrawal_k_settle": 6,
              "per_deposit_cap_una": 1000, "per_account_cap_una": 2000, "global_cap_una": 3000,
              "per_withdrawal_cap_una": 500, "per_account_outstanding_cap_una": 1000, "max_queue_depth": 10}
    first = rpc("vault.create", [policy])
    assert first["created"] is True and first["attached"] is False and first["wallet"] == wallet
    genesis = rpc("getblockhash", [0])
    assert first["creation_height"] == 0 and first["creation_block_hash"] == genesis
    creation_anchor = {"creation_anchor_present": True, "creation_height": 0, "creation_block_hash": genesis}
    def assert_creation_anchors():
        current = rpc("vault.list")
        assert current["historical_completeness_verified"] is False
        assert current["vaults"]
        for row in current["vaults"]:
            assert {key: row[key] for key in creation_anchor} == creation_anchor
    first_id = first["vault_id"]
    assert len(first_id) == 64 and int(first_id, 16) != 0
    detached()
    listed = rpc("vault.list")
    assert listed["historical_completeness_verified"] is False
    assert [v["vault_id"] for v in listed["vaults"]] == [first_id]
    assert rpc("vault.open", [{"vault_id": first_id}])["attached"] is True
    assert rpc("vault.getoperator")["account"] == "owned-account"
    print("PASS encrypted wallet explicitly creates lists and attaches authenticated vault", flush=True)
    policy["account_id"] = "second-account"
    second = rpc("vault.create", [policy])
    second_id = second["vault_id"]
    assert second_id != first_id and second["attached"] is False
    assert rpc("vault.getoperator")["account"] == "owned-account"
    rpc("vault.open", [{"vault_id": second_id}], expect_error=True)
    assert rpc("vault.getoperator")["account"] == "owned-account"
    assert {v["vault_id"] for v in rpc("vault.list")["vaults"]} == {first_id, second_id}
    assert_creation_anchors()
    # Refusal is checked only on this patched daemon. Both parameter forms and
    # the legacy string override must preserve all existing owner records.
    refuse_existing_creation(wallet)
    rpc("wallet.lock")
    refuse_existing_creation(wallet)
    assert rpc("wallet.unlock", [password, 3600])["success"] is True
    rpc("wallet.unlock", ["replacement-password", 3600], expect_error=True)
    assert rpc("wallet.getinfo")["locked"] is False
    unencrypted = "explicit-unencrypted-owner"
    created_plain = rpc("wallet.createhd", [unencrypted, 12, "", ""])
    assert created_plain["success"] is True
    created_plain.clear()
    refuse_existing_creation(unencrypted)
    refuse_existing_creation(wallet)  # inactive encrypted owner; selection stays unchanged
    assert rpc("wallet.open", [wallet])["locked"] is True
    assert rpc("wallet.unlock", [password, 3600])["success"] is True
    assert rpc("vault.getoperator")["account"] == "owned-account"
    print("PASS wallet creation requires new name and preserves every existing owner row", flush=True)
    stop()
    db_path = owner_database()
    original = owner_rows(db_path)
    print("PASS new owner never replaces attached or existing vaults", flush=True)

    start("reopen")
    detached()
    assert rpc("wallet.open", [wallet])["locked"] is True
    rpc("vault.list", expect_error=True)
    assert rpc("wallet.unlock", [password, 3600])["success"] is True
    detached()
    assert {v["vault_id"] for v in rpc("vault.list")["vaults"]} == {first_id, second_id}
    assert_creation_anchors()
    assert rpc("vault.open", [{"vault_id": first_id}])["attached"] is True
    assert rpc("vault.getoperator")["account"] == "owned-account"
    stop()
    assert owner_rows(db_path) == original, "Reopen changed vault rows or issued addresses"
    print("PASS restart requires explicit attachment and preserves exact owner rows", flush=True)

    legacy = datadir / "vault"
    legacy.mkdir(exist_ok=True)
    originals = {legacy / "ledger.jsonl": b"legacy ledger bytes requiring separate recovery\n",
                 legacy / "idempotency.jsonl": b"legacy identity bytes requiring separate recovery\n"}
    for path, data in originals.items():
        assert not path.exists(), "Unexpected preexisting legacy evidence path"
        path.write_bytes(data)
    config.write_text("p2p.offline=true\nvault=true\nvault.address=" + address +
                      "\nvault.ledgerpath=" + str(legacy / "ledger.jsonl") + "\n")
    start("legacy-config")
    detached()
    assert rpc("wallet.open", [wallet])["locked"] is True
    assert rpc("wallet.unlock", [password, 3600])["success"] is True
    detached()
    assert {v["vault_id"] for v in rpc("vault.list")["vaults"]} == {first_id, second_id}
    assert_creation_anchors()
    stop()
    assert all(path.read_bytes() == data for path, data in originals.items())
    assert owner_rows(db_path) == original
    text = (root / "legacy-config.log").read_text(errors="replace")
    assert "Legacy automatic-start settings do not attach a vault" in text
    print("PASS legacy enable settings and files cannot initialize or overwrite a vault", flush=True)
    print("PASS canonical creation anchor survives actual daemon restarts without delivery claim", flush=True)
    print("PASS explicit vault startup HTTP lifecycle and clean shutdown", flush=True)
    success = True
finally:
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    if output is not None:
        output.close()
    for sock in sockets:
        sock.close()
    if success and os.environ.get("DINERO_KEEP_VAULT_STARTUP_EVIDENCE") != "1":
        shutil.rmtree(root)
    else:
        print(f"Retained explicit vault startup evidence: {root}", flush=True)

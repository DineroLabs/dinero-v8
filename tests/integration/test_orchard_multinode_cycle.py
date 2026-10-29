#!/usr/bin/env python3
"""Orchard across real nodes: activation, shield, send, unshield, reorg.

Node A (wallet, miner, Utreexo bridge) runs the same JSON-RPC cycle as
test_orchard_daemon_wallet_cycle.py. Followers connect to it over localhost
P2P and must independently validate every block:
  B  full node             tip, Utreexo roots and Orchard rule status agree;
                           A's Orchard transactions reach B's mempool; B
                           survives a restart and a full reindex.
  C  stateless (CSN) node  reported, not assumed: the Orchard boundary is
                           documented to refuse stateless replay, so C's
                           outcome at and after activation is recorded.
Every node runs test_orchard_wallet_cycle_daemon, the regtest entry point that
schedules Orchard at height 102 (production dinerod leaves it unset).
Regtest only, isolated temporary directories, no wallet or chain database
edits. Usage: test_orchard_multinode_cycle.py CYCLE_DAEMON MIGRATOR [--migrate-followers | --no-manual-migration | --fresh-no-restart]
"""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_orchard_daemon_wallet_cycle import Node, RpcError  # noqa: E402


def free_ports(n):
    holders = [socket.socket() for _ in range(n)]
    for sock in holders:
        sock.bind(("127.0.0.1", 0))
    ports = [sock.getsockname()[1] for sock in holders]
    for sock in holders:
        sock.close()
    return ports


class Miner(Node):
    """Node A: the wallet-cycle node, listening for P2P on localhost."""

    def __init__(self, binary, migrator):
        super().__init__(binary, migrator)
        self.config.write_text("pool.accounting.enable=true\n"
                               "lightning.oracles.enable=false\nwallet.socket.enable=false\n")

    def stop(self):
        # A fresh datadir with the automatic upgrade runs a bootstrap DaemonApp
        # in the same process first, so startup lines appear twice in this log.
        process = self.process
        if process is None:
            return
        if process.poll() is not None:
            raise RuntimeError(f"daemon exited before requested stop: {process.returncode}")
        process.stdin.write(b"stop\n")
        process.stdin.flush()
        process.stdin.close()
        assert process.wait(timeout=60) == 0, "daemon did not stop cleanly"
        self.process = None
        log = (self.root / f"process-{self.starts}.log").read_text(errors="replace")
        assert log.count("PASS Orchard wallet cycle daemon stopped normally") == 1

    def start(self, extra=()):
        self.wallet_binding = None
        self.starts += 1
        # A fresh automatic upgrade boots a temporary daemon to persist the
        # genesis records, stops it, then starts the real daemon on the same
        # RPC port. A response from that first listener is not readiness.
        wait_for_upgrade = (os.environ.get("DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE") == "1"
                            and not (self.datadir / "blockchain" / "chaindb" / "CURRENT").exists())
        if not hasattr(self, "fixed_ports"):
            self.fixed_ports = free_ports(3)  # followers keep the same address across restarts
        self.ports = list(self.fixed_ports)
        args = [self.binary, "--regtest", f"--datadir={self.datadir}", f"--conf={self.config}",
                f"--rpcport={self.ports[0]}", f"--port={self.ports[1]}",
                f"--wallet-socket-port={self.ports[2]}", "--listen=1",
                "--dnsseed=0", "--fixedseeds=0", "--discover=0", "--listenonion=0",
                "--utreexo=1", "--utreexo-bridge=1", *extra]
        process_log = self.root / f"process-{self.starts}.log"
        with process_log.open("wb") as log:
            self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=log, stderr=log)

        def listening():
            if wait_for_upgrade and "Shielded-state storage upgraded" not in process_log.read_text(errors="replace"):
                return False
            try:
                return isinstance(self.rpc("getblockcount"), int)
            except (OSError, ValueError, RpcError):
                return False
        self.wait(listening, "miner JSON-RPC listener")
        self.check_profile()


class Follower(Node):
    """A node without a wallet that only follows A over P2P."""

    def __init__(self, binary, name, connect_port, stateless=False, migrator=None):
        super().__init__(binary, migrator or binary)
        self.name = name
        self.connect_port = connect_port
        self.stateless = stateless
        self.config.write_text("lightning.oracles.enable=false\nwallet.socket.enable=false\n")

    def start(self, extra=()):
        self.wallet_binding = None
        self.starts += 1
        wait_for_upgrade = (not self.stateless and
                            os.environ.get("DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE") == "1" and
                            not (self.datadir / "blockchain" / "chaindb" / "CURRENT").exists())
        self.ports = free_ports(3)
        utreexo = ["--utreexo=1", "--utreexo-stateless=1"] if self.stateless else ["--utreexo=1"]
        args = [self.binary, "--regtest", f"--datadir={self.datadir}", f"--conf={self.config}",
                f"--rpcport={self.ports[0]}", f"--port={self.ports[1]}",
                f"--wallet-socket-port={self.ports[2]}", "--listen=0", "--dnsseed=0",
                "--fixedseeds=0", "--discover=0", "--listenonion=0",
                f"--connect=127.0.0.1:{self.connect_port}", *utreexo, *extra]
        process_log = self.root / f"process-{self.starts}.log"
        with process_log.open("wb") as log:
            self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=log, stderr=log)

        def listening():
            if wait_for_upgrade and "Shielded-state storage upgraded" not in process_log.read_text(errors="replace"):
                return False
            try:
                return isinstance(self.rpc("getblockcount"), int)
            except (OSError, ValueError, RpcError):
                return False
        self.wait(listening, f"{self.name} JSON-RPC listener")

    def stop(self):
        process = self.process
        if process is None:
            return
        if process.poll() is not None:
            raise RuntimeError(f"{self.name} exited before requested stop: {process.returncode}")
        process.stdin.write(b"stop\n")
        process.stdin.flush()
        process.stdin.close()
        assert process.wait(timeout=60) == 0, f"{self.name} did not stop cleanly"
        self.process = None

    def migrate(self):
        """Same stopped copy + qualification migrator as Node.migrate_storage, without a wallet."""
        tip = self.rpc("getbestblockhash")
        self.stop()
        candidate = self.root / "separated"
        assert not candidate.exists()
        shutil.copytree(self.datadir, candidate)
        with (self.root / "migration.log").open("wb") as output:
            migrated = subprocess.run([self.migrator, str(self.datadir), str(candidate), "--apply"],
                                      stdout=output, stderr=subprocess.STDOUT, timeout=120)
        assert migrated.returncode == 0, f"{self.name} migration refused; see {self.root}/migration.log"
        self.datadir = candidate
        self.start()
        assert self.rpc("getbestblockhash") == tip
        print(f"PASS {self.name} storage migrated at its tip", flush=True)

    def alive(self):
        return self.process is not None and self.process.poll() is None


def state(node):
    roots = node.rpc("blockchain.getutreexoroots")
    activation = node.rpc("orchard.getactivationstatus")
    return {"tip": node.rpc("getbestblockhash"), "height": node.rpc("getblockcount"),
            "num_leaves": roots.get("num_leaves"), "roots": roots.get("roots"),
            "orchard": {k: activation.get(k) for k in sorted(activation)
                        if k not in ("observed_at", "time")}}


def follow(miner, follower, label, seconds=180):
    want = state(miner)

    def caught_up():
        try:
            return follower.rpc("getbestblockhash") == want["tip"]
        except (OSError, ValueError, RpcError):
            return False
    follower.wait(caught_up, f"{follower.name} reaches {label}", seconds)
    got = state(follower)
    assert got == want, f"{follower.name} disagrees after {label}:\n  miner    {want}\n  follower {got}"
    print(f"PASS {follower.name} agrees after {label} (height {got['height']})", flush=True)


def relayed(miner, follower, txid, label):
    def seen():
        pool = follower.rpc("getrawmempool")
        pool = pool if isinstance(pool, list) else list(pool)
        return txid in pool
    follower.wait(seen, f"{label} relayed to {follower.name}", 60)
    print(f"PASS {label} {txid[:16]} relayed to {follower.name} mempool", flush=True)


def report_stateless(miner, c, label):
    """C is observed, not required: record exactly where it stands."""
    if not c.alive():
        print(f"INFO C (stateless) not running at {label}: exit {c.process.returncode if c.process else None}", flush=True)
        return
    try:
        tip, height = c.rpc("getbestblockhash"), c.rpc("getblockcount")
    except Exception as error:  # noqa: BLE001 - recorded, not hidden
        print(f"INFO C (stateless) RPC failed at {label}: {error}", flush=True)
        return
    same = tip == miner.rpc("getbestblockhash")
    print(f"INFO C (stateless) at {label}: height {height}, {'same tip as A' if same else 'DIFFERENT tip from A'}",
          flush=True)


def light_node_notice(node):
    """The light-node Orchard notice as both status RPCs report it."""
    info = node.rpc("getnetworkinfo")
    status = node.rpc("orchard.getactivationstatus")
    return info.get("warnings", ""), status.get("node_supported"), status.get("node_notice", "")


def check_notices(full, light, label):
    warn, supported, notice = light_node_notice(light)
    assert supported is False and "light" in notice.lower() and "full node" in notice.lower(), \
        f"light node C lacks the Orchard notice at {label}: {supported!r} {notice!r}"
    assert notice in warn, f"getnetworkinfo.warnings misses the notice at {label}: {warn!r}"
    fwarn, fsupported, fnotice = light_node_notice(full)
    assert fsupported is True and fnotice == "" and "light" not in fwarn.lower(), \
        f"full node B shows a light-node notice at {label}: {fsupported!r} {fnotice!r} {fwarn!r}"
    print(f"PASS light node C reports the Orchard notice at {label}; full node B does not", flush=True)


def main():
    assert len(sys.argv) in (3, 4) and all(f in ("--migrate-followers", "--no-manual-migration", "--fresh-no-restart") for f in sys.argv[3:])
    if {"--no-manual-migration", "--fresh-no-restart"} & set(sys.argv[3:]):
        # Every node upgrades its own store at startup, as production dinerod does.
        os.environ["DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE"] = "1"
    a = Miner(sys.argv[1], sys.argv[2])
    print(f"Private isolated evidence: A {a.root}", flush=True)
    followers = []
    try:
        a.start()
        migrate_followers = "--migrate-followers" in sys.argv[3:]
        # The release target: no node is migrated by hand; each one must reach
        # the Orchard storage layout on its own.
        no_manual_migration = bool({"--no-manual-migration", "--fresh-no-restart"} & set(sys.argv[3:]))
        # Fresh installs: every node starts on a brand-new datadir and none
        # restarts before activation, so each must start on the new layout.
        fresh_no_restart = "--fresh-no-restart" in sys.argv[3:]
        b = Follower(sys.argv[1], "B", a.ports[1], migrator=sys.argv[2])
        c = Follower(sys.argv[1], "C", a.ports[1], stateless=True, migrator=sys.argv[2])
        followers = [b, c]
        print(f"Private isolated evidence: B {b.root}  C {c.root}", flush=True)
        b.start()
        c.start()

        created = a.rpc("wallet.createhd", [a.wallet, 12, "", a.password])
        assert created["success"] is True
        created.clear()  # never print or keep the mnemonic
        assert a.rpc("wallet.unlock", [a.password, 3600])["success"] is True
        a.refresh_wallet_binding()
        address = a.rpc("wallet.getnewaddress", ["taproot", "multinode funding"])["address"]
        assert len(a.generate(101, address)) == 101
        follow(a, b, "101 ordinary blocks")
        report_stateless(a, c, "height 101")
        check_notices(b, c, "height 101 (before activation)")
        # Building Orchard blocks needs the separated shielded-state layout,
        # which only the qualification migrator creates (no daemon/operator
        # entry point yet). Migrate A only; B and C stay on the layout a node
        # starts with, so this also shows whether unmigrated nodes validate.
        if fresh_no_restart:
            pass
        elif no_manual_migration:
            # Existing nodes restart onto the new binary before activation.
            a.restart(accounts=())
            follow(a, b, "A restart (automatic storage upgrade)")
            b.stop()
            b.start()
            follow(a, b, "B restart (automatic storage upgrade)")
        else:
            a.migrate_storage()
            follow(a, b, "A storage migration and restart")
        if migrate_followers and not no_manual_migration:
            b.migrate()
            follow(a, b, "B storage migration")

        activation = a.mine(address)  # selected-parent Orchard activation at regtest height 102
        follow(a, b, "Orchard activation")
        report_stateless(a, c, "activation")
        check_notices(b, c, "activation")

        first = a.rpc("wallet.orchard.createaccount", {"account": 3})
        second = a.rpc("wallet.orchard.createaccount", {"account": 17})
        a.synced(3)
        a.synced(17)

        shield = a.operation(3, "91" + "00" * 31,
                             [{"address": second["address"], "amount_una": 200000, "memo_hex": "0100"}])
        shield_txid = a.ready("shield", shield)
        relayed(a, b, shield_txid, "shield")
        shield_block = a.mine(address)
        a.confirmed(3, shield["request_id"], shield_txid, shield_block, a.rpc("getblockcount"))
        a.synced(17)
        follow(a, b, "shield block")

        send = a.operation(17, "92" + "00" * 31, [{"address": first["address"], "amount_una": 100000}], [])
        send_txid = a.ready("spend", send)
        relayed(a, b, send_txid, "Orchard send")
        send_block = a.mine(address)
        a.confirmed(17, send["request_id"], send_txid, send_block, a.rpc("getblockcount"))
        a.synced(3)
        follow(a, b, "send block")

        destination = a.rpc("wallet.getnewaddress", ["taproot", "multinode unshield"])["address"]
        unshield = a.operation(3, "93" + "00" * 31, [], [{"address": destination, "amount_una": 90000}])
        unshield_txid = a.ready("spend", unshield)
        relayed(a, b, unshield_txid, "unshield")
        parent = a.rpc("getbestblockhash")
        block = a.mine(address)
        a.confirmed(3, unshield["request_id"], unshield_txid, block, a.rpc("getblockcount"))
        follow(a, b, "unshield block")
        report_stateless(a, c, "after shield/send/unshield")

        # Reorg on A: the unshield block is replaced; B must follow to the replacement.
        a.rpc("blockchain.invalidateblock", [block])
        assert a.rpc("getbestblockhash") == parent
        a.synced(3)  # the wallet applies the disconnect asynchronously; finish after it has
        replay = a.rpc("wallet.orchard.finishspend", unshield)
        assert replay["txid"] == unshield_txid and (replay["admitted"] or replay["already_in_mempool"])
        replacement = a.mine(address)
        filler = a.mine(address)  # longer branch, so B reorganizes rather than keeps the old block
        a.confirmed(3, unshield["request_id"], unshield_txid, replacement, a.rpc("getblockcount") - 1)
        follow(a, b, "reorg to the replacement unshield block")
        assert b.rpc("getblockheader", [filler])["height"] == a.rpc("getblockcount")

        # B restart, then a full reindex of B: same tip and state again.
        b.stop()
        b.start()
        follow(a, b, "B restart")
        b.stop()
        b.start(["--reindex"])
        follow(a, b, "B full reindex", seconds=300)
        report_stateless(a, c, "end")
        print("PASS Orchard multi-node cycle", flush=True)
    finally:
        for node in [*followers, a]:
            if node.process is not None and node.process.poll() is None:
                try:
                    node.stop()
                except Exception:  # noqa: BLE001 - kill below, failure already raised
                    node.process.kill()
                    node.process.wait()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""A store upgraded at genesis must equal a store upgraded later, block for block.

F starts on a brand-new datadir with the automatic upgrade (bootstrap at
genesis, then the separated layout from height 0). It mines 100 blocks and a
legacy shield, so the anchor history and commitment tree are not empty. L
starts as a legacy node, syncs the same blocks from F over P2P, then restarts
with the upgrade on. Both must report the same daemon.shieldedstatehash
(tree, nullifiers, anchor history) and the same tip and Utreexo roots.
Usage: CYCLE_DAEMON MIGRATOR
"""
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_orchard_multinode_cycle import Follower, Miner, follow  # noqa: E402


def shielded(node):
    h = node.rpc("daemon.shieldedstatehash")
    return h if isinstance(h, str) else {k: v for k, v in h.items() if k not in ("time", "observed_at")}


def main():
    assert len(sys.argv) == 3
    os.environ["DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE"] = "1"
    f = Miner(sys.argv[1], sys.argv[2])
    l = None
    print(f"Private isolated evidence: F {f.root}", flush=True)
    try:
        f.start()  # fresh: bootstrap writes genesis records, then upgrades at genesis
        flog = (f.root / "process-1.log").read_text(errors="replace")
        assert "Fresh store genesis records persisted" in flog and "Shielded-state storage upgraded" in flog
        os.environ.pop("DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE")
        l = Follower(sys.argv[1], "L", f.ports[1], migrator=sys.argv[2])
        l.start()  # legacy layout
        print(f"Private isolated evidence: L {l.root}", flush=True)

        assert f.rpc("wallet.createhd", [f.wallet, 12, "", f.password])["success"] is True
        assert f.rpc("wallet.unlock", [f.password, 3600])["success"] is True
        address = f.rpc("wallet.getnewaddress", ["taproot", "equivalence"])["address"]
        f.generate(100, address)
        empty_root = f.rpc("daemon.shieldedroot")["shielded_root"]
        shield = f.rpc("wallet.shield", {"amount_una": 50_000_000})
        assert isinstance(shield, dict) and not shield.get("error"), f"legacy shield failed: {shield}"
        f.generate(1, address)  # height 101, before Orchard activation at 102
        assert f.rpc("daemon.shieldedroot")["shielded_root"] != empty_root, "the legacy shield was not mined"
        follow(f, l, "100 blocks and a legacy shield")
        before = shielded(f)
        assert before == shielded(l), "shielded state differs before L's upgrade"

        l.stop()
        os.environ["DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE"] = "1"
        l.start()  # upgrade of a legacy store at height 101
        llog = (l.root / f"process-{l.starts}.log").read_text(errors="replace")
        assert "Shielded-state storage upgraded" in llog
        follow(f, l, "L upgraded at height 101")
        assert shielded(l) == shielded(f) == before, \
            f"shielded state differs:\n  F (upgraded at genesis) {shielded(f)}\n  L (upgraded at 101)    {shielded(l)}"
        print(f"PASS genesis-upgraded and later-upgraded stores agree: {before}", flush=True)

        # Both continue identically across the Orchard activation block.
        activation = f.mine(address)
        follow(f, l, "Orchard activation")
        assert l.rpc("getbestblockhash") == activation and shielded(l) == shielded(f)
        print("PASS both cross Orchard activation with the same state", flush=True)
    finally:
        for node in [l, f]:
            if node is not None and node.process is not None and node.process.poll() is None:
                node.stop()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Reconsider failed header-only rows that startup cannot index.

The child and a separate root have durable ChainDB metadata but deliberately
have neither bodies nor HeaderStore rows. The real RPC must clear both after
restart even though CBlockIndex cannot enumerate them.
"""

from pathlib import Path
import subprocess
import sys

from test_orchard_daemon_wallet_cycle import Node


def main():
    assert len(sys.argv) == 3
    node = Node(sys.argv[1], sys.argv[1])
    helper = str(Path(sys.argv[2]).resolve())
    child = "f" * 63 + "1"
    print(f"Private isolated evidence: {node.root}", flush=True)
    try:
        node.start()
        created = node.rpc("wallet.createhd", [node.wallet, 12, "", node.password])
        assert created["success"] is True
        created.clear()  # Never archive mnemonic material.
        assert node.rpc("wallet.unlock", [node.password, 3600])["success"] is True
        address = node.rpc("wallet.getnewaddress", ["taproot", "header reconsider"])['address']
        root, tip = node.generate(2, address)
        node.rpc("blockchain.invalidateblock", [root])
        assert node.rpc("getblockcount") == 0
        node.stop()

        chain_db = str(node.datadir / "blockchain" / "chaindb")
        def probe(mode, parent=root, header_hash=child):
            subprocess.run([helper, mode, chain_db, parent, header_hash], check=True,
                           timeout=30)

        probe("--inject-pruned-failed-child")
        probe("--check-pruned-failed-child")
        node.start()
        assert node.rpc("getblockcount") == 0
        assert node.rpc("getdaemonstatus")["sync"]["best_header"]["height"] == 0
        node.rpc("blockchain.reconsiderblock", [root])
        sync = node.rpc("getdaemonstatus")["sync"]
        assert sync["best_header"]["height"] >= 2
        node.stop()

        # This fails if ReconsiderBlock only walks its in-memory subtree.
        probe("--check-cleared-pruned-child")
        node.start()
        assert node.rpc("getdaemonstatus")["sync"]["best_header"]["height"] >= 2
        assert node.rpc("getbestblockhash") == tip
        genesis = node.rpc("getblockhash", [0])
        node.stop()
        print("PASS reconsider cleared pruned header-only child across restart", flush=True)

        # The failed root itself can also be absent from CBlockIndex after its
        # header is evicted. It must be reconsiderable by durable metadata,
        # without pretending a body-backed missing index is healthy.
        missing_root = "e" * 63 + "1"
        probe("--inject-pruned-failed-root", genesis, missing_root)
        probe("--check-pruned-failed-root", genesis, missing_root)
        node.start()
        node.rpc("blockchain.reconsiderblock", [missing_root])
        node.stop()
        probe("--check-cleared-pruned-root", genesis, missing_root)
        node.start()
        assert node.rpc("getbestblockhash") == tip
        node.stop()
        print("PASS reconsider cleared pruned metadata-only root across restart", flush=True)
    finally:
        if node.process is not None and node.process.poll() is None:
            node.stop()


if __name__ == "__main__":
    main()

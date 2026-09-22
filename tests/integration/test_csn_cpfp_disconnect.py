#!/usr/bin/env python3
"""Disconnecting a mined parent/child package must not revive its parent coin.

Exercise full-node and CSN durable undo, restart at the disconnected tip, and
reconsider the original block. No database mutation or recovery override is used.
"""
from pathlib import Path

import test_csn_replay_metadata_recovery as h


def require_absent(node, txid):
    result = node.call("gettxout", [txid, 0])
    h.require(result is None, f"{node.name}: disconnected package coin resurrected: {txid}: {result}")


def main():
    h.require(h.BINARY.is_file(), f"missing executable: {h.BINARY}")
    h.RECEIPT.update({"binary_sha256": h.sha256(h.BINARY),
                      "test_sha256": h.sha256(Path(__file__)),
                      "harness_sha256": h.sha256(Path(h.__file__))})
    full, csn = h.Node("full"), h.Node("csn", True)
    full.peer, csn.peer = csn, full
    full.start()
    address = full.call("wallet.getnewaddress")["address"]
    h.mine_mature_prefix(full, address)
    csn.start()
    ancestor = full.call("getbestblockhash")
    h.wait(lambda: csn.call("getbestblockhash") == ancestor, "CSN mature prefix")
    ancestor_root = h.root(full)
    h.require(h.root(csn) == ancestor_root, "initial roots differ")
    ancestor_coins = {node.name: node.call("gettxoutsetinfo") for node in (full, csn)}
    h.require(ancestor_coins["full"] == ancestor_coins["csn"], "initial durable coin sets differ")
    coin = full.call("getblock", [full.call("getblockhash", [1]), 1])["tx"][0]
    original_proof = h.proof(full, csn, coin)

    parent, prevout = h.spend(full, coin, 99.9, address)
    child, _ = h.spend(full, parent, 9.8, address, prevout)
    package = h.mine(full, address, 9_020_000_000, [parent, child])
    h.wait(lambda: csn.call("getbestblockhash") == package, "CSN parent/child package")
    package_root = h.root(full)
    h.require(h.root(csn) == package_root, "package roots differ")
    h.proof(full, csn, child)
    package_coinbase = full.call("getblock", [package, 1])["tx"][0]
    h.RECEIPT.update({"ancestor": ancestor, "ancestor_root": ancestor_root,
                      "package": package, "package_root": package_root,
                      "external_input": coin, "package_parent": parent, "package_child": child})
    h.RECEIPT["ancestor_coins"] = ancestor_coins
    print("PASS real-PoW parent/child package and matching canonical proofs", flush=True)

    # The peer remains stopped during CSN rollback/restart, so a fresh block
    # download cannot repair or conceal an incorrect local disconnect.
    full.stop()
    csn.call("blockchain.invalidateblock", [package])
    h.require(csn.call("getbestblockhash") == ancestor and h.root(csn) == ancestor_root,
              "CSN disconnect changed ancestor identity/root")
    h.RECEIPT["csn_disconnected_coins"] = csn.call("gettxoutsetinfo")
    require_absent(csn, parent)
    require_absent(csn, child)
    require_absent(csn, package_coinbase)
    h.require(h.RECEIPT["csn_disconnected_coins"] == ancestor_coins["csn"],
              "CSN disconnect did not restore exact pre-block durable UTXO set")
    verified = csn.call("blockchain.verifyutxoproofs_batch", [original_proof])
    h.require(verified["valid"] == 1 and verified["invalid"] == 0,
              f"CSN did not restore external input proof: {verified}")
    csn.stop()
    csn.start()
    h.require(csn.call("getbestblockhash") == ancestor and h.root(csn) == ancestor_root,
              "CSN disconnected restart changed ancestor identity/root")
    require_absent(csn, parent)
    require_absent(csn, child)
    require_absent(csn, package_coinbase)
    h.require(csn.call("gettxoutsetinfo") == ancestor_coins["csn"],
              "CSN restart did not preserve exact pre-block durable UTXO set")
    h.require_no_recovery_fuse()
    print("PASS CSN disconnect and offline restart preserve only pre-block coins", flush=True)

    # Keep the bridge on the invalidated package until the CSN actually
    # processes its refetched proof. Immediately invalidating the bridge too
    # hid a race: the worker could advance the canonical forest for a block
    # whose persistent failure flag later prevented chain activation.
    csn_log = h.WORK / "csn.log"
    replay_log_start = csn_log.stat().st_size
    full.start()

    def refetched_package_processed():
        with csn_log.open("rb") as log:
            log.seek(replay_log_start)
            text = log.read().decode(errors="replace")
        # Wait for a completed worker decision, not merely receipt. The old
        # path marks the block connected even though it cannot activate it.
        return (f"Skipping canonical proof for invalidated block {package}" in text or
                f"Block marked CONNECTED: {package}" in text)

    h.wait(refetched_package_processed, "invalidated package proof processed")
    h.require(csn.call("getbestblockhash") == ancestor and h.root(csn) == ancestor_root,
              "refetched invalidated proof changed canonical tip/forest")
    h.require(csn.call("gettxoutsetinfo") == ancestor_coins["csn"],
              "refetched invalidated proof changed durable coins")
    verified = csn.call("blockchain.verifyutxoproofs_batch", [original_proof])
    h.require(verified["valid"] == 1 and verified["invalid"] == 0,
              f"refetched invalidated proof invalidated ancestor proof: {verified}")
    print("PASS refetched invalidated package leaves canonical state unchanged", flush=True)

    full.call("blockchain.invalidateblock", [package])
    h.require(full.call("getbestblockhash") == ancestor and h.root(full) == ancestor_root,
              "full-node disconnect changed ancestor identity/root")
    require_absent(full, parent)
    require_absent(full, child)
    require_absent(full, package_coinbase)
    h.require(full.call("gettxoutsetinfo") == ancestor_coins["full"],
              "full-node disconnect did not restore exact pre-block durable UTXO set")
    h.proof(full, csn, coin)
    full.stop()
    full.start()
    h.require(full.call("getbestblockhash") == ancestor and h.root(full) == ancestor_root,
              "full-node disconnected restart changed ancestor identity/root")
    require_absent(full, parent)
    require_absent(full, child)
    require_absent(full, package_coinbase)
    h.require(full.call("gettxoutsetinfo") == ancestor_coins["full"],
              "full-node restart did not preserve exact pre-block durable UTXO set")
    h.proof(full, csn, coin)
    print("PASS full-node disconnect and restart preserve only pre-block coins", flush=True)

    alternate_address = full.call("wallet.getnewaddress")["address"]
    # Disconnect may return the old package to the mempool. Empty it only
    # after verifying rollback, so the alternate branch contains fresh coins.
    for node in (full, csn):
        node.call("mempool.clear")
    alternate = [h.mine(full, alternate_address) for _ in range(2)]
    h.wait(lambda: csn.call("getbestblockhash") == alternate[-1], "alternate branch reconnect")
    h.require(h.root(full) == h.root(csn), "alternate branch roots differ")
    h.require(full.call("gettxoutsetinfo") == csn.call("gettxoutsetinfo"),
              "alternate branch durable coin sets differ")
    for node in (full, csn):
        require_absent(node, parent)
        require_absent(node, child)
    h.proof(full, csn, coin)
    for node in (csn, full):
        node.call("blockchain.invalidateblock", [alternate[0]])
        h.require(node.call("gettxoutsetinfo") == ancestor_coins[node.name],
                  f"{node.name}: alternate branch rollback changed ancestor coin set")
    print("PASS alternate branch reconnect and rollback preserve exact coin set", flush=True)

    for node in (full, csn):
        node.call("blockchain.reconsiderblock", [package])
    h.wait(lambda: all(node.call("getbestblockhash") == package for node in (full, csn)),
           "parent/child package reconsideration")
    h.require(h.root(full) == package_root and h.root(csn) == package_root,
              "reconnected package roots differ")
    h.proof(full, csn, child)
    for node in (full, csn):
        require_absent(node, parent)
        h.spent(node, original_proof)
    h.require_no_recovery_fuse()
    print("PASS reconsider restores exact package root and live child proof", flush=True)


if __name__ == "__main__":
    h.run_test(main)

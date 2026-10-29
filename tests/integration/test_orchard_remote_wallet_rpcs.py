#!/usr/bin/env python3
"""Mobile remote mode keeps working on a full node after Orchard activation.

DineroDPI's remote mode (light clients against the seed full nodes) reads
chain, address, mempool and proof data over JSON-RPC. After activation every
block is an Orchard block. This drives one regtest node through activation,
an Orchard shield/send/unshield and an ordinary transparent payment, then
makes the app's calls with the app's parameter shapes (DineroDPI
Core/NodeKit.swift, SharedRPCWalletPresentation.swift, WalletAlignment.swift,
ConsensusBridge.swift, TransactionTracker.swift).
Usage: CYCLE_DAEMON MIGRATOR
"""
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_orchard_daemon_wallet_cycle import RpcError  # noqa: E402
from test_orchard_multinode_cycle import Miner  # fresh-datadir bootstrap logs startup twice  # noqa: E402

fails = []


def check(cond, what):
    print(("  OK   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        fails.append(what)


def call(node, method, params=None):
    try:
        return node.rpc(method, params), None
    except (RpcError, OSError, ValueError) as error:
        return None, str(error)


def main():
    assert len(sys.argv) == 3
    os.environ["DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE"] = "1"
    n = Miner(sys.argv[1], sys.argv[2])
    print(f"Private isolated evidence: {n.root}", flush=True)
    try:
        n.start()
        assert n.rpc("wallet.createhd", [n.wallet, 12, "", n.password])["success"] is True
        assert n.rpc("wallet.unlock", [n.password, 3600])["success"] is True
        n.refresh_wallet_binding()
        miner = n.rpc("wallet.getnewaddress", ["taproot", "remote miner"])["address"]
        n.generate(101, miner)
        activation = n.mine(miner)
        first = n.rpc("wallet.orchard.createaccount", {"account": 3})
        second = n.rpc("wallet.orchard.createaccount", {"account": 17})
        n.synced(3)
        n.synced(17)
        shield = n.operation(3, "a1" + "00" * 31, [{"address": second["address"], "amount_una": 200000}])
        shield_txid = n.ready("shield", shield)
        n.mine(miner)
        n.synced(17)
        send = n.operation(17, "a2" + "00" * 31, [{"address": first["address"], "amount_una": 100000}], [])
        send_txid = n.ready("spend", send)
        n.mine(miner)
        n.synced(3)
        receiver = n.rpc("wallet.getnewaddress", ["taproot", "remote receiver"])["address"]
        unshield = n.operation(3, "a3" + "00" * 31, [], [{"address": receiver, "amount_una": 90000}])
        unshield_txid = n.ready("spend", unshield)
        unshield_block = n.mine(miner)
        n.confirmed(3, unshield["request_id"], unshield_txid, unshield_block, n.rpc("getblockcount"))
        # Ordinary transparent payment after activation (every mainnet user's path).
        payee = n.rpc("wallet.getnewaddress", ["taproot", "remote payee"])["address"]
        paid, error = call(n, "wallet.sendtoaddress", [payee, 1.5])
        check(error is None and paid, f"transparent wallet.sendtoaddress after activation ({error or 'ok'})")
        pay_txid = paid if isinstance(paid, str) else (paid or {}).get("txid")
        pool, _ = call(n, "getrawmempool")
        check(isinstance(pool, list) and pay_txid in pool, "transparent payment in the mempool")
        entry, error = call(n, "getmempoolentry", [pay_txid])
        check(error is None, f"getmempoolentry ({error or 'ok'})")
        info, error = call(n, "getmempoolinfo")
        check(error is None, f"getmempoolinfo ({error or 'ok'})")
        addr_pool, error = call(n, "getaddressmempool", [payee])
        check(error is None, f"getaddressmempool [address] ({error or 'ok'})")
        pay_block = n.mine(miner)
        tip = n.rpc("getblockcount")
        print(f"  ..   chain at {tip}; Orchard active since 102", flush=True)

        # Chain reads.
        for method, params in [("getblockcount", None), ("getbestblockhash", None), ("getblockchaininfo", None),
                               ("getnetworkinfo", None), ("getpeerinfo", None), ("blockchain.getibdprogress", None),
                               ("getcheckpoints", None), ("getutreexoroots", None), ("blockchain.getutreexoroots", None),
                               ("getblockhash", [102]), ("getblockheader", [activation]),
                               ("getblockheader", [pay_block])]:
            result, error = call(n, method, params)
            check(error is None and result is not None, f"{method} {params or ''} ({error or 'ok'})")

        # Transactions in Orchard blocks.
        for label, txid in [("shield", shield_txid), ("Orchard send", send_txid), ("unshield", unshield_txid),
                            ("transparent payment", pay_txid)]:
            result, error = call(n, "getrawtransaction", [txid, True])
            check(error is None and isinstance(result, dict), f"getrawtransaction {label} ({error or 'ok'})")

        # Address index across Orchard blocks.
        for label, address, txid, amount in [("unshield receiver", receiver, unshield_txid, 90000),
                                             ("transparent payee", payee, pay_txid, 150000000)]:
            balance, error = call(n, "getaddressbalance", [address])
            got = None
            if isinstance(balance, dict):
                got = balance.get("balance_una", balance.get("balance"))
            check(error is None and got in (amount, amount / 1e8), f"getaddressbalance {label} = {got} ({error or 'ok'})")
            history, error = call(n, "getaddresshistory", [address])
            rows = history if isinstance(history, list) else (history or {}).get("transactions", [])
            seen = any(txid in str(row) for row in rows)
            check(error is None and seen, f"getaddresshistory {label} lists {txid[:16]} ({error or 'ok'})")
            if not seen:
                print(f"       history returned: {str(history)[:600]}", flush=True)
            scan, error = call(n, "blockchain.scantxoutset", [address])
            check(error is None and txid in str(scan), f"blockchain.scantxoutset {label} ({error or 'ok'})")
        # wallet.snapshot's batch scan (same primitive as blockchain.getaddressbatch).
        batch, error = call(n, "blockchain.getaddressbatch", {"addresses": [receiver, payee]})
        check(error is None and unshield_txid in str(batch) and pay_txid in str(batch),
              f"blockchain.getaddressbatch lists the unshield and the payment ({error or str(batch)[:200]})")
        miner_history, error = call(n, "getaddresshistory", [miner])
        check(error is None, f"getaddresshistory miner (coinbases in Orchard blocks) ({error or 'ok'})")

        # After the address scans (so no block read by getblock can help them).
        # Explorers read whole blocks: an Orchard block with Orchard transactions.
        for verbosity in (0, 1, 2):
            block, error = call(n, "getblock", [unshield_block, verbosity])
            listed = unshield_txid in str(block)
            check(error is None and (verbosity == 0 or listed),
                  f"getblock Orchard block verbosity {verbosity} ({error or ('lists unshield' if listed else str(block)[:160])})")
        # Proof updates for a coin created in an Orchard block.
        roots, _ = call(n, "blockchain.getutreexoroots")
        root_from = (roots or {}).get("roots", [""])[0] if isinstance(roots, dict) else ""
        coins = [c for c in (n.rpc("wallet.listunspent", [0]) or []) if c.get("txid") == pay_txid]
        if coins:
            proof, error = call(n, "blockchain.getproofupdates",
                                [{"root_from": root_from, "outpoints": [{"txid": pay_txid, "vout": coins[0]["vout"]}]}])
            ok = isinstance(proof, dict) and proof.get("proofs") and proof["proofs"][0].get("success") is True
            check(error is None and ok, f"blockchain.getproofupdates for a post-activation coin ({error or proof})")
        else:
            check(False, "post-activation payment coin not found for proof update")

        # Server-side wallet reads the remote mode uses.
        for method, params in [("wallet.listunspent", None), ("wallet.getbalance", None),
                               ("wallet.listtransactions", None), ("wallet.listaddresses", None),
                               ("wallet.getsyncstatus", None), ("wallet.getstatus", None),
                               ("wallet.getsyncstate", None),
                               ("wallet.proofstatus", [(n.rpc("blockchain.getutreexocommitment") or {}).get(
                                   "commitment", (roots or {}).get("roots", [""])[0])])]:
            result, error = call(n, method, params)
            check(error is None, f"{method} ({error or 'ok'})")
        n.stop()
    finally:
        if n.process is not None and n.process.poll() is None:
            n.stop()
    print("REMOTE MODE RPCS: " + ("PASS" if not fails else f"FAIL ({len(fails)})"), flush=True)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()

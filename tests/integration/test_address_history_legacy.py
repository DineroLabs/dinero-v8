#!/usr/bin/env python3
"""Address history on a chain without Orchard blocks (backend-OFF builds too).

Plain regtest dinerod: mine, pay an address, mine; getaddresshistory and
blockchain.getaddressbatch must list the payment. Guards the shared address
scan (src/rpc/methods_address_index.cpp) when the Orchard reader is
compiled out. Usage: DINEROD
"""
import base64, json, os, socket, subprocess, sys, tempfile, time, urllib.error, urllib.request


def main():
    dinerod = sys.argv[1]
    root = tempfile.mkdtemp(prefix="dinero-address-history-")
    ports = []
    for _ in range(2):
        s = socket.socket(); s.bind(("127.0.0.1", 0)); ports.append(s.getsockname()[1]); s.close()
    proc = subprocess.Popen([dinerod, "-regtest", "-daemon=0", "-server", "-rpcuser=t", "-rpcpassword=t",
                             f"-rpcport={ports[0]}", f"-port={ports[1]}", f"-datadir={root}", "-listenonion=0",
                             "-discover=0", "-dnsseed=0", "-fixedseeds=0"],
                            stdout=open(os.path.join(root, "log"), "wb"), stderr=subprocess.STDOUT)

    def call(method, params=None):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or []}).encode()
        req = urllib.request.Request(f"http://127.0.0.1:{ports[0]}/", data=body, headers={
            "content-type": "application/json", "authorization": "Basic " + base64.b64encode(b"t:t").decode()})
        for _ in range(60):
            try:
                reply = json.load(urllib.request.urlopen(req, timeout=30))
                return reply.get("result"), reply.get("error")
            except urllib.error.HTTPError as e:
                if e.code != 429: raise
                time.sleep(1)
    try:
        for _ in range(90):
            try:
                if call("getblockcount")[0] is not None: break
            except OSError: pass
            time.sleep(1)
        call("wallet.createhd", ["w"])
        miner = call("wallet.getnewaddress", ["taproot", "m"])[0]
        miner = miner["address"] if isinstance(miner, dict) else miner
        call("generatetoaddress", [101, miner])
        payee = call("wallet.getnewaddress", ["taproot", "p"])[0]
        payee = payee["address"] if isinstance(payee, dict) else payee
        txid = call("wallet.sendtoaddress", [payee, 1.0])[0]
        txid = txid if isinstance(txid, str) else (txid or {}).get("txid")
        call("generatetoaddress", [1, miner])
        history, error = call("getaddresshistory", [payee])
        batch, berror = call("blockchain.getaddressbatch", {"addresses": [payee]})
        ok = error is None and txid in str(history) and berror is None and txid in str(batch)
        print(("PASS" if ok else "FAIL") + f" legacy address history lists {txid}: history={str(history)[:120]} batch_err={berror}")
        if not ok: return 1
        # Positive control for the fail-closed coverage contract: every body of
        # this real chain is readable, so the full scan is complete to genesis.
        tip = call("getblockcount")[0]
        for name, reply in (("history", history), ("batch", batch)):
            coverage = reply.get("history_coverage") or {}
            want = {"scan_from_height": tip, "lowest_scanned_height": 0, "stop_reason": "genesis",
                    "skipped_heights": 0, "skipped_height_list": [], "unresolved_inputs": 0, "chain_changed": False}
            if reply.get("history_complete") is not True or any(coverage.get(k) != v for k, v in want.items()):
                print(f"FAIL {name} full-chain coverage: complete={reply.get('history_complete')} coverage={coverage}")
                return 1
        # The caller's count is a requested bound, not missing data: it is
        # reported as count_limit and must not flip history_complete, since
        # paging clients read a short page as exhaustion.
        limited, lerror = call("getaddresshistory", [payee, 1])
        coverage = (limited or {}).get("history_coverage") or {}
        if lerror is not None or limited.get("history_complete") is not True or \
                coverage.get("stop_reason") != "count_limit" or coverage.get("skipped_heights") != 0:
            print(f"FAIL count-limited history: error={lerror} reply={str(limited)[:300]}")
            return 1
        print("PASS address history coverage: full chain complete to genesis; count limit reported, not incomplete")
        return 0
    finally:
        proc.terminate(); proc.wait(60)


if __name__ == "__main__":
    sys.exit(main())

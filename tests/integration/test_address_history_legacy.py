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
        return 0 if ok else 1
    finally:
        proc.terminate(); proc.wait(60)


if __name__ == "__main__":
    sys.exit(main())

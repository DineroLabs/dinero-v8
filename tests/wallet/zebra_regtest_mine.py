#!/usr/bin/env python3
"""Mine Zebra regtest blocks with chosen timestamps (getblocktemplate + submitblock).

Zebra's own `generate` stamps every block with the earliest time allowed (the
median of the last 11 blocks + 1 s), so a regtest chain sits near 2011 and
creeps forward. The swap tests need Zcash time to follow the wall clock, and
one check needs a block stamped ahead of the median. Regtest has proof of work
off: an all-zero Equihash solution (1344 bytes, as `generate` makes) is accepted.

    zebra_regtest_mine.py <rpc-port> <count> [time | now | +SECONDS]

`now` (default) stamps each block with the current time; `+SECONDS` stamps it
that far past the chain's median time past; a number is used as is. Prints the
submitblock result per block ("null" = accepted) and exits 1 on a rejection.
"""
import json
import struct
import sys
import time
import urllib.request


def rpc(port, method, params):
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}",
        json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode(),
        {"content-type": "application/json"},
    )
    reply = json.load(urllib.request.urlopen(req, timeout=60))
    if reply.get("error"):
        raise RuntimeError(f"{method}: {reply['error']}")
    return reply["result"]


def median_time_past(port, tip):
    times = [rpc(port, "getblockheader", [str(h), True])["time"] for h in range(max(0, tip - 10), tip + 1)]
    return sorted(times)[len(times) // 2]


def mine_one(port, when):
    tpl = rpc(port, "getblocktemplate", [])
    if when == "now":
        stamp = int(time.time())
    elif when.startswith("+"):
        stamp = median_time_past(port, tpl["height"] - 1) + int(when[1:])
    else:
        stamp = int(when)
    stamp = max(stamp, tpl["mintime"])
    # The solution length Zebra's own `generate` uses (1344, the n=200, k=9
    # size); the regtest genesis block has a shorter one, so copy only a 0xfd form.
    tip = bytes.fromhex(rpc(port, "getblock", [tpl["previousblockhash"], 0]))
    sol_len = struct.unpack("<H", tip[141:143])[0] if tip[140] == 0xFD else 1344
    rev = lambda h: bytes.fromhex(h)[::-1]
    header = (
        struct.pack("<I", tpl["version"])
        + rev(tpl["previousblockhash"])
        + rev(tpl["defaultroots"]["merkleroot"])
        + rev(tpl["defaultroots"]["blockcommitmentshash"])
        + struct.pack("<I", stamp)
        + struct.pack("<I", int(tpl["bits"], 16))
        + b"\0" * 32
        + b"\xfd" + struct.pack("<H", sol_len) + b"\0" * sol_len
    )
    txs = [tpl["coinbasetxn"]["data"]] + [t["data"] for t in tpl["transactions"]]
    n = len(txs)
    count = bytes([n]) if n < 0xFD else b"\xfd" + struct.pack("<H", n)
    block = header + count + b"".join(bytes.fromhex(t) for t in txs)
    return rpc(port, "submitblock", [block.hex()])


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    port, count = int(sys.argv[1]), int(sys.argv[2])
    when = sys.argv[3] if len(sys.argv) > 3 else "now"
    for _ in range(count):
        result = mine_one(port, when)
        print(json.dumps(result))
        if result is not None:
            sys.exit(1)


if __name__ == "__main__":
    main()

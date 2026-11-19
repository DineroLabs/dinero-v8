#!/usr/bin/env python3
"""Mine Zebra regtest blocks with chosen timestamps (getblocktemplate + submitblock).

Zebra's own `generate` stamps every block with the earliest time allowed (the
median of the last 11 blocks + 1 s), so a regtest chain sits near 2011 and
creeps forward. The swap tests need Zcash time to follow the wall clock, and
one check needs a block stamped ahead of the median. Regtest has proof of work
off: an all-zero Equihash solution (1344 bytes, as `generate` makes) is accepted.

    zebra_regtest_mine.py <rpc-port> <count> [time | now | +SECONDS] [--with-tx HEX]

`now` (default) stamps each block with the current time; `+SECONDS` stamps it
that far past the chain's median time past; a number is used as is. Prints the
submitblock result per block ("null" = accepted) and exits 1 on a rejection.

`--with-tx HEX` adds a transparent-only v5 transaction the mempool would refuse
(one time-locked past the median, final only by the block's own time) to each
block, recomputing the merkle root and the ZIP 244 block commitments; those
computations are first checked against Zebra's own roots for the template.
"""
import hashlib
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


def blake(person, data):
    return hashlib.blake2b(data, digest_size=32, person=person).digest()


def sha256d(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def compact(data, pos):
    n = data[pos]
    if n < 0xFD:
        return n, pos + 1
    width = {0xFD: 2, 0xFE: 4, 0xFF: 8}[n]
    return int.from_bytes(data[pos + 1:pos + 1 + width], "little"), pos + 1 + width


def v5_digests(raw):
    """(txid, auth digest) in internal byte order, for a transparent-only v5 transaction (ZIP 244)."""
    header, branch = raw[:20], raw[8:12]
    if int.from_bytes(raw[:4], "little") & 0x7FFFFFFF != 5:
        raise ValueError("not a v5 transaction")
    pos = 20
    n_in, pos = compact(raw, pos)
    prevouts, sequences, scripts = b"", b"", b""
    for _ in range(n_in):
        prevouts += raw[pos:pos + 36]
        script_start = pos + 36
        length, pos = compact(raw, script_start)
        scripts += raw[script_start:pos + length]
        pos += length
        sequences += raw[pos:pos + 4]
        pos += 4
    n_out, pos = compact(raw, pos)
    outputs = b""
    for _ in range(n_out):
        start = pos
        length, pos = compact(raw, pos + 8)
        pos += length
        outputs += raw[start:pos]
    if raw[pos:] != b"\0\0\0":
        raise ValueError("not transparent-only (Sapling or Orchard parts present)")
    if n_in or n_out:
        transparent = blake(b"ZTxIdTranspaHash", blake(b"ZTxIdPrevoutHash", prevouts)
                            + blake(b"ZTxIdSequencHash", sequences) + blake(b"ZTxIdOutputsHash", outputs))
    else:
        transparent = blake(b"ZTxIdTranspaHash", b"")
    txid = blake(b"ZcashTxHash_" + branch, blake(b"ZTxIdHeadersHash", header) + transparent
                 + blake(b"ZTxIdSaplingHash", b"") + blake(b"ZTxIdOrchardHash", b""))
    auth = blake(b"ZTxAuthHash_" + branch, blake(b"ZTxAuthTransHash", scripts if n_in else b"")
                 + blake(b"ZTxAuthSapliHash", b"") + blake(b"ZTxAuthOrchaHash", b""))
    return txid, auth


def merkle(leaves, combine, pad):
    level = list(leaves)
    if pad:  # ZIP 244 auth data tree: zero leaves up to a power of two
        while len(level) & (len(level) - 1):
            level.append(b"\0" * 32)
    while len(level) > 1:
        if len(level) % 2:
            level.append(level[-1])
        level = [combine(level[i] + level[i + 1]) for i in range(0, len(level), 2)]
    return level[0]


def roots(txids, auths, history_root):
    merkle_root = merkle(txids, sha256d, False)
    auth_root = merkle(auths, lambda d: blake(b"ZcashAuthDatHash", d), True)
    return merkle_root, auth_root, blake(b"ZcashBlockCommit", history_root + auth_root + b"\0" * 32)


def mine_one(port, when, extra_txs=()):
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
    merkle_root = rev(tpl["defaultroots"]["merkleroot"])
    commitments = rev(tpl["defaultroots"]["blockcommitmentshash"])
    if extra_txs:
        entries = [tpl["coinbasetxn"]] + tpl["transactions"]
        txids = [rev(t["hash"]) for t in entries]
        auths = [rev(t["authdigest"]) for t in entries]
        history = rev(tpl["defaultroots"]["chainhistoryroot"])
        coinbase = v5_digests(bytes.fromhex(tpl["coinbasetxn"]["data"]))
        if (coinbase != (txids[0], auths[0])
                or roots(txids, auths, history) != (merkle_root, rev(tpl["defaultroots"]["authdataroot"]), commitments)):
            raise RuntimeError("block root computation disagrees with Zebra's template")
        for raw in extra_txs:
            txid, auth = v5_digests(bytes.fromhex(raw))
            txids.append(txid)
            auths.append(auth)
        merkle_root, _, commitments = roots(txids, auths, history)
    header = (
        struct.pack("<I", tpl["version"])
        + rev(tpl["previousblockhash"])
        + merkle_root
        + commitments
        + struct.pack("<I", stamp)
        + struct.pack("<I", int(tpl["bits"], 16))
        + b"\0" * 32
        + b"\xfd" + struct.pack("<H", sol_len) + b"\0" * sol_len
    )
    txs = [tpl["coinbasetxn"]["data"]] + [t["data"] for t in tpl["transactions"]] + list(extra_txs)
    n = len(txs)
    count = bytes([n]) if n < 0xFD else b"\xfd" + struct.pack("<H", n)
    block = header + count + b"".join(bytes.fromhex(t) for t in txs)
    return rpc(port, "submitblock", [block.hex()])


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    args = sys.argv[1:]
    extra = []
    while "--with-tx" in args:
        i = args.index("--with-tx")
        extra.append(args[i + 1])
        del args[i:i + 2]
    port, count = int(args[0]), int(args[1])
    when = args[2] if len(args) > 2 else "now"
    for _ in range(count):
        result = mine_one(port, when, extra)
        print(json.dumps(result))
        if result is not None:
            sys.exit(1)


if __name__ == "__main__":
    main()

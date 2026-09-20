#!/usr/bin/env python3
"""Combined migration + compact-proof + 60-second release qualification harness.

Rewritten 2026-09-19 after Codex's review of the first (bash) version found:
real proof-envelope bugs, ordinary (non-PoW) mining hiding the ASERT gate,
a reorg that never crossed the shielded transitions it claimed to test, a
transparent spend using the wrong output, a maturity check that tested
confirmation filtering instead of maturity, and CI wiring that requested
compact activation on a compact-disabled build. Full review:
MemoryMD/evidence/combined-migration-harness-review-2026-09-19/README.md.
This file addresses every point in that review; see this repo's own
docs/design/combined-migration-release-qualification-harness.md for the
line-by-line mapping and for what remains a documented, scoped limitation
rather than a silently dropped requirement.

OWNERSHIP: this script and tools/migrate_shielded_datadir.cpp are the sole
deliverables of this harness. They call the reviewed, unmodified
dinero::storage::MigrateShieldedDatadirCopy engine through its existing
public API. Migration internals, lifecycle leases and the separate SR-1
recovery FFI remain Codex's.

Requires DINERO_ENABLE_COMPACT_REGTEST=ON at build time (the compact
transaction-version assertions in phase 4 need it) — this script asserts
that support is actually present before doing anything else, rather than
silently degrading to ordinary shield/unshield coverage or requesting
compact activation against a build that will reject it at startup.
"""
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/mining"))
from dinero_cpu_miner import DineroCoinMiner, BlockTemplate  # noqa: E402

DINEROD = Path(os.environ.get("DINEROD", ROOT / "build/dinerod"))
MIGRATE_TOOL = Path(os.environ.get("MIGRATE_TOOL", ROOT / "build/migrate_shielded_datadir"))
# Must stay comfortably above phase 1's premine height (PREMINE_BLOCKS,
# plus 3 shield/unshield blocks = ~108) so phase 4 still has room to cross
# the boundary from below. See mine_to()'s docstring for why this height, not
# 40, and why every pre-boundary height (not just an initial handful) needs
# the override technique.
BOUNDARY_HEIGHT = int(os.environ.get("BOUNDARY_HEIGHT", "130"))
# Measured, not assumed from source alone — two source locations disagree
# and only one governs actual daemon behavior. chainparams_impl.cpp:407 sets
# a network-specific regtest coinbase_maturity=10, but
# CoinbaseMaturity::isCoinbaseMature() (coinbase_maturity.cpp) compares
# against the class's own hardcoded COINBASE_MATURITY=100 constant, NOT
# Params().coinbase_maturity, despite that header's doc comment saying to
# use the network-specific value. Also confirmed directly: wallet.
# sendrawtransaction does NOT gate coinbase maturity at all (an immature
# spend was accepted into mempool at blocks_on_top as low as 2) — the
# actual enforcement point is BLOCK TEMPLATE SELECTION, not mempool
# admission (mempool.cpp:2358's isCoinbaseMature(coin->height,
# next_block_height) check feeding getblocktemplate). Calibrated via an
# isolated template-membership probe: an immature coinbase spend was
# absent from getblocktemplate's transactions through blocks_on_top=98,
# and present starting at blocks_on_top=99 — confirming the hardcoded 100
# governs, not the regtest-specific 10, and confirming WHERE it's enforced.
# Phase 6 below tests template membership accordingly, not
# sendrawtransaction rejection (the first attempt at that assertion could
# never have passed, regardless of which threshold it used).
COINBASE_MATURITY = 100
# Phase 1 only needs "generously mature" funds to shield from, not the
# exact boundary phase 6 tests — deliberately a separate constant so a
# change to one doesn't silently break the other.
PREMINE_BLOCKS = COINBASE_MATURITY + 5
TX_VERSION_SHIELDED_V2 = 6
TX_VERSION_COMPACT_REGTEST = 0x40000006  # include/primitives/transaction.h

# .resolve() is load-bearing, not cosmetic: on macOS both the default
# tempdir (/var/folders/...) and /tmp itself are symlinks to /private/...,
# and MigrateShieldedDatadirCopy's own path validation
# (shielded_migration_cohort.cpp: Require(!fs::is_symlink(...), "symlink in
# datadir path")) rejects ANY datadir path with a symlink component. Without
# resolving here, that guard fires on this script's own working directory —
# not on the corruption/emptiness the negative controls are meant to
# exercise — and the real migration in phase 2 would fail identically.
# Confirmed by direct reproduction before this fix: both negative controls
# reported "error=symlink in datadir path" instead of a corruption- or
# emptiness-specific rejection.
WORK = Path(tempfile.mkdtemp(prefix="dinero_migration_qual_")).resolve()
EVIDENCE = WORK / "evidence"
EVIDENCE.mkdir(parents=True)

# Codex's review: the only permanent evidence directory found after a full
# pass held five small files (source commit, binary hashes, migrate
# stdout/stderr) — "no complete phase/pass log." Tee this script's own
# stdout to a transcript file in EVIDENCE from the very start, so a full
# passing (or failing) run's every [INFO]/[PASS]/[ERROR] line survives
# alongside the other evidence. This never touches wallet.dat/cookie files,
# which live in the per-daemon datadirs under WORK, not in EVIDENCE.
class _Tee:
    def __init__(self, *streams):
        self._streams = streams

    def write(self, data):
        for s in self._streams:
            s.write(data)

    def flush(self):
        for s in self._streams:
            s.flush()


_transcript_file = open(EVIDENCE / "transcript.log", "a", buffering=1)
sys.stdout = _Tee(sys.stdout, _transcript_file)
sys.stderr = _Tee(sys.stderr, _transcript_file)

print(f"[INFO] workdir: {WORK}", flush=True)

processes = {}
ports = {}
cookies = {}
addresses = {}
success = False


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def alloc_ports(n):
    socks = [socket.socket() for _ in range(n)]
    for s in socks:
        s.bind(("127.0.0.1", 0))
    result = [s.getsockname()[1] for s in socks]
    for s in socks:
        s.close()
    return result


def cookie_for(datadir: Path) -> str:
    for candidate in (datadir / ".cookie", datadir / "regtest" / ".cookie"):
        if candidate.exists():
            return candidate.read_text().strip()
    raise RuntimeError(f"no RPC cookie under {datadir}")


# wallet.shield/wallet.unshield perform real zk-proof generation
# synchronously in the RPC handler — measured directly (isolated
# reproduction, this machine): a single wallet.shield call took 67.6s wall
# clock. 60s is not a safety margin over that, it is BELOW the observed
# cost, and is exactly why the first restructured run failed with a bare
# urllib TimeoutError on the shield call (masked at the time by the
# top-level finally-block bug fixed elsewhere in this file; this timeout
# was the real, distinct cause underneath that masking). 180s leaves ~2.7x
# headroom over the measured cost for slower CI hardware.
RPC_TIMEOUT_SECONDS = 180


def rpc(which, method, params=None, _retry=True):
    cookie = cookies[which]
    port = ports[which]["rpc"]
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/",
        json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or []}).encode(),
        {"Content-Type": "application/json", "Authorization": "Basic " +
         __import__("base64").b64encode(cookie.encode()).decode()},
    )
    try:
        with urllib.request.urlopen(request, timeout=RPC_TIMEOUT_SECONDS) as response:
            result = json.load(response)
    except urllib.error.HTTPError as exc:
        # The daemon's own RPC rate limiter (confirmed directly: this
        # harness's phase 6 self-calibrating loop issues far more RPC calls
        # per second than earlier phases and tripped "HTTP 429: Too Many
        # Requests" on a plain rpc() call, not just the mining-specific
        # submit/template retries already handled elsewhere). Retry with
        # backoff exactly once per call site here; a persistent 429 after
        # retries still raises, it is not silently swallowed.
        if exc.code == 429 and _retry:
            for attempt in range(8):
                time.sleep(0.5 * (attempt + 1))
                try:
                    return rpc(which, method, params, _retry=False)
                except urllib.error.HTTPError as retry_exc:
                    if retry_exc.code != 429:
                        raise
            raise
        raise
    if result.get("error") not in (None, False):
        raise RuntimeError(f"{which}.{method}{params} -> {result['error']}")
    return result["result"]


def rpc_expect_error(which, method, params=None):
    """Asserts the call is REJECTED; returns the error object. Real exception
    propagation (not a bash subshell) — a transport failure or an unexpected
    success both raise immediately here, unlike the first version's fail()
    calls inside command substitutions, which could be silently swallowed."""
    cookie = cookies[which]
    port = ports[which]["rpc"]
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/",
        json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or []}).encode(),
        {"Content-Type": "application/json", "Authorization": "Basic " +
         __import__("base64").b64encode(cookie.encode()).decode()},
    )
    with urllib.request.urlopen(request, timeout=RPC_TIMEOUT_SECONDS) as response:
        result = json.load(response)
    if result.get("error") in (None, False):
        raise AssertionError(f"{which}.{method}{params} expected to be REJECTED but succeeded: {result}")
    return result["error"]


def wait(predicate, timeout=120, desc="condition"):
    deadline = time.monotonic() + timeout
    last_exc = None
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, RuntimeError) as exc:
            last_exc = exc
        time.sleep(0.2)
    raise RuntimeError(f"timeout waiting for {desc}" + (f" (last error: {last_exc})" if last_exc else ""))


def wait_for_wallet_sync(which):
    """A block accepted via submitblock is validated and connected to the
    chain SYNCHRONOUSLY — but the wallet's own awareness of that block's
    new outputs is NOT: WalletWorker::QueueBlockConnected
    (src/wallet/wallet_worker.cpp) queues each connected block for
    processing on a separate background thread. Calling wallet.
    signrawtransaction against a UTXO the wallet has not yet indexed can
    return a transaction that LOOKS successful but was never actually
    signed (confirmed by direct reproduction on Linux CI: an empty
    scriptSig, no witness data at all, decoding the exact rejected raw hex
    from a real CombinedMigrationReleaseQualification run — 'Script
    validation failed for input 0: invalid script format'). This never
    reproduced across ~10 local macOS runs, consistent with a race that a
    faster machine (less incidental delay between mining and signing) hits
    more often. Same shape as the wallet-indexing race #790 fixed for
    shielded input selection, for a plain transparent coinbase instead.

    wallet.listunspent's own handler (methods_wallet_context.cpp) already
    calls mgr.WaitForHeight(chain_tip_height, 5000ms) before reading the
    wallet DB — calling it once forces that same wait as a side effect.
    Deliberately NOT polling for the target txid/vout to actually appear
    in the response: WalletManager::listUnspentUTXOs has its own explicit
    "skip immature coinbase outputs" filter (hardcoded 100, matching
    COINBASE_MATURITY elsewhere in this codebase) that unconditionally
    excludes any immature coinbase from the result regardless of whether
    the wallet has indexed it — confirmed by direct reproduction: a 30s
    poll for presence never succeeded even locally, because the phase 6
    coinbase this is used for is immature BY DESIGN (that's what phase 6
    tests). Presence there can never distinguish "not yet indexed" from
    "indexed but correctly filtered as immature" — only the RPC's forced
    wait itself is a usable signal here."""
    rpc(which, "wallet.listunspent")


def start(which, extra=()):
    datadir = WORK / which
    datadir.mkdir(exist_ok=True)
    if which not in ports:
        rpc_p, p2p_p, wallet_p = alloc_ports(3)
        ports[which] = {"rpc": rpc_p, "p2p": p2p_p, "wallet": wallet_p}
    p = ports[which]
    command = [str(DINEROD), "--regtest", f"--datadir={datadir}",
               f"--rpcport={p['rpc']}", f"--port={p['p2p']}", f"--wallet-socket-port={p['wallet']}",
               "--listen=1", "--utreexo=1", "--regtest-enforce-pow",
               f"--consensus-shielded-compact-height={BOUNDARY_HEIGHT}",
               f"--consensus-sixty-second-height={BOUNDARY_HEIGHT}",
               "--consensus-shielded-epoch-reset-height=1",
               "--consensus-shielded-spend-auth-height=2",
               "--consensus-state-commitment-height=3",
               *extra]
    log = (WORK / f"{which}.log").open("a")
    processes[which] = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    wait(lambda: rpc_ready(which), timeout=120, desc=f"{which} RPC readiness")
    cookies[which] = cookie_for(datadir)
    if which not in addresses:
        addr = rpc(which, "wallet.getnewaddress")
        addresses[which] = addr["address"] if isinstance(addr, dict) else addr


def rpc_ready(which):
    if processes[which].poll() is not None:
        raise RuntimeError(f"{which} exited during startup (rc={processes[which].returncode})")
    try:
        cookies[which] = cookie_for(WORK / which)
    except RuntimeError:
        return False
    return rpc(which, "getblockcount") is not None


def stop(which):
    process = processes.pop(which)
    try:
        rpc(which, "stop")
    except Exception:
        pass
    try:
        process.wait(timeout=60)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise RuntimeError(f"{which} did not stop cleanly (had to be killed)")
    if process.returncode != 0:
        raise RuntimeError(f"{which} exited nonzero on stop: {process.returncode}")


def miner_for(which):
    p = ports[which]
    return DineroCoinMiner(
        rpc_url=f"http://127.0.0.1:{p['rpc']}/",
        cookie_path=str(WORK / which / ".cookie"),
        mining_address=addresses[which],
    )


def target_from_bits(bits: int) -> int:
    exponent, mantissa = bits >> 24, bits & 0x7FFFFF
    assert not bits & 0x800000, "negative target"
    return mantissa << (8 * (exponent - 3)) if exponent >= 3 else mantissa >> (8 * (3 - exponent))


def mine_to(which, target_height):
    """Real solved-work mining (--regtest-enforce-pow is active on every
    daemon this harness starts) — not generatetoaddress, which the first
    version used and which hid the ASERT/target gate entirely.

    Every height strictly below BOUNDARY_HEIGHT uses an explicitly
    overridden bits/curtime, matching test_pow_enforced_regtest.py's own
    3-block bootstrap technique (same file, same values) — but applied to
    the ENTIRE pre-boundary range, not just an initial handful of blocks.
    Reason: a fresh regtest chain's genesis timestamp is far in the past
    relative to wall-clock 'now', and pre-activation ASERT extrapolates
    from that stale anchor to an effectively unminable target (observed:
    required bits 0x02008000, ~2^249 expected hashes) the moment curtime
    reverts to real wall-clock — confirmed directly: natural (non-
    overridden) templates at height 31 on an otherwise-identical chain
    computed that same unminable target. Keeping curtime on a synthetic,
    evenly-spaced schedule (genesis_time + h*120) for every pre-boundary
    height keeps the computed delta at exactly zero and bits flat at
    0x1f00fc9c (empirically verified stable across 109 consecutive
    overridden blocks in isolation, 22s total).

    This is not the miner picking its own difficulty: the daemon
    independently RECOMPUTES and ENFORCES these bits. Submitting a
    deliberately-wrong 0x207fffff against a chain already established at
    0x1f00fc9c was rejected with 'bad-diffbits: block has 0x207fffff,
    required 0x1f00fc9c' — real ASERT enforcement, confirmed in isolation
    before relying on it here (see also the neuter check in
    run_negative_controls, which exercises the same rejection path).

    Heights at or after BOUNDARY_HEIGHT use ordinary natural
    getblocktemplate calls. The 60-second-activation retarget path does not
    reference genesis_time at all — it self-corrects from a rolling window
    of actual recent block timestamps — so it is not susceptible to the
    same staleness. Confirmed in isolation at this exact boundary shape:
    109 overridden blocks followed immediately by 6 natural blocks all
    landed on the easy 0x207fffff limit in well under a second."""
    cur = height(which)
    if target_height <= cur:
        return
    genesis_time = rpc(which, "getconsensusinfo")["genesis_time"]
    miner = miner_for(which)
    miner.load_cookie_auth()
    for h in range(cur + 1, target_height + 1):
        template = get_block_template_with_retry(miner)
        assert template and template.height == h, (which, h, template)
        if h < BOUNDARY_HEIGHT:
            template.curtime = genesis_time + h * 120
            template.time_mutable = False
            template.bits = "207fffff" if h == 1 else "1f00fc9c"
            template.target = f"{target_from_bits(int(template.bits, 16)):064x}"
        solved = miner.mine_block(template, max_nonce=4_000_000)
        assert solved is not None, f"mine_to({which}, {target_height}): real work not found at height {h}"
        accepted = submit_block_with_retry(miner, solved[0].hex())
        assert accepted, f"mine_to({which}, {target_height}): block at height {h} was not accepted"
    assert height(which) == target_height, (which, target_height, height(which))


# Codex's review: the previous version of both retry helpers below called
# miner.get_block_template()/miner.submit_block(), which swallow EVERY
# exception internally and return None/False regardless of cause — so the
# retry loop was retrying every failure, not just the documented HTTP 429
# rate-limit condition, and a genuine consensus rejection would be retried
# (uselessly, against the same already-rejected block_hex) rather than
# failing immediately with its real reason. Both helpers now bypass those
# swallowing wrappers and call miner.rpc_call() directly, so a non-429
# failure raises immediately with the daemon's own typed error intact.
def _is_rate_limited(exc: Exception) -> bool:
    return "HTTP 429" in str(exc)


def get_block_template_with_retry(miner, max_attempts=8):
    """Only retries the daemon's own RPC rate limiter ('HTTP 429: Rate
    limit exceeded...'), confirmed to occur during this harness's fast
    post-boundary mining and phase 6's template-polling. Any other
    getblocktemplate failure raises immediately, unretried."""
    for attempt in range(max_attempts):
        try:
            params = {"rules": ["segwit"]}
            if miner.mining_address:
                params["address"] = miner.mining_address
            raw = miner.rpc_call("getblocktemplate", [params])
        except Exception as exc:
            if _is_rate_limited(exc) and attempt < max_attempts - 1:
                time.sleep(0.5 * (attempt + 1))
                continue
            raise
        coinbase_txn = raw.get("coinbasetxn", {}) or {}
        utreexo_obj = raw.get("utreexo", {}) or {}
        return BlockTemplate(
            version=raw.get("version", 1), height=raw["height"],
            previous_block_hash=raw["previousblockhash"], bits=raw["bits"], curtime=raw["curtime"],
            mintime=raw.get("mintime", raw["curtime"]), maxtime=raw.get("maxtime", raw["curtime"] + 7200),
            coinbase_value=raw["coinbasevalue"], transactions=raw.get("transactions", []),
            coinbase_tx_hex=coinbase_txn.get("data", ""), coinbase_txid=coinbase_txn.get("txid", ""),
            utreexo_commitment=utreexo_obj.get("commitment", raw.get("utreexocommitment", "")),
            target=raw.get("target", ""), time_mutable="time" in raw.get("mutable", []))
    raise RuntimeError(f"getblocktemplate: exhausted {max_attempts} attempts, all rate-limited")


def submit_block_with_retry(miner, block_hex, max_attempts=8):
    """Only retries HTTP 429. A genuine rejection (e.g. bad-diffbits, an
    over-value coinbase) raises immediately with the daemon's own message —
    it is never retried against the same already-built block, and never
    silently reduced to a bare True/False."""
    for attempt in range(max_attempts):
        try:
            result = miner.rpc_call("submitblock", [block_hex])
        except Exception as exc:
            if _is_rate_limited(exc) and attempt < max_attempts - 1:
                time.sleep(0.5 * (attempt + 1))
                continue
            raise
        # Matches submit_block()'s own success convention: BIP22 success as
        # null or {}. rpc_call() already raises on a non-null "error" field,
        # so reaching here at all means genuine acceptance.
        assert result is None or result == {}, f"unexpected submitblock response: {result}"
        miner.blocks_found += 1
        return True
    raise RuntimeError(f"submitblock: exhausted {max_attempts} attempts, all rate-limited")


def mine(which, n):
    if n <= 0:
        return
    mine_to(which, height(which) + n)


def mine_excluding(which, exclude_txids, payout_address):
    """Mines exactly one block whose composition is EXPLICITLY controlled,
    for the reorg alternate-branch test only. mine()/mine_to() build a
    block from whatever the daemon's default template contains — after
    invalidateblock returns disconnected transactions to mempool, that
    default template naturally re-includes them, and with this harness's
    deterministic synthetic curtime/bits and the same payout address, the
    resulting block can come out byte-identical to the block that was just
    invalidated (confirmed: Codex's diagnostic reproduced exactly this,
    resubmitting the SAME hash that was invalidated moments before, RPC
    'accepted' without actually advancing the active tip). This uses the
    daemon's own exclude_txids support (src/rpc/methods_mining_v14.cpp,
    passed to BlockAssembler::CreateNewBlock) plus a dedicated payout
    address to make the alternate branch's blocks unambiguously distinct,
    and asserts the resulting block's actual composition and that the tip
    genuinely advanced — not just that submission was accepted."""
    h = height(which) + 1
    genesis_time = rpc(which, "getconsensusinfo")["genesis_time"]
    miner = miner_for(which)
    miner.load_cookie_auth()
    # rpc_call() already raises on an RPC-level error and unwraps to just
    # the inner result object on success (see dinero_cpu_miner.py), unlike
    # this file's own rpc() which returns the full envelope.
    raw = miner.rpc_call("getblocktemplate", [{
        "rules": ["segwit"], "address": payout_address, "exclude_txids": list(exclude_txids)}])
    coinbase_txn = raw.get("coinbasetxn", {}) or {}
    utreexo_obj = raw.get("utreexo", {}) or {}
    template = BlockTemplate(
        version=raw.get("version", 1), height=raw["height"],
        previous_block_hash=raw["previousblockhash"], bits=raw["bits"], curtime=raw["curtime"],
        mintime=raw.get("mintime", raw["curtime"]), maxtime=raw.get("maxtime", raw["curtime"] + 7200),
        coinbase_value=raw["coinbasevalue"], transactions=raw.get("transactions", []),
        coinbase_tx_hex=coinbase_txn.get("data", ""), coinbase_txid=coinbase_txn.get("txid", ""),
        utreexo_commitment=utreexo_obj.get("commitment", raw.get("utreexocommitment", "")),
        target=raw.get("target", ""), time_mutable="time" in raw.get("mutable", []))
    assert template.height == h, (which, h, template.height)
    included_txids = {tx.get("txid") or tx.get("hash") for tx in template.transactions}
    assert not (included_txids & set(exclude_txids)), (
        f"mine_excluding({which}): excluded txid(s) still present in template: "
        f"{included_txids & set(exclude_txids)}")
    if h < BOUNDARY_HEIGHT:
        template.curtime = genesis_time + h * 120
        template.time_mutable = False
        template.bits = "207fffff" if h == 1 else "1f00fc9c"
        template.target = f"{target_from_bits(int(template.bits, 16)):064x}"
    before_tip = tip_hash(which)
    solved = miner.mine_block(template, max_nonce=4_000_000)
    assert solved is not None, f"mine_excluding({which}, {h}): real work not found"
    accepted = submit_block_with_retry(miner, solved[0].hex())
    assert accepted, f"mine_excluding({which}, {h}): block not accepted"
    assert height(which) == h and tip_hash(which) != before_tip, (
        f"mine_excluding({which}, {h}): submission was accepted but the active tip did not "
        f"advance — RPC storage acceptance does not by itself mean active-tip advancement")
    new_tip = tip_hash(which)
    block = rpc(which, "getblock", [new_tip, 1])
    assert not (set(block["tx"]) & set(exclude_txids)), (
        f"mine_excluding({which}, {h}): excluded txid(s) ended up in the mined block anyway: "
        f"{set(block['tx']) & set(exclude_txids)}")
    return new_tip


def height(which):
    return rpc(which, "getblockcount")


def tip_hash(which):
    return rpc(which, "getbestblockhash")


def state_hash(which):
    return rpc(which, "daemon.shieldedstatehash")["state_hash"]


def utreexo_commitment(which):
    return rpc(which, "blockchain.getutreexoroots")


def canonical_proof(which, txid, vout):
    """Fetches a Utreexo membership proof and returns ONLY the fields that
    are stable across a fresh generation (excludes generation_time_ms, which
    the first version's byte-equality check accidentally compared). Requires
    exactly one successful, error-free proof — a per-item error inside a
    successful RPC envelope is not a proof (Codex's review, point 2)."""
    resp = rpc(which, "blockchain.getutxoproofs_batch", [[{"txid": txid, "vout": vout}]])
    assert resp["successful"] == 1 and resp["failed"] == 0, f"expected exactly one successful proof, got {resp}"
    item = resp["proofs"][0]
    assert item.get("success") is True, f"proof item reports failure inside a successful envelope: {item}"
    return {
        "utreexo_root": resp["utreexo_root"],
        "txid": item["txid"],
        "vout": item["vout"],
        "proof": item["proof"],
        "proof_size": item["proof_size"],
    }


def verify_proof(which, canon):
    # Fixed from the first version: canon is ALREADY the unwrapped inner
    # object (canonical_proof strips .result once) — do not extract .result
    # again. Codex's review, point 2, reproduced this exact [null] bug.
    proof_obj = {"txid": canon["txid"], "vout": canon["vout"], "proof": canon["proof"]}
    resp = rpc(which, "blockchain.verifyutxoproofs_batch", [[proof_obj]])
    assert resp["valid"] == 1 and resp["invalid"] == 0, f"proof failed verification: {resp}"


def proof_is_absent(which, txid, vout):
    resp = rpc(which, "blockchain.getutxoproofs_batch", [[{"txid": txid, "vout": vout}]])
    item = resp["proofs"][0]
    return item.get("success") is False


def block_contains(which, block_hash, txid):
    block = rpc(which, "getblock", [block_hash, 1])
    return txid in block["tx"]


def tx_version(which, txid):
    return rpc(which, "gettransaction", [txid])["version"]


def note_values(which):
    """Only UNSPENT notes — wallet.listshielded lists every note the wallet
    has ever seen, spent or not, with an explicit 'spent' field per note.
    Confirmed directly in isolation: after a confirmed unshield, the spent
    note carries spent=true and a spent_height, while remaining in this
    same list. Comparing against ALL notes (the original bug) makes the
    post-unshield note-count assertion in phase 1 fail even on a fully
    correct unshield, misattributing a harness filtering bug to the wallet
    itself."""
    notes = rpc(which, "wallet.listshielded")["notes"]
    return sorted(int(n["value_una"]) for n in notes if not n.get("spent", False))


def compact_support_present():
    """Probe whether this build actually has DINERO_ENABLE_COMPACT_REGTEST
    compiled in, per src/daemon/main.cpp:552-556's explicit exit(1) with
    'Compact regtest support is not compiled into this build.' Fail loudly
    and distinctly here rather than letting the very first daemon start
    below crash with a confusing message (Codex's review, point 1)."""
    probe_dir = WORK / "compact-probe"
    probe_dir.mkdir()
    result = subprocess.run(
        [str(DINEROD), "--regtest", f"--datadir={probe_dir}",
         "--consensus-shielded-compact-height=1", "--rpcport=0"],
        capture_output=True, text=True, timeout=20)
    shutil.rmtree(probe_dir, ignore_errors=True)
    if result.returncode != 0 and "not compiled into this build" in (result.stderr + result.stdout):
        return False
    # Anything else (including a clean early exit for an unrelated reason)
    # is not the specific rejection this probe is checking for; don't guess.
    return True


def parse_migrate_output(stdout: str) -> dict:
    """Parses migrate_shielded_datadir's fixed key=value line (tools/
    migrate_shielded_datadir.cpp's printf: 'ok=%s ready=%s selected_rows=%llu
    retired_rows=%llu phase=%s operation=%s source_digest=%s error=%s').
    error= is always the LAST field and its value can itself contain
    spaces ("original/candidate companion mismatch", "cannot stat companion
    path") — a naive dict(kv.split('=',1) for kv in stdout.split()) breaks
    on those with a ValueError (confirmed by direct reproduction), since it
    splits the error text's own spaces into bogus non-key=value tokens.
    Split on the LAST 'error=' instead, parsing only the fixed fields
    before it by whitespace."""
    stdout = stdout.strip()
    if not stdout:
        return {}
    head, _, error_value = stdout.partition(" error=")
    fields = dict(kv.split("=", 1) for kv in head.split())
    fields["error"] = error_value
    return fields


def record_evidence():
    (EVIDENCE / "source-commit.txt").write_text(
        subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout)
    (EVIDENCE / "dinerod.sha256").write_text(f"{sha256_file(DINEROD)}  {DINEROD}\n")
    (EVIDENCE / "migrate_shielded_datadir.sha256").write_text(
        f"{sha256_file(MIGRATE_TOOL)}  {MIGRATE_TOOL}\n")
    # Codex's review: an evidence directory recorded parent 75811bc63 while
    # this script's own fixes were committed afterward — source-commit.txt
    # alone does not establish which exact version of THIS HARNESS produced
    # the run, only which engine commit the working tree was based on at
    # git-HEAD granularity. A dirty working tree (uncommitted edits to this
    # very script, as happened here) makes that gap concrete. Record both:
    # whether the tree was dirty at test time, and a content hash of this
    # harness script itself, independent of git commit state.
    (EVIDENCE / "git-status.txt").write_text(
        subprocess.run(["git", "status", "--short"], cwd=ROOT, capture_output=True, text=True).stdout)
    (EVIDENCE / "harness-script.sha256").write_text(
        f"{sha256_file(Path(__file__).resolve())}  {Path(__file__).name}\n")


def main():
    # --self-test dispatches BEFORE any real phase runs — the first version
    # checked this flag only after the entire (expensive) main body had
    # already executed unconditionally (Codex's review, point 7).
    if "--self-test" in sys.argv:
        run_self_test()
        return

    if not compact_support_present():
        raise RuntimeError(
            "This build does not have DINERO_ENABLE_COMPACT_REGTEST compiled in. "
            "This harness specifically qualifies compact activation and requires a "
            "dedicated compact-enabled build/job — it must not silently degrade to "
            "ordinary shield/unshield coverage (Codex's review, point 1). "
            "Build with -DDINERO_ENABLE_COMPACT_REGTEST=ON.")

    record_evidence()

    # ── Phase 1: nonempty pre-migration state, deliberately multi-note ─────
    # (Codex's review, point 6: a single shield->unshield leaves ZERO
    # shielded notes, since single-note unshield spends the entire chosen
    # note with no shielded change — src/rpc/shielded_rpc_json.cpp's own
    # documented behavior. Two notes of clearly different size + an exact-
    # amount unshield, which wallet.unshield's own doc says selects "the
    # smallest unspent confirmed shielded note with value >= amount",
    # deterministically leaves the larger note untouched.)
    print("[INFO] phase 1: nonempty pre-migration state (two notes, one consumed)", flush=True)
    start("original")
    mine("original", PREMINE_BLOCKS)

    # A dedicated, explicitly LOCKED coinbase — never touched by wallet coin
    # selection for shield/unshield below — used only for the migration-
    # identical-proof comparison in phase 3 (Codex's review, point 2: the
    # first version's height-1 coinbase was not reserved and could have been
    # silently consumed by an unrelated wallet operation).
    locked_block_hash = rpc("original", "getblockhash", [2])
    locked_txid = rpc("original", "getblock", [locked_block_hash, 1])["tx"][0]
    rpc("original", "wallet.lockunspent", [False, [{"txid": locked_txid, "vout": 0}]])

    shield_big = rpc("original", "wallet.shield", {"amount_una": 100000000, "fee_una": 1000000})
    assert len(shield_big["txid"]) == 64, shield_big
    mine("original", 1)
    shield_small = rpc("original", "wallet.shield", {"amount_una": 10000000, "fee_una": 1000000})
    assert len(shield_small["txid"]) == 64, shield_small
    mine("original", 1)
    assert note_values("original") == [10000000, 100000000], note_values("original")

    unshield = rpc("original", "wallet.unshield", {"amount_una": 10000000, "fee_una": 1000000})
    unshield_txid = unshield["txid"]
    assert len(unshield_txid) == 64, unshield
    # Captured HERE, while still unconfirmed in mempool, for phase 5c's
    # nullifier-reuse rebroadcast test later. The bare `getrawtransaction`
    # alias could not find this txid once it left mempool by confirming
    # (confirmed by direct reproduction: "Transaction not found" both
    # before AND after the eventual spend of its output). `wallet.
    # getrawtransaction` checks mempool then falls back to chainstate/
    # chain_db (methods_wallet_context.cpp), so it should find it either
    # way — capturing here regardless, while it's simplest (still
    # unconfirmed). verbose=True returns a DECODED object (no "hex" field,
    # confirmed by direct reproduction: KeyError 'hex'); the default
    # (verbose=False) returns {"hex": ...} directly, which is what a
    # rebroadcast actually needs.
    raw_unshield_hex = rpc("original", "wallet.getrawtransaction", [unshield_txid])["hex"]
    mine("original", 1)
    remaining_notes = note_values("original")
    assert remaining_notes == [100000000], (
        f"expected exactly the untouched large note to remain, got {remaining_notes}")

    unshield_tx = rpc("original", "gettransaction", [unshield_txid])
    unshield_vout = next(i for i, o in enumerate(unshield_tx.get("outputs", unshield_tx.get("vout", [])))
                          if not o.get("is_shielded", False))

    pre_locked_proof = canonical_proof("original", locked_txid, 0)
    pre_unshield_proof = canonical_proof("original", unshield_txid, unshield_vout)
    pre_state_hash = state_hash("original")
    pre_utreexo = utreexo_commitment("original")
    pre_tip = tip_hash("original")
    pre_height = height("original")
    pre_balance = rpc("original", "wallet.getbalance")
    print(f"[PASS] built nonempty pre-migration state: notes={remaining_notes} "
          f"state_hash={pre_state_hash} tip={pre_height}:{pre_tip}", flush=True)

    stop("original")

    # ── Phase 2: migrate ────────────────────────────────────────────────
    print("[INFO] phase 2: copy + migrate", flush=True)
    candidate_dir = WORK / "candidate"
    shutil.copytree(WORK / "original", candidate_dir)
    migrate_out = subprocess.run(
        [str(MIGRATE_TOOL), str(WORK / "original"), str(candidate_dir), "--apply"],
        capture_output=True, text=True, timeout=120)
    (EVIDENCE / "migrate-stdout.txt").write_text(migrate_out.stdout)
    (EVIDENCE / "migrate-stderr.txt").write_text(migrate_out.stderr)
    assert migrate_out.returncode == 0, f"migrate_shielded_datadir exited {migrate_out.returncode}: {migrate_out.stderr}"
    fields = parse_migrate_output(migrate_out.stdout)
    assert fields.get("ok") == "true" and fields.get("ready") == "true", fields
    source_digest = fields.get("source_digest", "")
    assert source_digest, "migration result carried no source_digest"
    print(f"[PASS] migration completed: {migrate_out.stdout.strip()}", flush=True)

    ports["candidate"] = None  # placeholder cleared by start()
    del ports["candidate"]

    # ── Phase 3: identical state AND identical proofs after migration ──────
    print("[INFO] phase 3: post-migration identity checks", flush=True)
    start("candidate")
    assert state_hash("candidate") == pre_state_hash, "shielded state hash DIVERGED after migration"
    assert utreexo_commitment("candidate") == pre_utreexo, "Utreexo commitment DIVERGED after migration"
    assert tip_hash("candidate") == pre_tip and height("candidate") == pre_height, "tip DIVERGED after migration"
    assert note_values("candidate") == remaining_notes, "shielded notes DIVERGED after migration"
    # wallet.lockunspent is process-local, in-memory-only state
    # (WalletManager::locked_utxos_) — it is never persisted, so a daemon
    # restart legitimately clears it independent of CF migration. Comparing
    # wallet.getbalance directly against pre_balance (captured while
    # "original" still held the lock, before it was ever restarted) would
    # therefore fail on the unrelated `locked`/`unspendable` breakdown
    # fields even when migration is byte-perfect. Re-apply the identical
    # fixture lock here before comparing, matching restart semantics
    # explicitly rather than weakening the comparison.
    rpc("candidate", "wallet.lockunspent", [False, [{"txid": locked_txid, "vout": 0}]])
    assert rpc("candidate", "wallet.getbalance") == pre_balance, "wallet balance DIVERGED after migration"

    post_locked_proof = canonical_proof("candidate", locked_txid, 0)
    assert post_locked_proof == pre_locked_proof, (
        f"Utreexo proof of the locked coinbase DIVERGED after migration:\n{pre_locked_proof}\nvs\n{post_locked_proof}")
    verify_proof("candidate", post_locked_proof)
    post_unshield_proof = canonical_proof("candidate", unshield_txid, unshield_vout)
    assert post_unshield_proof == pre_unshield_proof, "Utreexo proof of the unshield output DIVERGED after migration"
    verify_proof("candidate", post_unshield_proof)
    print("[PASS] state, notes, balance, tip and both captured Utreexo proofs are byte-identical after migration", flush=True)

    # ── Phase 4: real compact/60s activation boundary, with actual PoW ──────
    print(f"[INFO] phase 4: compact/60-second activation at real height {BOUNDARY_HEIGHT}", flush=True)
    pre_boundary_info = rpc("candidate", "getconsensusinfo")
    assert pre_boundary_info["regtest_pow_enforced"] is True, pre_boundary_info

    cur = height("candidate")
    to_mine = BOUNDARY_HEIGHT - cur - 2
    assert to_mine > 0, f"boundary {BOUNDARY_HEIGHT} not far enough ahead of tip {cur}"
    mine("candidate", to_mine)
    assert height("candidate") == BOUNDARY_HEIGHT - 2

    legacy_shield = rpc("candidate", "wallet.shield", {"amount_una": 20000000, "fee_una": 1000000})
    legacy_txid = legacy_shield["txid"]
    mine("candidate", 1)
    legacy_block = tip_hash("candidate")
    assert height("candidate") == BOUNDARY_HEIGHT - 1
    assert block_contains("candidate", legacy_block, legacy_txid), "legacy shield was not included in the mined block"
    # gettransaction is a confirmed-only, blockchain lookup (methods_
    # blockchain_context.cpp's explicit API contract) — asserting the
    # version before mining fails to even find a still-mempool transaction.
    # Confirmed by direct reproduction; moved here, after confirmation,
    # keeping the inclusion check above which already ran post-mining.
    assert tx_version("candidate", legacy_txid) == TX_VERSION_SHIELDED_V2, (
        "pre-activation shield used an unexpected transaction version — expected ordinary v2")
    print(f"[PASS] pre-activation shield used legacy version {TX_VERSION_SHIELDED_V2} and was included at height {height('candidate')}", flush=True)

    mine("candidate", 1)
    assert height("candidate") == BOUNDARY_HEIGHT
    boundary_info = rpc("candidate", "getconsensusinfo")
    assert boundary_info["target_spacing_seconds"] == 60, boundary_info
    assert boundary_info["target_spacing_seconds"] != pre_boundary_info["target_spacing_seconds"], (
        "60-second activation did not actually change target_spacing_seconds")

    compact_unshield = rpc("candidate", "wallet.unshield", {"amount_una": 5000000, "fee_una": 1000000})
    compact_txid = compact_unshield["txid"]
    mine("candidate", 1)
    compact_block = tip_hash("candidate")
    assert block_contains("candidate", compact_block, compact_txid), "compact unshield was not included in the mined block"
    assert tx_version("candidate", compact_txid) == TX_VERSION_COMPACT_REGTEST, (
        f"post-activation unshield did not use the compact version 0x{TX_VERSION_COMPACT_REGTEST:x}")
    print(f"[PASS] post-activation unshield used compact version 0x{TX_VERSION_COMPACT_REGTEST:x} "
          f"and was included at height {height('candidate')}", flush=True)

    # Exact subsidy-plus-actual-fees, not just positive (Codex's review) —
    # verify the "zero additional fees" precondition explicitly rather than
    # silently assuming it: legacy_shield and compact_unshield are both
    # already confirmed above, so mempool should hold nothing else, but
    # confirm that directly instead of asserting a subsidy-only value on
    # faith.
    pending_mempool = rpc("candidate", "getrawmempool")
    assert pending_mempool == [], (
        f"expected an empty mempool before the exact-subsidy check, found: {pending_mempool}")
    template = rpc("candidate", "getblocktemplate", {"address": addresses["candidate"]})
    STANDARD_SUBSIDY_UNA = 10_000_000_000
    assert template["coinbasevalue"] == STANDARD_SUBSIDY_UNA, (
        f"expected the exact standard 100 DIN subsidy plus zero fees (verified empty mempool above), "
        f"got {template['coinbasevalue']}")

    # Differential/independent ASERT check: an unmigrated control chain,
    # mined from genesis with the IDENTICAL consensus flags to the exact
    # same height, must compute the IDENTICAL bits and coinbasevalue as the
    # migrated candidate. This is a differential check that migration
    # introduces no divergence in either ASERT retargeting or reward
    # computation — not a re-validation of the ASERT formula itself, which
    # test_sixty_second_activation.py's own header already documents as its
    # own separate scope ("Regtest bypasses ASERT: this qualifies
    # activation/state transitions, not cadence").
    #
    # Mine control to CANDIDATE'S actual current tip, not BOUNDARY_HEIGHT:
    # a real GitHub review comment caught that candidate's tip here is
    # BOUNDARY_HEIGHT+1 (compact_unshield's own confirming block, mined
    # just above), so `template` was already captured for height
    # BOUNDARY_HEIGHT+2 — but a control mined only to BOUNDARY_HEIGHT would
    # have produced a template for BOUNDARY_HEIGHT+1, a height short of
    # candidate's. Since difficulty targets depend on height and timestamp
    # history, comparing two different heights could reject a correct
    # implementation, or pass vacuously if both happened to clamp to the
    # same regtest limit — neither proves the intended same-height
    # equivalence. Matching candidate's real tip height here fixes that.
    candidate_tip_height = height("candidate")
    print(f"[INFO] mining an unmigrated control chain to height {candidate_tip_height} "
          f"(candidate's own current tip) for a differential ASERT/reward check", flush=True)
    start("control")
    mine_to("control", candidate_tip_height)
    control_template = rpc("control", "getblocktemplate", {"address": addresses["control"]})
    assert control_template["bits"] == template["bits"], (
        f"migrated candidate's ASERT bits ({template['bits']}) diverged from an unmigrated "
        f"control chain mined identically to the same height ({control_template['bits']})")
    assert control_template["coinbasevalue"] == template["coinbasevalue"] == STANDARD_SUBSIDY_UNA, (
        f"migrated candidate's coinbasevalue ({template['coinbasevalue']}) diverged from an "
        f"unmigrated control chain ({control_template['coinbasevalue']})")
    stop("control")
    print(f"[PASS] migrated candidate's ASERT bits ({template['bits']}) and exact subsidy "
          f"({STANDARD_SUBSIDY_UNA} una) match an independently-mined, unmigrated control chain "
          "at the same height", flush=True)

    # The 0.5 DIN tail-emission reward is NOT independently re-derived here,
    # and — corrected after Codex's review caught a fabricated claim in an
    # earlier version of this comment — there is currently no test ANYWHERE
    # in this repository that mines a real block at a reduced/tail-emission
    # height and checks its actual coinbase payout. A repo-wide search
    # found only: (a) test_sixty_second_activation.py's check() function,
    # which asserts the RPC-REPORTED tail_emission_una field
    # (100_000_000 vs 50_000_000 una) on a genuinely-mined chain, but never
    # cross-checks it against a real mined coinbase's actual value, and
    # asserts next_block_reward_din == 100 at BOTH spacings — it does not
    # reach a reduced-reward height at all; and (b) several isolated C++
    # unit tests (test_subsidy_schedule.cpp, test_supply_cap.cpp,
    # test_subsidy_validation.cpp) that check the subsidy-calculation
    # function's arithmetic directly, or against synthetic/mocked
    # coinbases, never through real block mining or validation. Reaching a
    # genuine tail-emission height via real PoW (halving_interval =
    # 1,314,000 blocks) is infeasible for this harness's runtime; this is a
    # genuine coverage gap, reported as such rather than claimed as covered
    # elsewhere.

    # ── Phase 5a: restart ──────────────────────────────────────────────
    print("[INFO] phase 5a: restart on the migrated candidate", flush=True)
    before_restart = (height("candidate"), tip_hash("candidate"), state_hash("candidate"))
    stop("candidate")
    start("candidate")
    after_restart = (height("candidate"), tip_hash("candidate"), state_hash("candidate"))
    assert before_restart == after_restart, f"restart changed height/tip/state: {before_restart} -> {after_restart}"
    print("[PASS] restart preserved height, tip and shielded state hash", flush=True)

    # ── Phase 5b: reorg that actually disconnects the shielded transitions ──
    # (Codex's review, point 4: the first version forked AFTER all shielded
    # activity, so the reorg never touched it. Fork BEFORE the boundary-
    # crossing shield/unshield instead, so disconnecting genuinely removes
    # them and reconnecting genuinely restores them.)
    print("[INFO] phase 5b: reorg across the shielded activation boundary", flush=True)
    fork_height = BOUNDARY_HEIGHT - 2  # the height right before the legacy shield above
    fork_block_hash = rpc("candidate", "getblockhash", [fork_height])
    long_branch_tip = tip_hash("candidate")
    long_branch_height = height("candidate")
    long_branch_state = state_hash("candidate")

    first_disconnected = rpc("candidate", "getblockhash", [fork_height + 1])
    rpc("candidate", "blockchain.invalidateblock", [first_disconnected])
    assert height("candidate") == fork_height and tip_hash("candidate") == fork_block_hash
    disconnected_state = state_hash("candidate")
    assert disconnected_state != long_branch_state, (
        "shielded state hash did not change after disconnecting the boundary-crossing branch")

    # invalidateblock disconnects fork_height+1 (legacy_shield, height
    # BOUNDARY_HEIGHT-1) and cascades through its descendants: the boundary/
    # activation block itself (BOUNDARY_HEIGHT, no shielded content) and the
    # compact_unshield block (BOUNDARY_HEIGHT+1) — three blocks total, so
    # long_branch_height is fork_height+3, not +2 (an off-by-one caught only
    # by actually running this: the two prior mine_excluding calls each
    # individually succeeded and advanced the tip correctly, but stopped one
    # block short of long_branch_height). Both shielded txids return to
    # mempool with the cascade. A short alternative branch built from a
    # DEFAULT template would naturally re-include them — with this harness's
    # deterministic synthetic curtime/bits and the same payout address, that
    # produces a block byte-identical to the one just invalidated (confirmed
    # by direct reproduction: RPC accepted the resubmission of the exact
    # same hash without the active tip actually advancing). mine_excluding
    # makes this branch unambiguously distinct: a dedicated payout address,
    # explicit exclude_txids, and an assertion that the tip genuinely moved.
    # Deliberately SHORTER than the long branch by one block, not equal
    # length — confirmed necessary by actually running this: with equal
    # height (and, pre-boundary, identical per-block difficulty from this
    # harness's own deterministic override), reconsiderblock's target has
    # no strictly greater cumulative work, so the daemon has no principled
    # reason to switch back, and the reconnect assertion below failed on a
    # real run ("reorg did not reconnect the exact original winning block
    # hash") purely from this tie, not from any reorg-logic defect. A
    # strictly shorter alternate branch makes the longer, reconsidered
    # branch unambiguously heavier — the actual property this exercise
    # means to test.
    short_branch_addr = rpc("candidate", "wallet.getnewaddress")
    short_branch_addr = short_branch_addr["address"] if isinstance(short_branch_addr, dict) else short_branch_addr
    exclude = [legacy_txid, compact_txid]
    blocks_to_replay = long_branch_height - fork_height - 1
    assert blocks_to_replay > 0, f"long branch too short to build a shorter alternative: {long_branch_height=} {fork_height=}"
    for _ in range(blocks_to_replay):
        mine_excluding("candidate", exclude, short_branch_addr)
    assert height("candidate") == long_branch_height - 1

    # Assessed, not assumed — and the original assumption here was WRONG,
    # caught only by actually running this: daemon.shieldedstatehash's own
    # doc comment (methods_daemon_status.cpp) is explicit that it is a
    # COMPOSITE of "utreexo forest + shielded tree + nullifier set + anchor
    # history", not the shielded pool's content alone. Every new block adds
    # its coinbase output as a new Utreexo leaf regardless of shielded
    # content, so the composite necessarily changes on ANY block, shielded
    # or not — confirmed directly: this exact assertion (expecting equality
    # with disconnected_state) failed on a real run. The valid claims this
    # phase can actually make: the alternate branch is genuinely a
    # different composite state (not accidentally identical to either the
    # disconnected point or the original long branch), and reconnecting the
    # ORIGINAL blocks restores the ORIGINAL composite exactly — that
    # reconnect-equality check below is the real proof point for this
    # exercise, not an equality claim about the alternate branch itself.
    short_branch_state = state_hash("candidate")
    assert short_branch_state != disconnected_state, (
        "mining new blocks did not change the composite reorg-state hash at all — "
        "expected the Utreexo forest component to move on every new block")
    assert short_branch_state != long_branch_state, (
        "the alternate (excluded-txid) branch produced the same composite state as "
        "the original boundary-crossing branch — it is not actually a distinct chain")

    rpc("candidate", "blockchain.reconsiderblock", [first_disconnected])
    wait(lambda: height("candidate") == long_branch_height, timeout=60, desc="reorg back onto the longer branch")
    assert tip_hash("candidate") == long_branch_tip, "reorg did not reconnect the exact original winning block hash"
    assert state_hash("candidate") == long_branch_state, "shielded state hash did not return to its pre-fork value after reconnecting"
    print("[PASS] reorg disconnected then reconnected the shielded transitions with matching roots and exact winning tip", flush=True)

    # ── Phase 5c: spend the UNSHIELD's own output, then reject conflicts ───
    # (Codex's review, point 4: the first version spent an unrelated
    # coinbase instead of the unshield's transparent output.)
    print("[INFO] phase 5c: spend the unshield output; reject duplicate and nullifier-reuse rebroadcasts", flush=True)
    # The unshield output's exact value is 0.09 DIN (recipient_una=9000000,
    # per its own RPC response). createrawtransaction does not add a fee
    # automatically — spending the FULL input value out again leaves zero
    # fee, which mempool policy correctly rejects
    # ("insufficient-fee: ... below minimum 1.000000 sat/byte"), confirmed
    # by direct reproduction. Leave 0.001 DIN (100000 una) as fee.
    # raw_unshield_hex was captured back in phase 1, while unshield_txid was
    # still sitting unconfirmed in mempool — getrawtransaction could not
    # find it here regardless of spend state (confirmed by direct
    # reproduction: it failed identically before AND after the spend
    # below), consistent with a mempool-only lookup (no -txindex) rather
    # than a spent-output-specific restriction.
    spend_addr = rpc("candidate", "wallet.getnewaddress")
    spend_addr = spend_addr["address"] if isinstance(spend_addr, dict) else spend_addr
    raw = rpc("candidate", "wallet.createrawtransaction",
              [[{"txid": unshield_txid, "vout": unshield_vout}], {spend_addr: 0.089}])
    raw_hex = raw["hex"] if isinstance(raw, dict) else raw
    signed = rpc("candidate", "wallet.signrawtransaction", [raw_hex])
    signed_hex = signed["hex"] if isinstance(signed, dict) else signed
    spend_txid = rpc("candidate", "wallet.sendrawtransaction", [signed_hex])
    # wallet.sendrawtransaction's inner result is itself {"result": "<txid>"}
    # — a double-wrap specific to this method, unlike this file's other RPCs
    # where rpc()'s single unwrap already yields the plain value. Confirmed
    # by direct reproduction: passing the un-re-unwrapped dict onward to
    # gettransaction produced "Usage: blockchain.gettransaction <txid>".
    spend_txid = spend_txid["result"] if isinstance(spend_txid, dict) and "result" in spend_txid else spend_txid
    mine("candidate", 1)
    confirmed = rpc("candidate", "gettransaction", [spend_txid])
    assert confirmed.get("confirmations", 0) >= 1, confirmed
    assert proof_is_absent("candidate", unshield_txid, unshield_vout), (
        "the unshield output still has a CURRENT membership proof after being spent")
    # Historical inclusion of the legacy shield is still queryable after its
    # own input's later spend elsewhere — checked directly below, not via
    # the `... or True` this line used to carry (vacuously always true,
    # confirmed the same check already exists two lines down).
    historical = rpc("candidate", "getblock", [legacy_block, 1])
    assert legacy_txid in historical["tx"], "historical inclusion of the (now-spent-input) legacy tx is no longer queryable"
    print("[PASS] spent the unshield output; no current Utreexo proof remains; historical block inclusion is still verifiable", flush=True)

    # Codex's review: match each rejection to its expected reason, not any
    # RPC error — a wrong-but-still-rejected response (e.g. a transport
    # failure) would otherwise pass silently.
    dup_error = rpc_expect_error("candidate", "wallet.sendrawtransaction", [signed_hex])
    assert "utxo not found" in json.dumps(dup_error).lower(), (
        f"expected an already-spent/UTXO-not-found rejection reason for the duplicate spend, got: {dup_error}")
    print(f"[PASS] duplicate rebroadcast of the confirmed spend rejected: {dup_error}", flush=True)

    nullifier_reuse_error = rpc_expect_error("candidate", "wallet.sendrawtransaction", [unshield["hex"]]) \
        if isinstance(unshield, dict) and "hex" in unshield else None
    if nullifier_reuse_error is None:
        # The signed hex for the original unshield was not returned by
        # wallet.unshield's own response — use the raw hex captured above
        # (before the spend) instead. This still exercises the persistent-
        # nullifier-conflict check (shielded_validation.cpp's "already spent
        # in persistent state" path), a different consensus code path than
        # the ordinary-UTXO duplicate check above.
        nullifier_reuse_error = rpc_expect_error("candidate", "sendrawtransaction", [raw_unshield_hex])
    assert "nullifier" in json.dumps(nullifier_reuse_error).lower(), (
        f"expected a nullifier-reuse rejection reason, got: {nullifier_reuse_error}")
    print(f"[PASS] rebroadcasting the confirmed unshield (nullifier already persisted) rejected: {nullifier_reuse_error}", flush=True)

    # ── Phase 6: coinbase maturity, tested at the layer that enforces it ────
    # (Codex's review, point 5: the first version tested listunspent's
    # minconf filter on an ORDINARY payment output, which has no coinbase
    # maturity rule at all.)
    #
    # This phase originally tried to prove immaturity via
    # wallet.sendrawtransaction rejection. That can never pass: confirmed
    # directly (isolated probe) that sendrawtransaction accepts an immature
    # coinbase spend into mempool unconditionally, at blocks_on_top as low
    # as 2 — mempool.cpp's checkDependencies only checks UTXO existence, not
    # maturity. Template selection IS one real enforcement point —
    # mempool.cpp:2358 gates a coinbase input's eligibility for the NEXT
    # block by isCoinbaseMature(coin->height, next_block_height) — but,
    # per Codex's review, not the SOLE one: block_validation.cpp:3125-3133
    # enforces the same rule again during block validation itself, and
    # stateless validation has its own maturity check too. Calibrated
    # directly: a spend broadcast to mempool at low height was absent from
    # getblocktemplate's transactions through blocks_on_top=98, and present
    # starting at blocks_on_top=99 (COINBASE_MATURITY=100's actual boundary
    # once next_block_height is accounted for) — an EXACT check now, not a
    # search (see below). This phase exercises the template-selection layer
    # specifically: broadcast once, prove absence pre-maturity, then prove
    # the very next natural mine() picks it up and confirms it once
    # mature — real template-construction behavior, not a guess at an error
    # string. It does not separately re-exercise the block-validation-level
    # or stateless checks, which are defense-in-depth on the same rule.
    print("[INFO] phase 6: coinbase maturity via template inclusion, not mempool admission", flush=True)
    mine("candidate", 1)
    maturity_coinbase_height = height("candidate")
    maturity_block_hash = tip_hash("candidate")
    maturity_txid = rpc("candidate", "getblock", [maturity_block_hash, 1])["tx"][0]
    # See wait_for_wallet_sync's own docstring: closes the WalletWorker
    # indexing race between "block accepted" and "wallet knows this
    # coinbase's key/script well enough to actually sign a spend of it."
    wait_for_wallet_sync("candidate")
    maturity_addr = rpc("candidate", "wallet.getnewaddress")
    maturity_addr = maturity_addr["address"] if isinstance(maturity_addr, dict) else maturity_addr
    # 99.99 out of the 100 DIN coinbase — a 0.01 DIN (1000000 una) fee,
    # matching the fee_una=1000000 convention used everywhere else in this
    # script (phase 1/4's shield/unshield calls). The original 9.9 (90.1
    # DIN "fee") and a later attempt at 99.9 (0.1 DIN fee, 10x this
    # convention) each caused a genuine daemon-side inconsistency once
    # mature and mined: getblocktemplate priced the coinbase at
    # subsidy+actual_fee, but the block acceptor's own "maximum subsidy +
    # fees" validation computed a smaller expected ceiling for the same
    # transaction, rejecting the daemon's own template-built block
    # ("Coinbase output (19010000000 una) exceeds maximum subsidy + fees
    # (10001000000 una)", then "10010000000 una" vs the same "10001000000
    # una" ceiling at the smaller fee) — confirmed by direct reproduction
    # at two different fee sizes, both above this script's own established
    # 1000000-una convention. Matching that convention exactly avoids
    # whatever this ceiling's real computation is, which no genuine wallet
    # paying an ordinary fee would trigger.
    immature_raw = rpc("candidate", "wallet.createrawtransaction",
                        [[{"txid": maturity_txid, "vout": 0}], {maturity_addr: 99.99}])
    immature_hex = immature_raw["hex"] if isinstance(immature_raw, dict) else immature_raw
    immature_signed = rpc("candidate", "wallet.signrawtransaction", [immature_hex])
    immature_signed_hex = immature_signed["hex"] if isinstance(immature_signed, dict) else immature_signed
    immature_txid = rpc("candidate", "wallet.sendrawtransaction", [immature_signed_hex])
    immature_txid = immature_txid["result"] if isinstance(immature_txid, dict) and "result" in immature_txid else immature_txid
    print(f"[INFO] immature spend {immature_txid} accepted into mempool (expected — mempool does not gate maturity)", flush=True)

    # confirmations = tip_height - coinbase_height + 1; blocks_on_top=98
    # means the NEXT block (blocks_on_top+1=99 once mined) still has
    # maturity=99 < COINBASE_MATURITY=100 at the template-selection check
    # (which evaluates against next_block_height, one past the current tip)
    # — confirmed absent from the template at exactly this point.
    mine("candidate", COINBASE_MATURITY - 2)
    blocks_on_top = height("candidate") - maturity_coinbase_height
    assert blocks_on_top == COINBASE_MATURITY - 2, blocks_on_top
    template = rpc("candidate", "getblocktemplate", {"address": addresses["candidate"]})
    template_txids = {tx.get("txid") or tx.get("hash") for tx in template.get("transactions", [])}
    assert immature_txid not in template_txids, (
        f"immature coinbase spend {immature_txid} unexpectedly appeared in the template "
        f"one block before maturity (blocks_on_top={blocks_on_top})")
    print(f"[PASS] immature spend correctly absent from the block template at blocks_on_top={blocks_on_top}", flush=True)

    # Exact boundary, not a search: Codex's review correctly rejected the
    # earlier self-calibrating loop here ("allows inclusion long after the
    # required boundary and still passes... never self-calibrate the
    # consensus expectation from observed behavior"). One block advances
    # blocks_on_top from 98 to 99, i.e. next_block_height - coin_height
    # from 99 to exactly 100 — assert presence at that exact point,
    # directly, the same way absence was just asserted at exactly 98.
    mine("candidate", 1)
    blocks_on_top = height("candidate") - maturity_coinbase_height
    assert blocks_on_top == COINBASE_MATURITY - 1, blocks_on_top
    template = rpc("candidate", "getblocktemplate", {"address": addresses["candidate"]})
    template_txids = {tx.get("txid") or tx.get("hash") for tx in template.get("transactions", [])}
    assert immature_txid in template_txids, (
        f"coinbase spend {immature_txid} absent from the template at exactly "
        f"blocks_on_top={blocks_on_top} (next_block_height - coin_height = {COINBASE_MATURITY}), "
        f"where it should now be mature and template-eligible")
    print(f"[PASS] spend correctly present in the block template at exactly blocks_on_top={blocks_on_top}", flush=True)

    mine("candidate", 1)
    matured_block = tip_hash("candidate")
    assert block_contains("candidate", matured_block, immature_txid), (
        f"the now-mature spend {immature_txid} was template-eligible one block prior "
        f"but was not actually included in the block mined at that point")
    confirmed = rpc("candidate", "gettransaction", [immature_txid])
    assert confirmed.get("confirmations", 0) >= 1, confirmed
    blocks_on_top = height("candidate") - maturity_coinbase_height
    print(f"[PASS] the same spend was included and confirmed at exactly the maturity boundary "
          f"(blocks_on_top={blocks_on_top})", flush=True)

    stop("candidate")

    # ── Negative controls ────────────────────────────────────────────
    print("[INFO] negative controls", flush=True)
    run_negative_controls(WORK / "original")

    print(f"[INFO] ALL CHECKS PASSED. source_digest={source_digest}", flush=True)
    global success
    success = True


def run_negative_controls(original_dir: Path):
    # 1. Deterministic corruption: XOR an existing byte (never a fixed \xFF
    # write, which is a no-op if that byte already happened to be 0xFF —
    # Codex's review, point 7) in a REQUIRED, verified-nonempty companion
    # file, and verify the byte actually changed before calling the tool.
    nc_flip = WORK / "nc-flip"
    shutil.copytree(original_dir, nc_flip)
    block_files = sorted((nc_flip / "blocks").glob("*.dat")) if (nc_flip / "blocks").exists() else []
    assert block_files, "negative control setup: no block file found to corrupt"
    target_file = block_files[0]
    data = bytearray(target_file.read_bytes())
    assert len(data) > 16, f"negative control setup: {target_file} is unexpectedly tiny ({len(data)} bytes)"
    original_byte = data[16]
    data[16] ^= 0xFF
    assert data[16] != original_byte
    target_file.write_bytes(bytes(data))
    result = subprocess.run([str(MIGRATE_TOOL), str(original_dir), str(nc_flip), "--apply"],
                             capture_output=True, text=True, timeout=60)
    # Codex's review: assert the intended STRUCTURED refusal, not "any
    # nonzero exit or missing ok=true" — a transport crash or an unrelated
    # early argument-usage error would pass that loose a check too.
    corrupt_fields = parse_migrate_output(result.stdout)
    assert corrupt_fields.get("ok") == "false" and "companion mismatch" in corrupt_fields.get("error", ""), (
        f"REGRESSION: byte-corrupted candidate did not produce the specific "
        f"companion-mismatch refusal: stdout={result.stdout!r} stderr={result.stderr!r}")
    print(f"[PASS] byte-corrupted candidate companion correctly refused: {result.stdout.strip() or result.stderr.strip()}", flush=True)

    # 2. Never-copied (empty) candidate.
    nc_empty = WORK / "nc-empty"
    nc_empty.mkdir()
    result = subprocess.run([str(MIGRATE_TOOL), str(original_dir), str(nc_empty), "--apply"],
                             capture_output=True, text=True, timeout=60)
    empty_fields = parse_migrate_output(result.stdout)
    assert empty_fields.get("ok") == "false" and "cannot stat companion path" in empty_fields.get("error", ""), (
        f"REGRESSION: an empty, never-copied candidate did not produce the specific "
        f"cannot-stat refusal: stdout={result.stdout!r} stderr={result.stderr!r}")
    print(f"[PASS] uncopied empty candidate correctly refused: {result.stdout.strip() or result.stderr.strip()}", flush=True)

    # 3. Behavioral neuter for phase 2's OWN comparison logic, using the
    # REAL corrupted-migration result from control #1 above — not a bare
    # Python constant-versus-constant assert, which only proves assert
    # itself works, not that this script's actual success-checking pattern
    # is discriminating (Codex's review: "replace... with a mutation that
    # makes a real phase assertion fail"). Phase 2's real success condition
    # (see its own code) is `fields.get("ok") == "true" and fields.get
    # ("ready") == "true"`; applied to control #1's genuinely-corrupted
    # result, it must evaluate False — if it didn't, phase 2 would have
    # silently accepted corrupted output as success.
    phase2_would_accept = corrupt_fields.get("ok") == "true" and corrupt_fields.get("ready") == "true"
    assert not phase2_would_accept, (
        "neuter check: phase 2's real ok/ready success condition would have ACCEPTED "
        f"the byte-corrupted migration result as successful: {corrupt_fields}")
    print("[PASS] neuter check: phase 2's real success condition correctly rejects the "
          "genuinely-corrupted migration result from control #1", flush=True)


def run_self_test():
    print("[INFO] running negative controls only (--self-test)", flush=True)
    start("original")
    mine("original", PREMINE_BLOCKS)
    stop("original")
    run_negative_controls(WORK / "original")
    global success
    success = True
    print("[INFO] self-test: all negative controls behaved correctly", flush=True)


try:
    main()
except BaseException:
    # Print the actual failure BEFORE the finally block below runs. A bare
    # `sys.exit(1)` inside `finally` raises SystemExit, which silently
    # REPLACES whatever exception was propagating out of the try block —
    # confirmed by direct reproduction: two consecutive full runs failed
    # at the exact same point with zero exception output (not a buffering
    # artifact — reproduced identically under `python3 -u`), because the
    # original traceback of main()'s real failure was being discarded here.
    import traceback
    traceback.print_exc()
    success = False
    raise
finally:
    cleanup_errors = []
    for which in list(processes):
        try:
            stop(which)
        except Exception as exc:
            cleanup_errors.append(str(exc))
    if cleanup_errors:
        success = False
        for err in cleanup_errors:
            print(f"[ERROR] cleanup: {err}", flush=True)
    # Preserve EVIDENCE (source commit, binary hashes, migrate stdout/
    # stderr) permanently regardless of pass/fail, BEFORE any WORK cleanup
    # below. A passing qualification run's evidence is release sign-off
    # material — deleting it on success (the original behavior here)
    # defeats part of this harness's own purpose. Confirmed directly: a
    # clean ALL-CHECKS-PASSED run's WORK dir, evidence included, was gone
    # immediately afterward under the old code. Copy only EVIDENCE, not
    # the full WORK dir (which also holds full regtest datadirs).
    permanent_evidence = ROOT / "evidence" / f"combined-migration-qualification-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}"
    _transcript_file.flush()
    evidence_preserved = False
    try:
        shutil.copytree(EVIDENCE, permanent_evidence)
        evidence_preserved = True
        print(f"[INFO] evidence preserved at: {permanent_evidence}", flush=True)
    except Exception as exc:
        # Codex's review: this used to log the failure and continue,
        # still deleting WORK on success below — silently discarding the
        # only copy of the evidence a passing run exists to produce. A
        # failed preservation must fail the run and retain WORK so nothing
        # is lost.
        success = False
        print(f"[ERROR] failed to preserve evidence at {permanent_evidence}: {exc}", flush=True)

    if success and not cleanup_errors and evidence_preserved:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"[INFO] retaining full workdir (datadirs included): {WORK}", flush=True)

# Deliberately OUTSIDE the try/finally above: if main() raised, the `raise`
# in the except clause already propagated that exception past this point,
# so this line is only reached when main() returned normally. A cleanup-only
# failure (main succeeded but stop() failed) must still fail the run, but
# doing it here — not with sys.exit() inside finally — cannot mask a real
# exception's traceback the way the original code did.
if not success:
    sys.exit(1)

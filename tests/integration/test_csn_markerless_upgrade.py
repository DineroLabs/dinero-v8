#!/usr/bin/env python3
"""An old CSN sidecar without CSN-SP1 must be refetched, then connect.

The offline mutator is restricted to a stopped, explicitly marked regtest
datadir. Both cases begin with real mined, validated blocks and replay data.
One lets the source peer return in the same process; the other restarts the
CSN after quarantine and checks the cleared metadata on disk first.
"""
import json
import os
from pathlib import Path
import subprocess
import time

import test_csn_replay_metadata_recovery as base


RESTART_AFTER_QUARANTINE = os.environ.get("CSN_MARKERLESS_RESTART") == "1"


def inspect(node, blockhash, stage=False):
    base.require(node.process is None, "offline fixture requires stopped CSN")
    args = [str(base.FIXTURE), "--regtest-fixture", "--datadir", str(node.path),
            "--hash", blockhash,
            "--stage-markerless" if stage else "--inspect-markerless"]
    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    label = "stage" if stage else "inspect"
    (base.WORK / f"markerless-{label}-{blockhash}.log").write_text(
        result.stdout + result.stderr)
    base.require(result.returncode == 0,
                 f"{label} fixture failed: {result.stdout}\n{result.stderr}")
    lines = [json.loads(line.removeprefix("FIXTURE_RESULT "))
             for line in result.stdout.splitlines()
             if line.startswith("FIXTURE_RESULT ")]
    base.require(len(lines) == 1, "fixture did not emit exactly one result")
    return lines[0]


def log_text(node):
    return "\n".join(path.read_text(errors="replace") for path in
                     (node.path / "daemon.log", base.WORK / f"{node.name}.log")
                     if path.exists())


def main():
    for binary in (base.BINARY, base.FIXTURE):
        base.require(binary.is_file(), f"missing executable: {binary}")
    base.RECEIPT.update({
        "binary_sha256": base.sha256(base.BINARY),
        "fixture_sha256": base.sha256(base.FIXTURE),
        "test_sha256": base.sha256(Path(__file__)),
        "markerless_restart": RESTART_AFTER_QUARANTINE,
    })
    full, csn = base.Node("full"), base.Node("csn", True)
    full.peer, csn.peer = csn, full
    full.start()
    address = full.call("wallet.getnewaddress")["address"]
    base.mine_mature_prefix(full, address)
    csn.start()
    parent = full.call("getbestblockhash")
    base.wait(lambda: csn.call("getbestblockhash") == parent,
              "CSN mature prefix")
    parent_root = base.root(full)
    base.require(base.root(csn) == parent_root, "prefix roots disagree")
    (csn.path / "csn-replay-metadata-test-only").write_text(
        f"csn-replay-metadata-fixture-v1\n{csn.path.resolve()}\n")

    # Preserve a shorter, valid branch as the CSN's active chain while the
    # markerless, higher-work branch is reconsidered. The import gate checks
    # genuinely competing branches, not a forward extension of the tip.
    alternative = [base.mine(full, address) for _ in range(3)]
    waiting_tip = alternative[-1]
    base.wait(lambda: csn.call("getbestblockhash") == waiting_tip,
              "CSN shorter alternative")
    waiting_root = base.root(csn)
    base.require(base.root(full) == waiting_root, "alternative roots disagree")
    for node in (csn, full):
        node.call("blockchain.invalidateblock", [alternative[0]])
        base.require(node.call("getbestblockhash") == parent and
                     base.root(node) == parent_root,
                     "alternative branch did not rewind")

    first_coin = full.call("getblock", [full.call("getblockhash", [1]), 1])["tx"][0]
    spend_txid, _ = base.spend(full, first_coin, 99.9, address)
    candidate = base.mine(full, address, 10_000_000, [spend_txid])
    for _ in range(3):
        tip = base.mine(full, address)
    base.wait(lambda: csn.call("getbestblockhash") == tip,
              "CSN original fee branch")
    final_root = base.root(full)
    base.require(base.root(csn) == final_root, "original branch roots disagree")

    # The CSN's explicit invalidation leaves a genuine replay sidecar and a
    # stored, Merkle-bound body on a noncanonical header. Downgrade only the
    # proof-stage attestation; never synthesize proof or block bytes.
    csn.call("blockchain.invalidateblock", [candidate])
    base.require(csn.call("getbestblockhash") == parent and
                 base.root(csn) == parent_root,
                 "CSN did not rewind to the common ancestor")
    full.stop()
    csn.stop()
    staged = inspect(csn, candidate, stage=True)
    before, after = staged["before"], staged["after"]
    base.require(before["have_data"] and before["has_replay_sidecar"] and
                 after["have_data"] and after["has_replay_sidecar"] and
                 not after["transactions_valid"] and not after["has_proof_marker"],
                 "fixture did not retain an old markerless sidecar/body")
    base.RECEIPT["markerless_stage"] = staged

    csn.start()
    base.require(csn.call("getbestblockhash") == parent,
                 "markerless startup changed canonical tip")
    csn.call("blockchain.reconsiderblock", [alternative[0]])
    base.wait(lambda: csn.call("getbestblockhash") == waiting_tip,
              "CSN restores shorter competing branch")
    base.require(base.root(csn) == waiting_root,
                 "shorter branch changed during restart")
    csn.call("blockchain.reconsiderblock", [candidate])
    quarantine_messages = (
        "Re-requesting CSN body without speculative proof marker",
        "Reorg body has no durable proof-staged marker; awaiting revalidation",
    )
    base.wait(lambda: any(message in log_text(csn) for message in quarantine_messages),
              "markerless body quarantine", timeout=45)
    base.require(csn.call("getbestblockhash") == waiting_tip and
                 base.root(csn) == waiting_root,
                 "unvalidated old body changed the canonical state")
    print("PASS markerless sidecar quarantined and scheduler explicitly re-requested its hash",
          flush=True)

    if RESTART_AFTER_QUARANTINE:
        csn.stop()
        quarantined = inspect(csn, candidate)["before"]
        base.require(not quarantined["have_data"] and
                     not quarantined["transactions_valid"] and
                     quarantined["data_size"] == 0 and
                     quarantined["has_replay_sidecar"],
                     "quarantine did not durably clear body availability")
        base.RECEIPT["quarantined_after_restart"] = quarantined
        csn.start()
        base.require(csn.call("getbestblockhash") == waiting_tip,
                     "restarted CSN used quarantined body")
        print("PASS restart retains header/sidecar but clears stale body flags and locator",
              flush=True)

    # No second reconsiderblock, reindex or manual block submission. The
    # returning peer must supply fresh bytes and the CSN must validate them.
    full.start()
    base.require(full.call("getbestblockhash") == tip and
                 base.root(full) == final_root,
                 "source peer changed while stopped")
    base.wait(lambda: csn.call("getbestblockhash") == tip,
              "CSN automatic refetch/revalidation", timeout=120)
    base.require(base.root(csn) == final_root,
                 "recovered CSN root differs from source")
    log = log_text(csn)
    quarantine_at = max(log.find(message) for message in quarantine_messages)
    base.require(quarantine_at >= 0, "missing quarantine receipt")
    after_quarantine = log[quarantine_at:]
    if RESTART_AFTER_QUARANTINE:
        base.require("Sent getdata(MSG_UTREEXO_BLOCK) for block " + candidate in
                     after_quarantine,
                     "restarted scheduler did not fetch quarantined hash")
    else:
        base.require("Re-requesting block: " + candidate in after_quarantine,
                     "scheduler did not explicitly re-request quarantined hash")
    base.require("Validated and staged competing block" in after_quarantine,
                 "fresh body did not pass speculative proof validation")
    base.RECEIPT.update({"markerless_restart": RESTART_AFTER_QUARANTINE,
                         "final_tip": tip, "final_root": final_root})
    print("PASS fresh body revalidated and competing branch connected" +
          (" after restart" if RESTART_AFTER_QUARANTINE else " in the same process"),
          flush=True)


if __name__ == "__main__":
    base.run_test(main)

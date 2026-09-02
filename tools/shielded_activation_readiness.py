#!/usr/bin/env python3
"""Fail-closed shielded activation preflight for source trees and live RPC data."""

import argparse
import json
import pathlib
import re
import subprocess
import sys


def source_checks(repo: pathlib.Path):
    chainparams = (repo / "src/consensus/chainparams_impl.cpp").read_text()
    rpc = (repo / "src/rpc/shielded_rpc_json.cpp").read_text()
    ui = (repo / "qt/src/shieldedwidget.h").read_text()
    header = (repo / "include/consensus/chainparams.h").read_text()
    validation = (repo / "src/consensus/shielded/shielded_validation.cpp").read_text()
    block = (repo / "src/consensus/shielded/shielded_block_section.cpp").read_text()
    wallet_runtime = (repo / "src/wallet/shielded_wallet_runtime.cpp").read_text()
    note_store = (repo / "src/wallet/shielded_note_store.cpp").read_text()
    checks = {
        "mainnet_wallet_rpc_locked": "GetActiveChain() == Chain::MAINNET" in rpc and
            "shielded_spend_locked" in rpc,
        "qt_ui_locked": bool(re.search(r"kShieldedUiLockedOut\s*=\s*true", ui)),
        "spend_auth_default_dormant": bool(re.search(
            r"shielded_spend_auth_activation_height\s*=\s*UINT32_MAX", header)),
        "activation_order_enforced": "spend_auth_activation_height <" in chainparams and
            "shielded_cv_binding_activation_height" in chainparams,
        "anchor_rollback_persisted": "SerializePersistenceBytes" in
            (repo / "src/consensus/shielded/anchor_history.cpp").read_text(),
        "legacy_address_rejected": "legacy 43-byte payload is REJECTED" in
            (repo / "src/wallet/shielded_derivation.cpp").read_text(),
        "spend_auth_epoch_reset_supported":
            "shielded_spend_auth_epoch_reset_height" in header and
            "shielded_spend_auth_epoch_reset_height" in chainparams and
            "spend_auth_reset_height" in block and
            "IsShieldedEpochResetHeight" in block,
        "spend_auth_proof_gate_consumed":
            "shielded_spend_auth_activation_height" in validation and
            "spend_auth" in validation and "VerifySpend" in validation,
        "distinct_reset_pairing_enforced":
            "equal shielded_spend_auth_activation_height" in chainparams and
            "spend-auth and cv-binding epoch resets must" in chainparams,
        "auth_self_and_change_addressed":
            "DeriveFreshOwnedRecipient" in wallet_runtime and
            "spend-auth era requires addressed shielded change" not in wallet_runtime and
            "spend-auth era requires an addressed recipient output" not in wallet_runtime,
        "auth_diversifier_persisted":
            "diversifier" in note_store and "ReadHash(stmt, 14, n.d)" in note_store,
    }
    return checks


def rpc_call(cli: pathlib.Path, args, method, *params):
    cmd = [str(cli), *args, method, *params]
    proc = subprocess.run(cmd, check=False, text=True, capture_output=True)
    if proc.returncode:
        raise RuntimeError(f"{' '.join(cmd)}: {proc.stderr.strip()}")
    return json.loads(proc.stdout)


def live_checks(cli, cli_args):
    info = rpc_call(cli, cli_args, "getblockchaininfo")
    result = {
        "chain": info.get("chain"),
        "height": info.get("active_height"),
        "best_block_hash": info.get("active_best_hash"),
        "shielded_tree_size": info.get("shielded_tree_size"),
        "shielded_nullifier_count": info.get("shielded_nullifier_count"),
        "shielded_tip_marker_found": info.get("shielded_tip_marker_found", False),
        "shielded_tip_marker_height": info.get("shielded_tip_marker_height"),
        "shielded_tip_marker_hash": info.get("shielded_tip_marker_hash"),
        "frontier_root": info.get("shielded_frontier_root"),
        "marker_root": info.get("shielded_tip_marker_root"),
        "canonical_state_aligned": info.get("canonical_state_aligned", False),
    }
    result["tip_marker_aligned"] = (
        result["shielded_tip_marker_found"] and
        result["height"] == result["shielded_tip_marker_height"] and
        result["best_block_hash"] == result["shielded_tip_marker_hash"])
    result["root_aligned"] = (result["shielded_tip_marker_found"] and
                              result["frontier_root"] == result["marker_root"])
    result["epoch_reset_would_strand_value"] = bool(result["shielded_tree_size"])
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--repo", type=pathlib.Path, default=pathlib.Path(__file__).parents[1])
    p.add_argument("--source-only", action="store_true")
    p.add_argument("--cli", type=pathlib.Path)
    p.add_argument("--cli-arg", action="append", default=[])
    p.add_argument("--json", type=pathlib.Path)
    ns = p.parse_args()
    checks = source_checks(ns.repo.resolve())
    report = {"source_checks": checks, "source_ready": all(checks.values())}
    if not ns.source_only:
        if not ns.cli:
            p.error("--cli is required unless --source-only is used")
        report["live"] = live_checks(ns.cli, ns.cli_arg)
        report["live_ready"] = (report["live"]["tip_marker_aligned"] and
                                report["live"]["root_aligned"] and
                                report["live"]["canonical_state_aligned"] and
                                not report["live"]["epoch_reset_would_strand_value"])
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if ns.json:
        ns.json.parent.mkdir(parents=True, exist_ok=True)
        ns.json.write_text(encoded)
    sys.stdout.write(encoded)
    return 0 if report["source_ready"] and report.get("live_ready", True) else 1


if __name__ == "__main__":
    raise SystemExit(main())

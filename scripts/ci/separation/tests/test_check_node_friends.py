"""F1 checker unit + wiring tests (sep2 program, stage A)."""
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import check_node_friends as cnf  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[4]
WORKFLOW = REPO / ".github" / "workflows" / "separation-friend-gate.yml"


def test_flags_node_side_friend_qualified_and_bare():
    text = (
        "class WalletThing {\n"
        "private:\n"
        "    friend class dinero::ChainstateService;\n"
        "    friend class ChainstateService;\n"
        "    friend struct ChainstateService;\n"
        "};\n"
    )
    findings = cnf.scan_text(text, "include/wallet/x.h")
    assert findings == [
        "include/wallet/x.h:3 grants ChainstateService",
        "include/wallet/x.h:4 grants ChainstateService",
        "include/wallet/x.h:5 grants ChainstateService",
    ]


def test_ignores_wallet_internal_friends_comments_and_substrings():
    text = (
        "// friend class ChainstateService; (historical note, not a grant)\n"
        "    friend class OrchardAccountDelivery;\n"
        "    friend class dinero::RuntimeWalletRecovery;\n"
        "    friend class MyChainstateServiceFixture;\n"
        "    friend struct OrchardDetachedRecoveryTestAccess;\n"
    )
    assert cnf.scan_text(text, "include/wallet/y.h") == []


def test_live_repo_findings_match_design_inventory():
    # Stage B state: 8 grants remain (stage B dropped runtime_account_replay.h,
    # runtime_wallet_balance.h and wallet_transaction_signer.h; stage A dropped
    # the orchard_account_delivery.h:443 duplicate). Each later stage shrinks
    # this list; stage E empties it. Update this expectation in the same PR
    # that drops a grant.
    findings = cnf.scan_repo(REPO)
    files = sorted({f.split(":")[0] for f in findings})
    assert len(findings) == 8, findings
    assert files == [
        "include/wallet/authenticated_shield_inputs.h",
        "include/wallet/orchard_account_catalog.h",
        "include/wallet/orchard_account_delivery.h",
        "include/wallet/runtime_index_delivery.h",
        "include/wallet/runtime_origin_projection.h",
        "include/wallet/selected_history.h",
        "include/wallet/wallet_manager.h",
    ]


def test_cli_exit_codes(tmp_path):
    clean = tmp_path / "clean"
    (clean / "include" / "wallet").mkdir(parents=True)
    (clean / "src" / "wallet").mkdir(parents=True)
    (clean / "include" / "wallet" / "a.h").write_text(
        "class A { friend class WalletManager; };\n"
    )
    ok = subprocess.run(
        [sys.executable, str(REPO / "scripts/ci/separation/check_node_friends.py"), str(clean)],
        capture_output=True, text=True,
    )
    assert ok.returncode == 0 and "GATE F1 PASS" in ok.stdout

    (clean / "src" / "wallet" / "b.cpp").write_text(
        "class B {\n    friend class dinero::ChainstateService;\n};\n"
    )
    bad = subprocess.run(
        [sys.executable, str(REPO / "scripts/ci/separation/check_node_friends.py"), str(clean)],
        capture_output=True, text=True,
    )
    assert bad.returncode == 1
    assert "src/wallet/b.cpp:2 grants ChainstateService" in bad.stdout
    assert "GATE F1 FAIL rc=1 (1 node-side friend grant(s) in wallet code)" in bad.stdout


def test_workflow_invokes_checker_unconditionally():
    text = WORKFLOW.read_text()
    assert "pull_request" in text
    assert "python3 scripts/ci/separation/check_node_friends.py" in text
    # No path filter and no `if:` on the job: F1 is cheap and always runs, so
    # it never hits the skipped-matrix-name problem (#890/#891).
    assert "paths:" not in text
    assert "\n    if:" not in text

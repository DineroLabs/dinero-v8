#!/usr/bin/env python3
import argparse
import json
import pathlib
import subprocess
import sys


def run_version(binary: pathlib.Path) -> str:
    proc = subprocess.run([str(binary), "--version"], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{binary} --version failed: {proc.stderr.strip()}")
    return proc.stdout.strip()


def resolve_binary(path: pathlib.Path | None, alternates: list[pathlib.Path] | None = None) -> pathlib.Path | None:
    if path is None:
        return None
    candidates = [path]
    if alternates:
        candidates.extend(alternates)
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return path


def parse_identity(output: str) -> dict:
    parsed = {}
    for line in output.splitlines():
        if ": " not in line:
            continue
        key, value = line.split(": ", 1)
        parsed[key.strip()] = value.strip()
    return parsed


def git_head(repo: pathlib.Path) -> str:
    proc = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    )
    return proc.stdout.strip()


def verify_match(name: str, actual: str, expected: str, failures: list[str]) -> None:
    if expected and expected != "unknown" and actual != expected:
        failures.append(f"{name}: expected {expected}, got {actual}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify Dinero release identities")
    parser.add_argument("--bundle", type=pathlib.Path, required=True, help="Path to Dinero-Qt.app")
    parser.add_argument("--dinero-repo", type=pathlib.Path, default=None)
    parser.add_argument("--qt-repo", type=pathlib.Path, default=None)
    parser.add_argument("--stratum-bin", type=pathlib.Path, default=None)
    parser.add_argument("--stratum-repo", type=pathlib.Path, default=None)
    args = parser.parse_args()

    bundle = args.bundle
    manifest_path = bundle / "Contents" / "Resources" / "release-identity.json"
    app_bin = bundle / "Contents" / "MacOS" / "dinero-qt"
    daemon_bin = bundle / "Contents" / "Resources" / "dinerod"
    daemon_bin_alt = bundle / "Contents" / "MacOS" / "dinerod"
    gpu_bin = bundle / "Contents" / "MacOS" / "dinero-gpu-miner"
    cpu_bin = bundle / "Contents" / "Resources" / "dinero-miner"
    cpu_bin_alt = bundle / "Contents" / "MacOS" / "dinero-miner"
    stratum_worker_bin = bundle / "Contents" / "Resources" / "dinero-stratum-worker"
    stratum_worker_bin_alt = bundle / "Contents" / "MacOS" / "dinero-stratum-worker"

    manifest = json.loads(manifest_path.read_text())
    failures: list[str] = []

    qt_identity = parse_identity(run_version(app_bin))
    verify_match("qt bundle commit", qt_identity.get("commit", ""), manifest.get("qt", {}).get("commit", ""), failures)
    verify_match(
        "embedded solo miner commit",
        qt_identity.get("embedded_solo_miner_commit", ""),
        manifest.get("expected_repo_heads", {}).get("dinero_solo_miner", ""),
        failures,
    )

    if daemon_bin.exists():
        daemon_identity = parse_identity(run_version(daemon_bin))
        verify_match("bundled dinerod commit", daemon_identity.get("commit", ""), manifest.get("expected_repo_heads", {}).get("dinero", ""), failures)
        if daemon_bin_alt.exists():
            daemon_alt_identity = parse_identity(run_version(daemon_bin_alt))
            verify_match(
                "bundled dinerod macOS copy commit",
                daemon_alt_identity.get("commit", ""),
                daemon_identity.get("commit", ""),
                failures,
            )

    if gpu_bin.exists():
        gpu_identity = parse_identity(run_version(gpu_bin))
        verify_match("bundled gpu miner commit", gpu_identity.get("commit", ""), manifest.get("expected_repo_heads", {}).get("dinero", ""), failures)

    if cpu_bin.exists():
        cpu_identity = parse_identity(run_version(cpu_bin))
        verify_match("bundled cpu miner commit", cpu_identity.get("commit", ""), manifest.get("expected_repo_heads", {}).get("dinero", ""), failures)
        if cpu_bin_alt.exists():
            cpu_alt_identity = parse_identity(run_version(cpu_bin_alt))
            verify_match(
                "bundled cpu miner macOS copy commit",
                cpu_alt_identity.get("commit", ""),
                cpu_identity.get("commit", ""),
                failures,
            )

    if stratum_worker_bin.exists():
        stratum_worker_identity = parse_identity(run_version(stratum_worker_bin))
        verify_match(
            "bundled stratum worker commit",
            stratum_worker_identity.get("commit", ""),
            manifest.get("expected_repo_heads", {}).get("dinero", ""),
            failures,
        )
        if stratum_worker_bin_alt.exists():
            stratum_worker_alt_identity = parse_identity(run_version(stratum_worker_bin_alt))
            verify_match(
                "bundled stratum worker macOS copy commit",
                stratum_worker_alt_identity.get("commit", ""),
                stratum_worker_identity.get("commit", ""),
                failures,
            )

    if args.qt_repo:
        verify_match("qt repo HEAD", qt_identity.get("commit", ""), git_head(args.qt_repo), failures)
    if args.dinero_repo and daemon_bin.exists():
        daemon_identity = parse_identity(run_version(daemon_bin))
        verify_match("dinero repo HEAD", daemon_identity.get("commit", ""), git_head(args.dinero_repo), failures)
    if args.stratum_bin and args.stratum_repo:
        resolved_stratum = resolve_binary(
            args.stratum_bin,
            [args.stratum_bin.parent / "bin" / args.stratum_bin.name],
        )
        stratum_identity = parse_identity(run_version(resolved_stratum))
        verify_match("stratum repo HEAD", stratum_identity.get("commit", ""), git_head(args.stratum_repo), failures)

    if failures:
        print("RELEASE_IDENTITY_CHECK=FAIL")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("RELEASE_IDENTITY_CHECK=PASS")
    print(f"qt={qt_identity.get('commit', 'unknown')}")
    if daemon_bin.exists():
        print(f"dinerod={parse_identity(run_version(daemon_bin)).get('commit', 'unknown')}")
    if gpu_bin.exists():
        print(f"gpu_miner={parse_identity(run_version(gpu_bin)).get('commit', 'unknown')}")
    if cpu_bin.exists():
        print(f"cpu_miner={parse_identity(run_version(cpu_bin)).get('commit', 'unknown')}")
    if stratum_worker_bin.exists():
        print(f"stratum_worker={parse_identity(run_version(stratum_worker_bin)).get('commit', 'unknown')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

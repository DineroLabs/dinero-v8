#!/usr/bin/env python3
"""Inspect actual package binaries without starting Qt, a node, or RPC.

This proves compiled backend/profile and UI presence only. It does not prove
activation, runtime wallet readiness, or end-to-end installed-app behavior.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate build-info field: " + key)
        result[key] = value
    return result


def validate_report(text, component):
    report = json.loads(text, object_pairs_hook=unique_object)
    if type(report) is not dict or type(report.get("schema")) is not int or report["schema"] != 1:
        raise ValueError("unsupported Orchard build-info schema")
    if report.get("component") != component:
        raise ValueError("wrong Orchard build-info component")
    if component == "dinerod":
        if set(report) != {"schema", "component", "orchard_backend", "profile"}:
            raise ValueError("unexpected daemon build-info fields")
        if report["orchard_backend"] is not True:
            raise ValueError("package is missing the Orchard backend")
        profile = report["profile"]
        if (type(profile) is not list or any(type(n) is not int for n in profile)
                or profile != [7, 1, 1, 1, 5]):
            raise ValueError("package has an incompatible Orchard protocol profile")
    elif component == "dinero-qt":
        if set(report) != {"schema", "component", "orchard_ui"} or report["orchard_ui"] is not True:
            raise ValueError("package is missing the Orchard screens")
    else:
        raise ValueError("unsupported package component")
    return report


def inspect_binary(binary, component):
    binary = Path(binary).resolve(strict=True)
    if not binary.is_file():
        raise ValueError("expected a package binary")
    before = hashlib.sha256(binary.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="dinero-package-info-") as root:
        env = dict(os.environ, HOME=root, USERPROFILE=root,
                   APPDATA=root, LOCALAPPDATA=root, XDG_CONFIG_HOME=root,
                   XDG_DATA_HOME=root, XDG_CACHE_HOME=root, TMPDIR=root,
                   QT_QPA_PLATFORM="offscreen")
        # Old binaries recognize --version and exit without opening a datadir
        # or starting listeners; their non-JSON report is then rejected.
        result = subprocess.run([str(binary), "--orchard-build-info", "--version"],
                                cwd=root, env=env, capture_output=True, text=True,
                                timeout=10, check=True)
        report = validate_report(result.stdout, component)
        if any(Path(root).iterdir()):
            raise ValueError("build-info inspection unexpectedly created runtime state")
    if hashlib.sha256(binary.read_bytes()).hexdigest() != before:
        raise ValueError("package binary changed during inspection")
    return {"path": str(binary), "sha256": before, "report": report}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon", required=True, type=Path)
    parser.add_argument("--qt", type=Path)
    args = parser.parse_args()
    reports = [inspect_binary(args.daemon, "dinerod")]
    if args.qt is not None:
        reports.append(inspect_binary(args.qt, "dinero-qt"))
    print(json.dumps({"scope": "compiled-capabilities-only", "binaries": reports}, indent=2))


if __name__ == "__main__":
    main()

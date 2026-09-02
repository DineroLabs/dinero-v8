#!/usr/bin/env python3
"""Fail releases on known production-reachable cryptographic placeholders."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
scopes = [ROOT / "src", ROOT / "qt/src"]
forbidden = {
    "dummy_signature_literal": re.compile(r'dummy_sig_(?:buyer|seller)', re.I),
    "zero_lightning_node_key": re.compile(r'test_node_pubkey\s*=\s*"02"\s*\+\s*std::string\(64,\s*\'0\'\)'),
}
allow = {
    # This source is guarded by src/lightningd/CMakeLists.txt: DINERO_RELEASE
    # configurations fail if BUILD_LIGHTNINGD is requested.
    ROOT / "src/lightningd/lightning/lightning_app.cpp": {"zero_lightning_node_key"},
}
findings = []
for scope in scopes:
    for path in scope.rglob("*"):
        if path.suffix not in {".cpp", ".h", ".mm"} or not path.is_file():
            continue
        text = path.read_text(errors="replace")
        for name, pattern in forbidden.items():
            if pattern.search(text) and name not in allow.get(path, set()):
                findings.append(f"{path.relative_to(ROOT)}: {name}")
if findings:
    raise SystemExit("production placeholder tripwire failed:\n" + "\n".join(findings))
print("PASS: no forbidden production cryptographic placeholders")

#!/usr/bin/env python3
"""Exercise the real shell mining helpers without launching regtest daemons."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def function(path: Path, name: str) -> str:
    pattern = rf"(?ms)^{re.escape(name)}\(\) \{{\n.*?^\}}"
    match = re.search(pattern, path.read_text())
    if not match:
        raise AssertionError(f"{path}: {name} function missing")
    return match.group()


def run(source: str, env: dict[str, str]) -> str:
    result = subprocess.run(
        ["bash", "-s"], input="set -euo pipefail\n" + source,
        text=True, capture_output=True, env=env,
    )
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout.strip()


def timeout_for(path: Path, name: str, method: str, env: dict[str, str]) -> int:
    body = function(path, name)
    call = f'{name} 12345 "$RPC_DATADIR" {method} "120, \\\"address\\\""'
    source = (
        'curl() { printf "%s\\n" "$@" > "$CURL_ARGS"; '
        'printf "{\\"error\\":null,\\"result\\":[]}\\n"; }\n'
        + body + "\n" + call + " >/dev/null\n"
    )
    run(source, env)
    args = Path(env["CURL_ARGS"]).read_text().splitlines()
    return int(args[args.index("--max-time") + 1])


def main() -> None:
    spend, shielded = (Path(arg).resolve() for arg in sys.argv[1:3])
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / ".cookie").write_text("test:test\n")
        env = os.environ | {"RPC_DATADIR": str(root), "CURL_ARGS": str(root / "curl-args")}
        for path, name in ((spend, "rpc_call"), (shielded, "rpc")):
            mining = timeout_for(path, name, "generatetoaddress", env)
            ordinary = timeout_for(path, name, "getblockcount", env)
            assert mining >= 180, f"{path.name}: mining RPC timed out after {mining}s"
            assert ordinary == 30, f"{path.name}: ordinary RPC timeout changed to {ordinary}s"

        mine = function(spend, "mine_blocks_to_address")
        source = (
            'rpc_call() { return 28; }\n'
            'rpc_has_error() { return 1; }\n'
            + mine + '\n'
            'if mine_blocks_to_address 12345 "$RPC_DATADIR" 120 address; then\n'
            '  echo "mining helper ignored curl failure" >&2; exit 1\n'
            'fi\n'
        )
        run(source, env)
        source = (
            'rpc_call() { printf "{}\\n"; }\n'
            'rpc_has_error() { return 1; }\n'
            + mine + '\n'
            'if mine_blocks_to_address 12345 "$RPC_DATADIR" 120 address; then\n'
            '  echo "mining helper accepted an empty success envelope" >&2; exit 1\n'
            'fi\n'
        )
        run(source, env)
        source = (
            'rpc_call() { printf \'{"error":null,"result":{"blocks":["one"]}}\\n\'; }\n'
            'rpc_has_error() { return 1; }\n'
            + mine + '\n'
            'if mine_blocks_to_address 12345 "$RPC_DATADIR" 120 address; then\n'
            '  echo "mining helper accepted a short success batch" >&2; exit 1\n'
            'fi\n'
        )
        run(source, env)
    print("PASS: mining RPCs have bounded time and curl failure cannot pass")


if __name__ == "__main__":
    main()

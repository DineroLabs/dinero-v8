#!/usr/bin/env python3
"""Check an installed/extracted tower starts and rejects missing configuration.

No inbox, RPC endpoint or credentials are passed. This is an executable packaging
smoke check, not swap protocol or watchtower behavior qualification.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify-swap-tower-binary.py <packaged-binary>")
    binary = Path(sys.argv[1]).resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise SystemExit("tower is not an executable file")
    with tempfile.TemporaryDirectory(prefix="dinero-tower-package-check-") as work:
        try:
            result = subprocess.run([str(binary)], cwd=work, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=10, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise SystemExit(f"packaged tower could not complete its usage check: {error}")
        if result.returncode != 2 or b"missing --inbox" not in result.stderr:
            raise SystemExit(f"packaged tower usage check failed (exit {result.returncode})")
        if list(Path(work).iterdir()):
            raise SystemExit("unconfigured tower unexpectedly created files")
    print("OK: packaged dinero-swap-tower starts and refuses missing configuration")


if __name__ == "__main__":
    main()

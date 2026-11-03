#!/usr/bin/env python3
"""The swap topology must skip a port that another test already owns."""

import pathlib
import re
import socket
import subprocess
import sys
import tempfile


def bash(script: str) -> str:
    return subprocess.run(
        ["bash", "-c", script], check=True, capture_output=True, text=True
    ).stdout.strip()


def probe(script_path: pathlib.Path, seed: int) -> int:
    source = script_path.read_text()
    assignment = re.search(r"^DPORT=.*$", source, re.MULTILINE)
    if assignment is None:
        raise RuntimeError("swap fixture has no DPORT assignment")
    source_helper = next(
        (line for line in source.splitlines()
         if line.startswith("source ") and "port_alloc.sh" in line),
        "",
    )
    with tempfile.TemporaryDirectory() as temp:
        root = pathlib.Path(temp)
        wallet_dir = root / "tests" / "wallet"
        helper_dir = root / "tests" / "integration" / "lib"
        wallet_dir.mkdir(parents=True)
        helper_dir.mkdir(parents=True)
        (helper_dir / "port_alloc.sh").symlink_to(
            script_path.parents[1] / "integration" / "lib" / "port_alloc.sh"
        )
        fixture = wallet_dir / "probe.sh"
        fixture.write_text(
            f"#!/usr/bin/env bash\nset -euo pipefail\nRANDOM={seed}\n"
            f"{source_helper}\n{assignment.group(0)}\nprintf '%s\\n' \"$DPORT\"\n"
        )
        return int(subprocess.run(
            ["bash", str(fixture)], check=True, capture_output=True, text=True
        ).stdout.strip())


def main() -> None:
    script_path = pathlib.Path(sys.argv[1]).resolve()
    for seed in range(1, 101):
        old_base = int(bash(f"RANDOM={seed}; echo $((20000 + RANDOM % 20000))"))
        listener = socket.socket()
        try:
            listener.bind(("127.0.0.1", old_base))
            listener.listen(1)
        except OSError:
            listener.close()
            continue
        try:
            selected = probe(script_path, seed)
            if selected <= old_base <= selected + 23:
                raise AssertionError(
                    f"swap topology {selected}..{selected + 23} includes occupied port {old_base}"
                )
            print(f"PASS: swap topology {selected}..{selected + 23} skipped occupied {old_base}")
            return
        finally:
            listener.close()
    raise RuntimeError("no free collision port found in 100 seeded candidates")


if __name__ == "__main__":
    main()

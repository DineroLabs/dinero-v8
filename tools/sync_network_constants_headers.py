#!/usr/bin/env python3
"""Regenerate network-constants headers from chainparams.

`src/consensus/chainparams_impl.cpp` is the single source of truth for the
P2P wire magic of each Dinero network (mainnet / testnet / regtest). Any
process that needs the magic before `dinero::SelectParams()` is called —
notably the standalone seeder — would otherwise need a hand-copied literal.
This script mirrors `tools/sync_chain_identity_headers.py`: it extracts the
canonical values, renders a generated C++ header, and supports a `--check`
mode for the CI drift gate.

Outputs (committed to git, regenerated when chainparams_impl.cpp moves):
  - seeder/include/dinero/seeder/network_constants_generated.h

Usage:
  python3 tools/sync_network_constants_headers.py            # write
  python3 tools/sync_network_constants_headers.py --check    # fail if stale
"""
import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHAINPARAMS = ROOT / "src" / "consensus" / "chainparams_impl.cpp"

NETWORKS = ("mainnet", "testnet", "regtest")


def load_magics() -> dict[str, int]:
    """Extract per-network magic values from chainparams_impl.cpp.

    The file defines three ChainParams structs in order; each starts with
    `.name = "<chain>"` and lists `.magic = 0xNNNNNNNNu;` a few lines
    later. We scan for the `.name = "<chain>"` anchor and then take the
    next `.magic = 0xN;` literal within the next 50 lines.
    """
    text = CHAINPARAMS.read_text(encoding="utf-8")
    lines = text.splitlines()

    magics: dict[str, int] = {}
    for chain in NETWORKS:
        name_pattern = re.compile(rf'\.name\s*=\s*"{re.escape(chain)}"')
        magic_pattern = re.compile(r'\.magic\s*=\s*0x([0-9A-Fa-f]+)u?\s*,')
        anchor: int | None = None
        for i, line in enumerate(lines):
            if name_pattern.search(line):
                anchor = i
                break
        if anchor is None:
            raise SystemExit(
                f"failed to find `.name = \"{chain}\"` anchor in {CHAINPARAMS}"
            )
        for j in range(anchor + 1, min(anchor + 51, len(lines))):
            m = magic_pattern.search(lines[j])
            if m:
                magics[chain] = int(m.group(1), 16)
                break
        else:
            raise SystemExit(
                f"failed to find `.magic = 0xN` within 50 lines of `.name = \"{chain}\"`"
            )
    return magics


def render_seeder_header(magics: dict[str, int]) -> str:
    return f"""#pragma once

// AUTO-GENERATED from src/consensus/chainparams_impl.cpp.
// DO NOT EDIT BY HAND — run tools/sync_network_constants_headers.py
// to regenerate. The drift test in tests/integration/test_network_magic_sync.sh
// fails the build if the contents of this file disagree with the
// canonical chainparams source.

#include <cstdint>
#include <string_view>

namespace dinero::seeder {{

// P2P wire magic, per network. Mirrors the .magic field on the
// ChainParams struct for each Dinero chain. The seeder picks one of
// these based on its CLI --network flag (defaults to mainnet).
inline constexpr uint32_t kMagicMainnet = 0x{magics['mainnet']:08X}u;
inline constexpr uint32_t kMagicTestnet = 0x{magics['testnet']:08X}u;
inline constexpr uint32_t kMagicRegtest = 0x{magics['regtest']:08X}u;

inline constexpr uint32_t MagicForNetwork(std::string_view network) {{
    if (network == "mainnet" || network == "main") {{
        return kMagicMainnet;
    }}
    if (network == "testnet" || network == "test") {{
        return kMagicTestnet;
    }}
    if (network == "regtest") {{
        return kMagicRegtest;
    }}
    return 0u;  // unknown — caller should treat as error
}}

}} // namespace dinero::seeder
"""


def resolve_targets(magics: dict[str, int]) -> dict[Path, str]:
    targets: dict[Path, str] = {}
    seeder_header = ROOT / "seeder" / "include" / "dinero" / "seeder" / "network_constants_generated.h"
    if seeder_header.parent.is_dir():
        targets[seeder_header] = render_seeder_header(magics)
    else:
        print(
            f"warning: {seeder_header.parent} does not exist; "
            "skipping seeder network constants sync",
            file=sys.stderr,
        )
    return targets


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail (exit 1) if generated headers are out of date",
    )
    args = parser.parse_args()

    magics = load_magics()
    targets = resolve_targets(magics)

    mismatches: list[str] = []
    for path, content in targets.items():
        if args.check:
            existing = path.read_text(encoding="utf-8") if path.exists() else None
            if existing != content:
                mismatches.append(str(path))
            continue
        path.write_text(content, encoding="utf-8")
        print(f"wrote {path}")

    if mismatches:
        print("network constants headers are out of date:", file=sys.stderr)
        for path in mismatches:
            print(f"  {path}", file=sys.stderr)
        print(
            "run `python3 tools/sync_network_constants_headers.py` to regenerate.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHAIN_BUNDLE = ROOT / "include" / "consensus" / "chain_bundle_generated.h"
CHAIN_IDENTITY = ROOT / "include" / "consensus" / "chain_identity.h"


def extract(pattern: str, text: str, label: str) -> str:
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        raise SystemExit(f"failed to extract {label}")
    return match.group(1)


def load_hashes() -> dict[str, str]:
    bundle_text = CHAIN_BUNDLE.read_text()
    identity_text = CHAIN_IDENTITY.read_text()
    mainnet = extract(r'GENESIS_BLOCK_HASH\s*=\s*"([0-9a-f]{64})"', bundle_text, "mainnet genesis")
    testnet = extract(r'kTestnetGenesisHash\s*=\s*"([0-9a-f]{64})"', identity_text, "testnet genesis")
    return {
        "mainnet": mainnet,
        "testnet": testnet,
        "regtest": mainnet,
    }


def render_solo(hashes: dict[str, str]) -> str:
    return f"""#pragma once

// AUTO-GENERATED from dinero/include/consensus/chain_identity.h
// Run dinero/tools/sync_chain_identity_headers.py to regenerate.

#include <string_view>

namespace dinero::solo {{

inline constexpr std::string_view kMainnetGenesisHash = "{hashes['mainnet']}";
inline constexpr std::string_view kTestnetGenesisHash = "{hashes['testnet']}";
inline constexpr std::string_view kRegtestGenesisHash = "{hashes['regtest']}";

inline constexpr std::string_view NormalizeNetworkName(std::string_view raw) {{
    if (raw == "main" || raw == "mainnet") {{
        return "mainnet";
    }}
    if (raw == "test" || raw == "testnet") {{
        return "testnet";
    }}
    if (raw == "regtest") {{
        return "regtest";
    }}
    return {{}};
}}

inline constexpr std::string_view ExpectedGenesisForNetwork(std::string_view network) {{
    if (network == "mainnet") {{
        return kMainnetGenesisHash;
    }}
    if (network == "testnet") {{
        return kTestnetGenesisHash;
    }}
    if (network == "regtest") {{
        return kRegtestGenesisHash;
    }}
    return {{}};
}}

}} // namespace dinero::solo
"""


def render_stratum(hashes: dict[str, str]) -> str:
    return f"""#pragma once

// AUTO-GENERATED from dinero/include/consensus/chain_identity.h
// Run dinero/tools/sync_chain_identity_headers.py to regenerate.

#include <string_view>

namespace dinero::stratum {{

inline constexpr std::string_view kMainnetGenesisHash = "{hashes['mainnet']}";
inline constexpr std::string_view kTestnetGenesisHash = "{hashes['testnet']}";
inline constexpr std::string_view kRegtestGenesisHash = "{hashes['regtest']}";

inline constexpr std::string_view NormalizeNetworkName(std::string_view raw) {{
    if (raw == "main" || raw == "mainnet") {{
        return "mainnet";
    }}
    if (raw == "test" || raw == "testnet") {{
        return "testnet";
    }}
    if (raw == "regtest") {{
        return "regtest";
    }}
    return {{}};
}}

inline constexpr std::string_view ExpectedGenesisForNetwork(std::string_view network) {{
    if (network == "mainnet") {{
        return kMainnetGenesisHash;
    }}
    if (network == "testnet") {{
        return kTestnetGenesisHash;
    }}
    if (network == "regtest") {{
        return kRegtestGenesisHash;
    }}
    return {{}};
}}

}} // namespace dinero::stratum
"""


def resolve_targets(hashes: dict[str, str]) -> dict:
    """Build the {output_path: rendered_content} map, monorepo-aware.

    Solo miner target moved into the dinero monorepo at miner/include/...
    in the Phase 2 consolidation (2026-05-12). If miner/ is present in
    this tree, prefer the in-tree path. Otherwise fall back to the
    pre-consolidation sibling layout (dinero-solo-miner/...).

    Stratum is intentionally external during consolidation. Skip
    silently if the stratum sibling is not present in this checkout.
    """
    targets: dict = {}

    # Solo miner: prefer in-tree monorepo path; fall back to sibling.
    monorepo_solo = ROOT / "miner" / "include" / "solo_miner"
    sibling_solo = ROOT.parent / "dinero-solo-miner" / "include" / "solo_miner"
    if monorepo_solo.is_dir():
        targets[monorepo_solo / "chain_identity.h"] = render_solo(hashes)
    elif sibling_solo.is_dir():
        targets[sibling_solo / "chain_identity.h"] = render_solo(hashes)
    else:
        print(
            f"warning: neither {monorepo_solo} nor {sibling_solo} exists; "
            "skipping solo miner chain identity sync",
            file=sys.stderr,
        )

    # Stratum: external sibling, intentionally out of scope for the v8
    # monorepo. Skip cleanly if not in this checkout.
    stratum_dir = ROOT.parent / "stratum" / "include" / "stratum"
    if stratum_dir.is_dir():
        targets[stratum_dir / "chain_identity.h"] = render_stratum(hashes)

    return targets


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail if generated headers are out of date")
    args = parser.parse_args()

    hashes = load_hashes()
    targets = resolve_targets(hashes)

    mismatches = []
    for path, content in targets.items():
        if args.check:
            existing = path.read_text() if path.exists() else None
            if existing != content:
                mismatches.append(str(path))
            continue
        path.write_text(content)
        print(f"wrote {path}")

    if mismatches:
        print("chain identity headers are out of date:", file=sys.stderr)
        for path in mismatches:
            print(f"  {path}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

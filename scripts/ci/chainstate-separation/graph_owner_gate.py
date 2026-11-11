#!/usr/bin/env python3
"""Inventory and ratchet direct access to the shared block graph.

This is a preparatory boundary check. It records the current first-party CMake
compiles of the block graph implementations and raw global references outside
those implementation files. Source scanning is conservative, including dormant
first-party files. A new compile or reference fails; a reduction requires the
baseline to be updated in the same commit. It does not claim that the graph is
already owned by chainstate.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
BASELINE = HERE / "graph_owner_baseline.json"
SOURCE_FILES = (
    "src/consensus/block_index.cpp",
    "src/consensus/block_lifecycle.cpp",
)
RAW_GLOBALS = (
    "g_block_index",
    "g_block_index_mutex",
    "g_candidates",
    "g_orphan_pool",
    "g_invalid_blocks",
    "g_inflight_blocks",
    "g_invalid_descendants",
)
HEADER_SUFFIXES = {".h", ".hpp"}
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx"}
EXCLUDED_CMAKE_DIRS = {".git", ".venv", "_deps", "node_modules", "third_party", "vendor", "out"}


def mask_comments_and_literals(source):
    """Blank C++ comments and string/character literals, preserving newlines."""
    out = []
    i = 0
    n = len(source)
    while i < n:
        two = source[i:i + 2]
        if two == "//":
            end = source.find("\n", i)
            end = n if end < 0 else end
        elif two == "/*":
            close = source.find("*/", i + 2)
            end = n if close < 0 else close + 2
        elif two == 'R"':
            opening = source.find("(", i + 2)
            delimiter = source[i + 2:opening] if opening >= 0 else ""
            if opening >= 0 and len(delimiter) <= 16 and not any(c in delimiter for c in " ()\\\t\n"):
                close = source.find(")" + delimiter + '"', opening + 1)
                end = n if close < 0 else close + len(delimiter) + 2
            else:
                end = i + 1
        elif source[i] in "\"'":
            quote = source[i]
            end = i + 1
            while end < n:
                if source[end] == "\\":
                    end += 2
                elif source[end] == quote:
                    end += 1
                    break
                else:
                    end += 1
            end = min(end, n)
        else:
            out.append(source[i])
            i += 1
            continue
        out.append("".join(c if c == "\n" else " " for c in source[i:end]))
        i = end
    return "".join(out)


def first_party_cmake_files(repo):
    paths = []
    for root, dirs, files in os.walk(repo):
        dirs[:] = sorted(
            d for d in dirs
            if d not in EXCLUDED_CMAKE_DIRS and d != "build"
            and not d.startswith(("build-", "build_", "cmake-build-"))
        )
        if "CMakeLists.txt" in files:
            paths.append(Path(root) / "CMakeLists.txt")
    return sorted(paths)


def cmake_code(path):
    return "\n".join(line.split("#", 1)[0] for line in path.read_text(encoding="utf-8").splitlines())


def scan_cmake_sources(repo):
    result = {}
    for path in first_party_cmake_files(repo):
        code = cmake_code(path)
        counts = {}
        for source in SOURCE_FILES:
            pattern = r"(?<![A-Za-z0-9_])" + re.escape(source) + r"(?![A-Za-z0-9_.])"
            count = len(re.findall(pattern, code))
            if count:
                counts[source] = count
        if counts:
            result[path.relative_to(repo).as_posix()] = counts
    return result


def scan_raw_globals(repo):
    result = {}
    owners = set(SOURCE_FILES)
    patterns = {name: re.compile(r"\b" + re.escape(name) + r"\b") for name in RAW_GLOBALS}
    paths = set()
    for directory in (repo / "src", repo / "include"):
        if not directory.exists():
            continue
        paths.update(p for p in directory.rglob("*") if p.suffix in SOURCE_SUFFIXES | HEADER_SUFFIXES)
    for path in sorted(paths):
        if not path.is_file():
            continue
        rel = path.relative_to(repo).as_posix()
        if rel in owners:
            continue
        code = mask_comments_and_literals(path.read_text(encoding="utf-8", errors="replace"))
        counts = {name: len(rx.findall(code)) for name, rx in patterns.items()}
        counts = {name: count for name, count in counts.items() if count}
        if counts:
            result[rel] = counts
    return result


def scan(repo):
    repo = Path(repo)
    return {
        "cmake_sources": scan_cmake_sources(repo),
        "raw_globals": scan_raw_globals(repo),
    }


def regressions(current, baseline):
    """Return additions above the baseline."""
    added = []
    for section in ("cmake_sources", "raw_globals"):
        old = baseline.get(section, {})
        for path, symbols in current[section].items():
            for name, count in symbols.items():
                before = old.get(path, {}).get(name, 0)
                if count > before:
                    added.append((section, path, name, before, count))
    return sorted(added)


def stale_baseline(current, baseline):
    """Return reductions that need recording in the same commit."""
    stale = []
    for section in ("cmake_sources", "raw_globals"):
        now = current[section]
        for path, symbols in baseline.get(section, {}).items():
            for name, before in symbols.items():
                count = now.get(path, {}).get(name, 0)
                if count < before:
                    stale.append((section, path, name, before, count))
    return sorted(stale)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=HERE.parents[2])
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    parser.add_argument("--write-baseline", action="store_true")
    args = parser.parse_args(argv)
    current = scan(args.repo)
    if args.write_baseline:
        args.baseline.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"GRAPH OWNER baseline written: {args.baseline}")
        return 0
    try:
        baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"GRAPH OWNER ERROR: cannot read baseline: {exc}", file=sys.stderr)
        return 2
    added = regressions(current, baseline)
    stale = stale_baseline(current, baseline)
    for section, path, name, before, count in added:
        print(f"GRAPH OWNER REGRESSION [{section}] {path}: {name} {before} -> {count}")
    for section, path, name, before, count in stale:
        print(f"GRAPH OWNER STALE BASELINE [{section}] {path}: {name} {before} -> {count} "
              "(record the reduction with --write-baseline in the same commit)")
    if added or stale:
        print(f"GRAPH OWNER FAIL ({len(added)} regression row(s), {len(stale)} stale row(s))")
        return 1
    compiles = sum(sum(sources.values()) for sources in current["cmake_sources"].values())
    references = sum(sum(symbols.values()) for symbols in current["raw_globals"].values())
    print(f"GRAPH OWNER PASS ({compiles} listed compiles, {references} source references)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

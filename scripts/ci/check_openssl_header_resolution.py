#!/usr/bin/env python3
"""Verify the OpenSSL header selected by a real production compile command."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"OPENSSL HEADER RESOLUTION FAILED: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_cache_value(cache: Path, key: str) -> str:
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix):
            return line.split("=", 1)[1]
    fail(f"{key} is absent from {cache}")


def compile_tokens(entry: dict[str, object]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(
        isinstance(argument, str) for argument in arguments
    ):
        return list(arguments)
    command = entry.get("command")
    if isinstance(command, str):
        return shlex.split(command)
    fail("compile_commands entry has neither arguments nor command")


def include_resolution_flags(tokens: list[str]) -> list[str]:
    """Retain flags that affect preprocessor include resolution."""
    result: list[str] = []
    separate_argument_flags = {
        "-I",
        "-isystem",
        "-iquote",
        "-idirafter",
        "-F",
        "-iframework",
        "-isysroot",
        "--sysroot",
        "-target",
        "-arch",
    }
    joined_prefixes = (
        "-I",
        "-iquote",
        "-idirafter",
        "-F",
        "-iframework",
        "--sysroot=",
        "-target=",
    )

    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in separate_argument_flags:
            if index + 1 >= len(tokens):
                fail(f"compile command ends after {token}")
            result.extend((token, tokens[index + 1]))
            index += 2
            continue
        if token.startswith(joined_prefixes):
            result.append(token)
        index += 1
    return result


def run_preprocessor(
    compiler: str,
    flags: list[str],
    mode: str,
) -> str:
    completed = subprocess.run(
        [compiler, *flags, *mode.split(), "-x", "c++", "-"],
        input="#include <openssl/opensslv.h>\n",
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        fail(
            "preprocessor probe failed:\n"
            + completed.stdout
            + completed.stderr
        )
    return completed.stdout


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("expected_version")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    cache = build_dir / "CMakeCache.txt"
    commands_file = build_dir / "compile_commands.json"
    if not cache.is_file():
        fail(f"missing {cache}")
    if not commands_file.is_file():
        fail(
            f"missing {commands_file}; configure with "
            "CMAKE_EXPORT_COMPILE_COMMANDS enabled"
        )

    entries = json.loads(commands_file.read_text(encoding="utf-8"))
    entry = next(
        (
            item
            for item in entries
            if str(item.get("file", "")).endswith(
                "src/consensus/shielded/binding_sig.cpp"
            )
        ),
        None,
    )
    if entry is None:
        fail(
            "no production compile command found for "
            "src/consensus/shielded/binding_sig.cpp"
        )

    compiler = read_cache_value(cache, "CMAKE_CXX_COMPILER")
    flags = include_resolution_flags(compile_tokens(entry))
    macros = run_preprocessor(compiler, flags, "-dM -E")
    dependencies = run_preprocessor(compiler, flags, "-M")

    version_match = re.search(
        r'^#define OPENSSL_FULL_VERSION_STR "([^"]+)"$',
        macros,
        re.MULTILINE,
    )
    if version_match is None:
        version_match = re.search(
            r'^#define OPENSSL_VERSION_TEXT "OpenSSL ([^" ]+)',
            macros,
            re.MULTILINE,
        )
    if version_match is None:
        fail("resolved opensslv.h did not expose an OpenSSL version macro")

    header_matches = re.findall(
        r"(\S*openssl/opensslv\.h)",
        dependencies.replace("\\\n", " "),
    )
    resolved_header = header_matches[-1] if header_matches else "<unknown>"
    actual_version = version_match.group(1)
    print(
        "compiler header resolution: "
        f"{resolved_header} declares OpenSSL {actual_version}"
    )
    if actual_version != args.expected_version:
        fail(
            f"production compile resolves OpenSSL {actual_version} at "
            f"{resolved_header}; expected {args.expected_version}"
        )


if __name__ == "__main__":
    main()

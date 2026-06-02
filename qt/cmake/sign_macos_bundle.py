#!/usr/bin/env python3

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
from collections import deque
from pathlib import Path
from typing import Optional


def should_sign_file(path: Path) -> bool:
    if not path.is_file() or path.is_symlink():
        return False

    path_str = str(path)
    if "/Contents/MacOS/" in path_str:
        return True
    if "/Contents/PlugIns/" in path_str:
        return True
    if path.suffix in {".dylib", ".so"}:
        return True
    if ".framework/" in path_str:
        return True
    return os.access(path, os.X_OK)


def codesign(path: Path, identity: str, entitlements: Optional[str] = None, runtime: bool = False) -> None:
    cmd = ["codesign", "--force", "--sign", identity]
    if identity == "-":
        cmd.append("--timestamp=none")
    else:
        cmd.append("--timestamp")
    if runtime:
        cmd.extend(["--options", "runtime"])
    if entitlements:
        cmd.extend(["--entitlements", entitlements])
    cmd.append(str(path))
    subprocess.run(cmd, check=True)


def ensure_main_executable(app: Path) -> None:
    info_plist = app / "Contents" / "Info.plist"
    with info_plist.open("rb") as fh:
        plist = plistlib.load(fh)

    executable_name = plist.get("CFBundleExecutable")
    if not executable_name:
        raise SystemExit(f"CFBundleExecutable missing from {info_plist}")

    executable_path = app / "Contents" / "MacOS" / executable_name
    backup_path = executable_path.with_name(executable_name + ".bak")

    if not executable_path.exists() and backup_path.exists():
        executable_path.write_bytes(backup_path.read_bytes())

    if not executable_path.exists():
        raise SystemExit(f"Bundle executable missing: {executable_path}")

    executable_path.chmod(executable_path.stat().st_mode | 0o111)


def otool_deps(path: Path) -> list[str]:
    try:
        out = subprocess.check_output(["otool", "-L", str(path)], stderr=subprocess.DEVNULL, text=True)
    except subprocess.CalledProcessError:
        return []
    except Exception:
        return []

    deps = [line.strip().split(" (")[0] for line in out.splitlines()[1:]]
    if deps and os.path.basename(deps[0]) == path.name:
        deps = deps[1:]
    return deps


def is_external_dep(dep: str) -> bool:
    return dep.startswith("/opt/homebrew/") or dep.startswith("/usr/local/")


# Brew cellars to search when resolving @rpath/@loader_path refs that the
# consumer's rpaths don't cover (grpc ships libupb_*/libaddress_sorting via
# @rpath, relying on the caller's LC_RPATH list).
_RPATH_BREW_SEARCH = [
    "/opt/homebrew/Cellar/grpc",
    "/opt/homebrew/Cellar/abseil",
    "/opt/homebrew/Cellar/protobuf",
    "/opt/homebrew/lib",
]


def _resolve_rpath_dep(name: str) -> Optional[Path]:
    for base in _RPATH_BREW_SEARCH:
        base_path = Path(base)
        if not base_path.exists():
            continue
        if base_path.name == "lib":
            candidate = base_path / name
            if candidate.exists():
                return candidate.resolve()
            continue
        for cellar in base_path.iterdir():
            lib_dir = cellar / "lib"
            if not lib_dir.is_dir():
                continue
            candidate = lib_dir / name
            if candidate.exists():
                return candidate.resolve()
    return None


def bundle_external_deps(app: Path) -> None:
    contents = app / "Contents"
    frameworks_dir = contents / "Frameworks"
    frameworks_dir.mkdir(parents=True, exist_ok=True)

    queue: deque[Path] = deque()
    seen: set[Path] = set()
    copied: set[Path] = set()

    for root, _, files in os.walk(contents):
        for file_name in files:
            candidate = Path(root) / file_name
            if should_sign_file(candidate):
                queue.append(candidate)

    while queue:
        consumer = queue.popleft()
        if consumer in seen or not consumer.exists():
            continue
        seen.add(consumer)

        for dep in otool_deps(consumer):
            resolved: Optional[Path] = None

            if is_external_dep(dep):
                source = Path(dep)
                if source.exists():
                    resolved = source.resolve()
                    bundled = frameworks_dir / source.name
                else:
                    continue
            elif dep.startswith(("@rpath/", "@loader_path/")):
                name = os.path.basename(dep)
                bundled = frameworks_dir / name
                if bundled.exists():
                    queue.append(bundled)
                    continue
                resolved = _resolve_rpath_dep(name)
                if resolved is None:
                    continue
            else:
                continue

            if bundled not in copied and not bundled.exists():
                shutil.copy2(resolved, bundled)
                bundled.chmod(bundled.stat().st_mode | 0o200)
                subprocess.run(
                    ["install_name_tool", "-id", f"@rpath/{bundled.name}", str(bundled)],
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                copied.add(bundled)

            queue.append(bundled)


def rewrite_bundle_deps(app: Path) -> None:
    contents = app / "Contents"
    frameworks_dir = contents / "Frameworks"
    if not frameworks_dir.exists():
        return

    bundle_targets: dict[str, Path] = {}
    for root, _, files in os.walk(frameworks_dir):
        for file_name in files:
            candidate = Path(root) / file_name
            bundle_targets.setdefault(candidate.name, candidate)

    for root, _, files in os.walk(contents):
        for file_name in files:
            consumer = Path(root) / file_name
            deps = otool_deps(consumer)
            for dep in deps:
                if not (dep.startswith("/opt/homebrew/") or dep.startswith("/usr/local/")):
                    continue

                replacement_target = bundle_targets.get(os.path.basename(dep))
                if not replacement_target:
                    continue

                relative_target = os.path.relpath(replacement_target, consumer.parent)
                rewritten = "@loader_path/" + relative_target
                subprocess.run(
                    ["install_name_tool", "-change", dep, rewritten, str(consumer)],
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )


_STALE_BREW_RPATH_PREFIXES = ("/opt/homebrew/", "/usr/local/")


def _otool_rpaths(path: Path) -> list[str]:
    try:
        out = subprocess.check_output(["otool", "-l", str(path)], stderr=subprocess.DEVNULL, text=True)
    except Exception:
        return []
    rpaths: list[str] = []
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "cmd LC_RPATH" in line:
            for j in range(i, min(i + 4, len(lines))):
                stripped = lines[j].strip()
                if stripped.startswith("path "):
                    rpaths.append(stripped[5:].split(" (offset")[0])
                    break
    return rpaths


def _install_name_tool_silent(*args: str) -> None:
    subprocess.run(
        ["install_name_tool", *args],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def fix_rpaths(app: Path) -> None:
    """Ensure bundled binaries can find their @rpath/* deps inside the bundle.

    - Binaries in Contents/Resources (e.g., dinerod) get @loader_path/../Frameworks
    - Dylibs in Contents/Frameworks get @loader_path (so they see siblings)
    - Stale absolute Homebrew rpaths are stripped so hardened-runtime doesn't
      try to load signed-by-another-team libs.
    """
    contents = app / "Contents"
    for root, _, files in os.walk(contents):
        for file_name in files:
            candidate = Path(root) / file_name
            if not should_sign_file(candidate):
                continue
            candidate.chmod(candidate.stat().st_mode | 0o200)
            existing = _otool_rpaths(candidate)

            desired: list[str] = []
            if "/Contents/Frameworks/" in str(candidate) or str(candidate.parent).endswith("/Frameworks"):
                desired.append("@loader_path")
            elif "/Contents/Resources/" in str(candidate) or "/Contents/MacOS/" in str(candidate):
                desired.append("@loader_path/../Frameworks")

            for rp in desired:
                if rp not in existing:
                    _install_name_tool_silent("-add_rpath", rp, str(candidate))

            for rp in existing:
                if any(rp.startswith(p) for p in _STALE_BREW_RPATH_PREFIXES):
                    _install_name_tool_silent("-delete_rpath", rp, str(candidate))


def main() -> int:
    parser = argparse.ArgumentParser(description="Inside-out codesign for a macOS app bundle.")
    parser.add_argument("app_bundle", help="Path to .app bundle")
    parser.add_argument("--identity", default="-", help="codesign identity; '-' for ad-hoc")
    parser.add_argument("--entitlements", help="Optional entitlements plist for the top-level app")
    parser.add_argument(
        "--runtime",
        action="store_true",
        help="Enable hardened runtime on the app and every nested Mach-O",
    )
    args = parser.parse_args()

    app = Path(args.app_bundle).resolve()
    if app.suffix != ".app" or not app.is_dir():
        raise SystemExit(f"Not an app bundle: {app}")

    ensure_main_executable(app)
    bundle_external_deps(app)
    rewrite_bundle_deps(app)
    fix_rpaths(app)

    contents = app / "Contents"

    # The CFBundleExecutable is signed LAST via codesign(app, ...) below. Signing
    # it seals the whole bundle and requires every nested Mach-O — including the
    # sibling helper executables in Contents/MacOS/ — to already be signed. It
    # lives at the same path depth as those helpers, so the depth sort below does
    # NOT guarantee it is signed after them; whichever same-depth file os.walk
    # yields last wins (arm64 happened to order it last; x86_64 didn't). Exclude
    # it here so the final codesign(app) seals a fully-signed bundle.
    main_exe: Optional[Path] = None
    try:
        with (contents / "Info.plist").open("rb") as fh:
            _exe_name = plistlib.load(fh).get("CFBundleExecutable")
        if _exe_name:
            main_exe = (contents / "MacOS" / _exe_name).resolve()
    except Exception:
        main_exe = None

    sign_targets: list[Path] = []
    for root, _, files in os.walk(contents):
        for file_name in files:
            candidate = Path(root) / file_name
            if should_sign_file(candidate) and (main_exe is None or candidate.resolve() != main_exe):
                sign_targets.append(candidate)

    sign_targets.sort(key=lambda p: len(p.parts), reverse=True)
    for target in sign_targets:
        codesign(target, args.identity, runtime=args.runtime)

    codesign(app, args.identity, entitlements=args.entitlements, runtime=args.runtime)
    return 0


if __name__ == "__main__":
    sys.exit(main())

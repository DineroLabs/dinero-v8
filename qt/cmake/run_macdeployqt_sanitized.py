#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path


def hide_path(path: Path, hidden: list[tuple[Path, Path]]) -> None:
    if not path.exists() and not path.is_symlink():
        return
    shadow = path.with_name(path.name + ".dineroqt-hidden")
    if shadow.exists() or shadow.is_symlink():
        if shadow.is_dir() and not shadow.is_symlink():
            for child in shadow.iterdir():
                if child.is_dir():
                    # best-effort cleanup for stale temp dirs from interrupted runs
                    pass
        if shadow.is_dir() and not shadow.is_symlink():
            import shutil
            shutil.rmtree(shadow)
        else:
            shadow.unlink()
    path.rename(shadow)
    hidden.append((path, shadow))


def restore(hidden: list[tuple[Path, Path]]) -> None:
    for original, shadow in reversed(hidden):
        try:
            if shadow.exists() or shadow.is_symlink():
                shadow.rename(original)
        except Exception:
            pass


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: run_macdeployqt_sanitized.py <macdeployqt> <app_bundle>", file=sys.stderr)
        return 2

    macdeployqt = Path(sys.argv[1])
    app_bundle = Path(sys.argv[2])

    plugin_root = Path("/opt/homebrew/share/qt/plugins")
    candidates = [
        plugin_root / "imageformats" / "libqpdf.dylib",
        plugin_root / "imageformats" / "libqsvg.dylib",
        plugin_root / "iconengines",
        plugin_root / "platforminputcontexts",
    ]

    hidden: list[tuple[Path, Path]] = []
    try:
        for candidate in candidates:
            hide_path(candidate, hidden)
        completed = subprocess.run(
            # Signing is deliberately owned by sign_macos_bundle.py after all
            # dependency rewriting/removal is complete. Letting macdeployqt
            # ad-hoc sign here produces a transient (and alarming) nested
            # Brotli verification failure before the final inside-out pass.
            [str(macdeployqt), str(app_bundle), "-always-overwrite", "-no-codesign"],
            check=False,
        )
        return completed.returncode
    finally:
        restore(hidden)


if __name__ == "__main__":
    sys.exit(main())

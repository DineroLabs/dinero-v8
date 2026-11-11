"""Exercise the legacy Apple archive assembler without rebuilding SDK slices."""
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[4]


def assemble(tmp_path, *, with_mempool):
    source = (ROOT / "build_nodecore_xcframework.sh").read_text()
    start = source.index("create_fat_lib() {")
    end = source.index("\nDEVICE_FAT=", start)
    functions = tmp_path / "archive-functions.sh"
    functions.write_text(source[start:end])
    build = tmp_path / "build"
    build.mkdir()
    (build / "libnodecore_ffi.a").write_bytes(b"ffi")
    if with_mempool:
        (build / "libdinero_mempool.a").write_bytes(b"pool")
    tools = tmp_path / "tools"
    tools.mkdir()
    fake_libtool = tools / "libtool"
    fake_libtool.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$LIBTOOL_ARGS\"\n"
                            "while [ \"$1\" != '-o' ]; do shift; done\n"
                            "shift\n: > \"$1\"\n")
    fake_libtool.chmod(0o755)
    args = tmp_path / "libtool-args"
    env = dict(os.environ, PATH=f"{tools}:{os.environ['PATH']}", LIBTOOL_ARGS=str(args))
    result = subprocess.run(
        ["bash", "-c", 'source "$1"; create_fat_lib test "$2" "$3" "$4"',
         "bash", str(functions), str(build), str(tmp_path / "fat.a"), str(tmp_path / "openssl")],
        env=env, capture_output=True, text=True)
    return result, args.read_text().splitlines() if args.exists() else []


def test_legacy_apple_assembler_includes_mempool_archive(tmp_path):
    result, args = assemble(tmp_path, with_mempool=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert any(arg.endswith("/libdinero_mempool.a") for arg in args)


def test_legacy_apple_assembler_refuses_missing_mempool_archive(tmp_path):
    result, args = assemble(tmp_path, with_mempool=False)
    assert result.returncode != 0
    assert "libdinero_mempool.a" in result.stdout + result.stderr
    assert not args

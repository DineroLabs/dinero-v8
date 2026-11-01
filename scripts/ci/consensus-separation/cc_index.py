"""Shared reader for compile_commands.json: which CMake target compiles which source."""
import json, os, re, sys

_TARGET = re.compile(r"CMakeFiles/([^/]+)\.dir/")


class InputError(Exception):
    pass


def load(build_dir, src_root):
    """Return [(relative_source, target, entry)] for every compile entry. Raises InputError on bad input."""
    path = os.path.join(build_dir, "compile_commands.json")
    try:
        entries = json.load(open(path))
    except (OSError, ValueError) as exc:
        raise InputError("cannot read %s: %s" % (path, exc))
    if not entries:
        raise InputError("no compile entries in %s" % path)
    root = os.path.realpath(src_root)
    out = []
    for e in entries:
        m = _TARGET.search(e.get("output", "")) or _TARGET.search(e.get("command", ""))
        if not m:
            raise InputError("no CMake target dir for %s" % e.get("file"))
        src = os.path.realpath(os.path.join(e["directory"], e["file"]))
        rel = os.path.relpath(src, root)
        out.append((rel, m.group(1), e))
    return out


def is_library_target(name):
    """Production library targets: dinero_* but not dinero_test_*."""
    return name.startswith("dinero_") and not name.startswith("dinero_test_")


def fail_input(exc):
    sys.stderr.write("ERROR: %s\n" % exc)
    sys.exit(2)

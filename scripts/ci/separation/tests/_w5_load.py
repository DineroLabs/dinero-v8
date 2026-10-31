"""Load the W5 scripts (tests/separation/*.py) as modules for unit tests.

SEP_W5_SCRIPTS_DIR overrides the directory, so the tests can be pointed at a
neutered copy to show they fail.
"""
import importlib.util
import os
from pathlib import Path

DEFAULT = Path(__file__).resolve().parents[4] / "tests" / "separation"


def load(name):
    base = Path(os.environ.get("SEP_W5_SCRIPTS_DIR", DEFAULT))
    spec = importlib.util.spec_from_file_location("w5_" + name, base / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

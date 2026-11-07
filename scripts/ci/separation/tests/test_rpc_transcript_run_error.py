"""A node that exits during startup must leave its log in the error output."""
import os
import re
import stat
import sys

from _w5_load import load

rt = load("rpc_transcript")

FAKE = """#!%s
import os, sys
datadir = [a.split("=", 1)[1] for a in sys.argv if a.startswith("--datadir=")][0]
for i in range(100):
    print("fake dinerod output line %%d" %% i)
print("Error: fake startup failure")
os.makedirs(os.path.join(datadir, "regtest"), exist_ok=True)
with open(os.path.join(datadir, "regtest", "dinero.log"), "w") as f:
    f.write("dinero.log: refusing to start\\n")
sys.exit(1)
"""


def test_startup_exit_reports_log_path_and_tail(tmp_path, capsys):
    fake = tmp_path / "dinerod"
    fake.write_text(FAKE % sys.executable)
    fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    base = tmp_path / "off.json"
    base.write_text('{"config": "off", "transcript": []}\n')
    rc = rt.main(["check", "--dinerod", str(fake), "--config", "off", "--out", str(base)])
    err = capsys.readouterr().err
    assert rc == 2
    assert "rpc_transcript: RUN ERROR: a exited rc=1 during startup\n" in err
    m = re.search(r"a log \(stdout/stderr\) (\S+a\.1\.log) \(last 60 of 101 lines\):", err)
    assert m, err
    assert os.path.isfile(m.group(1))  # the work dir is kept on failure
    assert "  | Error: fake startup failure" in err
    assert "  | fake dinerod output line 41\n" in err
    assert "fake dinerod output line 40\n" not in err  # only the last 60 lines
    assert re.search(r"a dinero\.log \S+dinero\.log \(last 1 of 1 lines\):\n  \| dinero.log: refusing to start", err)
    assert "rpc_transcript: work dir kept at " in err

import json
import os
import tempfile
import unittest
from unittest import mock

from watcher import main

VALID_NODES = [
    {"name": "a", "role": "voting", "transport": "ssh", "target": "w@a"},
    {"name": "b", "role": "voting", "transport": "local", "target": "127.0.0.1:1"},
]


class TestMainCredentialGate(unittest.TestCase):
    def _write_config(self, db_path):
        payload = {"nodes": VALID_NODES, "db_path": db_path}
        fd, path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as handle:
            json.dump(payload, handle)
        self.addCleanup(os.unlink, path)
        return path

    def test_missing_credentials_exit_2_before_touching_persistent_state(self):
        """The credential gate must run before Store() opens the database.

        Store() creates the sqlite file (and WAL) as a side effect of
        construction, so a `db_path` whose parent directory does not exist
        would raise inside Store() and mask the real problem — no
        PUSHOVER_TOKEN/PUSHOVER_USER — with an unrelated OperationalError.
        A watcher that starts without a notifier would page nobody, and
        that must be caught even before any state directory exists (that
        directory is provisioned by the systemd unit, not by this
        process). This test fails if the credential check is ever moved
        back after Store(config.db_path).
        """
        unwritable_db_path = "/nonexistent-fleet-watcher-test-dir/watcher.db"
        config_path = self._write_config(unwritable_db_path)

        env = dict(os.environ)
        env.pop("PUSHOVER_TOKEN", None)
        env.pop("PUSHOVER_USER", None)
        with mock.patch.dict(os.environ, env, clear=True):
            exit_code = main(["--config", config_path, "--once"])

        self.assertEqual(exit_code, 2)


if __name__ == "__main__":
    unittest.main()

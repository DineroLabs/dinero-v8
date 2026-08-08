import json
import os
import tempfile
import unittest
from unittest import mock

import watcher
from config import Config
from engine import Engine
from store import Store
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


class TestSecret(unittest.TestCase):
    """The unit's LoadCredential comment claims secrets never touch the
    process environment. That claim is only true if _secret actually reads
    $CREDENTIALS_DIRECTORY first — this proves the wiring, not just the
    environment fallback the older test already covered."""

    def test_reads_from_credentials_directory_before_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            with open(os.path.join(directory, "pushover"), "w",
                      encoding="utf-8") as handle:
                handle.write("PUSHOVER_TOKEN=from-credential\n")
                handle.write("PUSHOVER_USER=from-credential-user\n")

            env = dict(os.environ)
            env["CREDENTIALS_DIRECTORY"] = directory
            env["PUSHOVER_TOKEN"] = "from-environment"  # must be ignored
            with mock.patch.dict(os.environ, env, clear=True):
                self.assertEqual(
                    watcher._secret("PUSHOVER_TOKEN", "pushover"),
                    "from-credential")
                self.assertEqual(
                    watcher._secret("PUSHOVER_USER", "pushover"),
                    "from-credential-user")

    def test_falls_back_to_environment_when_no_credentials_directory(self):
        env = dict(os.environ)
        env.pop("CREDENTIALS_DIRECTORY", None)
        env["HEARTBEAT_URL"] = "https://example.invalid/hb"
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(
                watcher._secret("HEARTBEAT_URL", "heartbeat"),
                "https://example.invalid/hb")

    def test_missing_key_in_credential_file_falls_back_to_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            with open(os.path.join(directory, "heartbeat"), "w",
                      encoding="utf-8") as handle:
                handle.write("UNRELATED_KEY=x\n")

            env = dict(os.environ)
            env["CREDENTIALS_DIRECTORY"] = directory
            env["HEARTBEAT_URL"] = "https://example.invalid/hb"
            with mock.patch.dict(os.environ, env, clear=True):
                self.assertEqual(
                    watcher._secret("HEARTBEAT_URL", "heartbeat"),
                    "https://example.invalid/hb")


class _Rpc:
    def call(self, node, method, params=None):
        if method == "getdaemonstatus":
            return {"result": {"height": 100}}
        if method == "blockchain.getbestblockhash":
            return {"result": "H100"}
        if method == "blockchain.getblockhash":
            return {"result": "H100"}
        return {"result": {}}

    def restart_id(self, node):
        return "r1"


class _Notifier:
    def send(self, title, message, priority):
        return True


class _Heartbeat:
    def __init__(self):
        self.pings = 0

    def ping(self):
        self.pings += 1
        return True


class TestRunOnce(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.addCleanup(os.unlink, self.path)
        self.store = Store(self.path)
        self.engine = Engine(self.store, open_after=3, close_after=3)
        self.config = Config(
            nodes=[{"name": "a", "role": "voting", "transport": "ssh", "target": "t"}],
            db_path=self.path, cycle_seconds=60, open_after=3, close_after=3,
            node_behind_blocks=10, overdue_deadline_seconds=300.0)

    def _run(self, previous=None):
        hb = _Heartbeat()
        result = watcher.run_once(self.config, self.store, self.engine, _Rpc(),
                                  _Notifier(), hb, previous)
        return result, hb

    def test_a_healthy_cycle_pings_once(self):
        _, hb = self._run()
        self.assertEqual(hb.pings, 1)

    def test_a_failed_commit_does_not_ping(self):
        """A watcher that polls but cannot persist is broken, and the dead-man
        is exactly what should notice."""
        def explode(_observations):
            raise RuntimeError("disk full")
        self.store.write_cycle = explode
        _, hb = self._run()
        self.assertEqual(hb.pings, 0)

    def test_a_failed_commit_preserves_the_previous_cycle(self):
        sentinel = ["previous-cycle"]
        def explode(_observations):
            raise RuntimeError("disk full")
        self.store.write_cycle = explode
        result, _ = self._run(previous=sentinel)
        self.assertIs(result, sentinel)

    def test_a_dead_delivery_worker_does_not_ping(self):
        """Collection working is not the property worth monitoring; the
        ability to raise an alarm is."""
        original = watcher.drain
        watcher.drain = lambda *a, **k: (_ for _ in ()).throw(RuntimeError("boom"))
        self.addCleanup(setattr, watcher, "drain", original)
        _, hb = self._run()
        self.assertEqual(hb.pings, 0)

    def test_the_configured_node_behind_threshold_reaches_evaluate(self):
        """config.node_behind_blocks was dead config until this round:
        rules.evaluate() hard-coded 10, so an operator setting 50 would
        still be paged at 10. evaluate() now takes it as a keyword with the
        constant as default — this proves run_once actually forwards
        config.node_behind_blocks rather than relying on that default."""
        seen = {}
        original = watcher.evaluate

        def spy(*args, **kwargs):
            seen.update(kwargs)
            return original(*args, **kwargs)

        watcher.evaluate = spy
        self.addCleanup(setattr, watcher, "evaluate", original)

        self.config = Config(**{**self.config.__dict__, "node_behind_blocks": 50})
        self._run()

        self.assertEqual(seen.get("node_behind_blocks"), 50)


if __name__ == "__main__":
    unittest.main()

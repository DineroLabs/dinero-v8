import json
import os
import tempfile
import unittest

from config import load_config

VALID = {
    "nodes": [
        {"name": "a", "role": "voting", "transport": "ssh", "target": "w@a"},
        {"name": "b", "role": "voting", "transport": "local", "target": "127.0.0.1:1"},
        {"name": "w", "role": "observer", "transport": "ssh", "target": "w@w"},
    ]
}


class TestLoadConfig(unittest.TestCase):
    def _write(self, payload):
        fd, path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as handle:
            json.dump(payload, handle)
        self.addCleanup(os.unlink, path)
        return path

    def _rejects(self, payload, fragment):
        with self.assertRaises(ValueError) as caught:
            load_config(self._write(payload))
        self.assertIn(fragment, str(caught.exception).lower())

    def test_a_valid_config_loads_and_counts_voters(self):
        config = load_config(self._write(VALID))
        self.assertEqual(len(config.nodes), 3)
        self.assertEqual(config.voting_total, 2)

    def test_duplicate_node_names_are_rejected(self):
        payload = {"nodes": [dict(VALID["nodes"][0]), dict(VALID["nodes"][0])]}
        self._rejects(payload, "duplicate")

    def test_an_empty_node_list_is_rejected(self):
        self._rejects({"nodes": []}, "non-empty")

    def test_a_config_with_no_voting_nodes_is_rejected(self):
        payload = {"nodes": [dict(VALID["nodes"][2])]}
        self._rejects(payload, "voting")

    def test_a_single_voter_config_is_rejected(self):
        """One voter can never reach QUORUM_MIN, so consensus_health would page
        every three cycles forever on a healthy fleet."""
        payload = {"nodes": [dict(VALID["nodes"][0]), dict(VALID["nodes"][2])]}
        self._rejects(payload, "at least 2")

    def test_an_unknown_transport_is_rejected(self):
        payload = {"nodes": [{**VALID["nodes"][0], "transport": "carrier-pigeon"}]}
        self._rejects(payload, "transport")

    def test_an_unknown_role_is_rejected(self):
        payload = {"nodes": [{**VALID["nodes"][0], "role": "auditor"}]}
        self._rejects(payload, "role")

    def test_a_missing_required_key_is_rejected(self):
        payload = {"nodes": [{"name": "a", "role": "voting"}]}
        self._rejects(payload, "missing")

    def test_remediation_must_target_a_local_voting_node(self):
        nodes = VALID["nodes"] + [
            {"name": "c", "role": "voting", "transport": "ssh", "target": "w@c"}]
        payload = {"nodes": nodes, "remediation": {
            "local_node": "a",
            "request_path": "/run/fleet-watcher/restart-dinero.json"}}
        self._rejects(payload, "local voting")

    def test_valid_remediation_config_is_loaded(self):
        nodes = VALID["nodes"] + [
            {"name": "c", "role": "voting", "transport": "ssh", "target": "w@c"}]
        payload = {"nodes": nodes, "remediation": {
            "local_node": "b",
            "request_path": "/run/fleet-watcher/restart-dinero.json"}}
        config = load_config(self._write(payload))
        self.assertEqual(config.remediation_node, "b")
        self.assertEqual(config.remediation_request_path,
                         "/run/fleet-watcher/restart-dinero.json")

    def test_remediation_requires_three_voters(self):
        payload = {**VALID, "remediation": {
            "local_node": "b",
            "request_path": "/run/fleet-watcher/restart-dinero.json"}}
        self._rejects(payload, "at least 3")

    def test_remediation_request_path_is_fixed(self):
        nodes = VALID["nodes"] + [
            {"name": "c", "role": "voting", "transport": "ssh", "target": "w@c"}]
        payload = {"nodes": nodes, "remediation": {
            "local_node": "b", "request_path": "/tmp/restart.json"}}
        self._rejects(payload, "request_path")


if __name__ == "__main__":
    unittest.main()

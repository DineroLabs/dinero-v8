"""The node-side forced command.

These tests run the real script as a subprocess against a stub RPC server, so
what is under test is the artifact that gets installed on every node — not a
reimplementation of its logic in Python.

The property that matters is NEGATIVE: a denied method must never reach the
daemon. Asserting only that permitted calls succeed would pass just as happily
against a wrapper with no allowlist at all, which is exactly the state this
suite exists to prevent regressing to.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

WRAPPER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "deploy", "fleet-watcher-rpc")

received: list = []


class _Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        received.append(json.loads(self.rfile.read(length).decode()))
        body = json.dumps({"jsonrpc": "2.0", "id": "w", "result": {"ok": True}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


class WrapperTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = HTTPServer(("127.0.0.1", 0), _Handler)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        received.clear()

    def run_wrapper(self, body, command="rpc", port=None):
        env = dict(os.environ)
        env["SSH_ORIGINAL_COMMAND"] = command
        env["DINERO_RPC_PORT"] = str(self.port if port is None else port)
        payload = body if isinstance(body, bytes) else json.dumps(body).encode()
        return subprocess.run([sys.executable, WRAPPER], input=payload,
                              capture_output=True, env=env, timeout=20)

    # ---- permitted ----------------------------------------------------
    def test_an_allowed_method_reaches_the_node(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "getdaemonstatus", "params": []})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["result"], {"ok": True})
        self.assertEqual([r["method"] for r in received], ["getdaemonstatus"])

    def test_getblockhash_passes_its_height(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "blockchain.getblockhash",
                                   "params": [1234]})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(received[0]["params"], [1234])

    # ---- denied: the whole point --------------------------------------
    def test_a_method_outside_the_allowlist_never_reaches_the_node(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "wallet.dumpprivkey", "params": []})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [], "a denied method was forwarded to the daemon")

    def test_a_write_method_is_denied(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "stop", "params": []})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    def test_a_duplicate_method_key_cannot_smuggle_a_denied_call(self):
        # json.loads keeps the LAST duplicate key. A wrapper that grepped the
        # body for an allowed name would see 'getdaemonstatus' and forward a
        # body the daemon reads as 'wallet.dumpprivkey'.
        raw = (b'{"jsonrpc":"2.0","id":"w","method":"getdaemonstatus",'
               b'"method":"wallet.dumpprivkey","params":[]}')
        result = self.run_wrapper(raw)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    def test_a_unicode_escaped_method_name_is_denied(self):
        # "stop" decodes to "stop". Text matching on the raw bytes misses it.
        raw = b'{"jsonrpc":"2.0","id":"w","method":"\\u0073top","params":[]}'
        result = self.run_wrapper(raw)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    def test_an_allowed_name_nested_in_a_string_does_not_authorise(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "getdaemonstatus",
                                   "method": "wallet.dumpprivkey", "params": []})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    def test_a_batch_request_is_denied(self):
        result = self.run_wrapper([{"jsonrpc": "2.0", "id": "w",
                                    "method": "getdaemonstatus", "params": []}])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    def test_malformed_json_is_denied(self):
        result = self.run_wrapper(b'{"method": "getdaemonstatus"')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    def test_an_oversized_body_is_denied(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "getdaemonstatus",
                                   "params": [], "pad": "x" * 9000})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    # ---- parameter shape ----------------------------------------------
    def test_getblockhash_rejects_a_non_integer_height(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "blockchain.getblockhash",
                                   "params": ["../../etc/passwd"]})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    def test_getblockhash_rejects_a_boolean_height(self):
        # bool is a subclass of int; True would otherwise pass as height 1.
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "blockchain.getblockhash",
                                   "params": [True]})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    def test_a_method_taking_no_params_rejects_extra_ones(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "node.status", "params": [1]})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    # ---- rebuilding, not relaying --------------------------------------
    def test_extra_members_are_stripped_rather_than_relayed(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "getdaemonstatus", "params": [],
                                   "auth": "smuggled", "extra": {"x": 1}})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(received[0]), {"jsonrpc", "id", "method", "params"},
                         "the caller's extra members reached the daemon")

    # ---- dispatch -------------------------------------------------------
    def test_an_unknown_command_is_refused(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "getdaemonstatus", "params": []},
                                  command="bash -i")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    def test_an_empty_command_is_refused(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "getdaemonstatus", "params": []},
                                  command="")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])

    def test_a_non_numeric_port_fails_closed(self):
        result = self.run_wrapper({"jsonrpc": "2.0", "id": "w",
                                   "method": "getdaemonstatus", "params": []},
                                  port="; rm -rf /")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(received, [])


if __name__ == "__main__":
    unittest.main()

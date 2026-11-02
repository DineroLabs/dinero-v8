#!/usr/bin/env python3
"""A moving authenticated catalog must not abort the wallet delivery wait."""

import unittest

from test_orchard_daemon_wallet_cycle import Node, RpcError


CATALOG_CHANGED = "Authenticated Orchard catalog changed during read"


class ReadSequence:
    def __init__(self, replies):
        self.replies = iter(replies)
        self.calls = 0

    def rpc(self, method, params):
        assert method == "wallet.orchard.listoperations"
        assert params == {"account": 3}
        self.calls += 1
        reply = next(self.replies)
        if isinstance(reply, Exception):
            raise reply
        return reply

    def wait(self, predicate, label):
        assert label == "account 3 actual provider delivery"
        for _ in range(4):
            if predicate():
                return
        raise AssertionError("delivery did not settle")


def rpc_error(message, *, code=-32603, method="wallet.orchard.listoperations"):
    return RpcError("test RPC failure", method=method,
                    error={"code": code, "message": message})


class CatalogReadRetryTest(unittest.TestCase):
    def test_retry_only_transient_catalog_change_then_return_settled_view(self):
        stale = {"account_sequence": 1, "captured_source_sequence": 2,
                 "account_digest": "a", "captured_source_digest": "b"}
        settled = {"account_sequence": 2, "captured_source_sequence": 2,
                   "account_digest": "b", "captured_source_digest": "b"}
        node = ReadSequence([rpc_error(CATALOG_CHANGED), stale, settled])
        self.assertIs(Node.synced(node, 3), settled)
        self.assertEqual(node.calls, 3)  # No second, racy read after the wait.

    def test_unrelated_rpc_error_is_not_hidden(self):
        for error in (rpc_error("Wallet catalog corrupt"),
                      rpc_error(CATALOG_CHANGED, code=-32602),
                      rpc_error(CATALOG_CHANGED, method="wallet.orchard.getbalance")):
            with self.subTest(message=error.rpc_message, code=error.code, method=error.method):
                node = ReadSequence([error])
                with self.assertRaises(RpcError) as raised:
                    Node.synced(node, 3)
                self.assertIs(raised.exception, error)
                self.assertEqual(node.calls, 1)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""
Integration tests for blockchain.gettransaction and wallet.gettransaction RPCs

Tests against a running daemon with actual blockchain data.
Verifies:
- Transaction lookup and formatting
- Error handling
- Consistency between blockchain and wallet views
- Taproot transaction support
"""

import json
import http.client
import base64
import sys
import time
from typing import Dict, Any, Optional

class DineroRPCClient:
    """Simple RPC client for testing"""

    def __init__(self, host="127.0.0.1", port=20998, datadir="/tmp/dinero_data"):
        self.host = host
        self.port = port
        self.datadir = datadir
        self.auth = None
        self._load_auth()

    def _load_auth(self):
        """Load RPC auth from .cookie file"""
        try:
            with open(f"{self.datadir}/.cookie", "r") as f:
                cookie = f.read().strip()
                self.auth = base64.b64encode(cookie.encode()).decode()
        except FileNotFoundError:
            print(f"Warning: Cookie file not found at {self.datadir}/.cookie")
            self.auth = None

    def call(self, method: str, params=None) -> Dict[str, Any]:
        """Make RPC call"""
        if params is None:
            params = []

        conn = http.client.HTTPConnection(self.host, self.port)
        headers = {
            'Content-type': 'application/json',
        }
        if self.auth:
            headers['Authorization'] = f'Basic {self.auth}'

        payload = {
            "jsonrpc": "2.0",
            "id": method,
            "method": method,
            "params": params
        }

        try:
            conn.request("POST", "/", json.dumps(payload), headers)
            response = conn.getresponse()
            data = response.read().decode()

            if not data:
                return {"error": "Empty response from daemon"}

            return json.loads(data)
        except Exception as e:
            return {"error": str(e)}
        finally:
            conn.close()


class TestResults:
    """Track test results"""

    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.errors = []

    def add_pass(self, test_name: str):
        self.passed += 1
        print(f"  ✅ {test_name}")

    def add_fail(self, test_name: str, reason: str):
        self.failed += 1
        self.errors.append((test_name, reason))
        print(f"  ❌ {test_name}: {reason}")

    def add_skip(self, test_name: str, reason: str):
        self.skipped += 1
        print(f"  ⏭️  {test_name}: {reason}")

    def summary(self):
        print("\n" + "=" * 70)
        print(f"Results: {self.passed} passed, {self.failed} failed, {self.skipped} skipped")
        if self.errors:
            print("\nFailures:")
            for test, reason in self.errors:
                print(f"  - {test}: {reason}")
        print("=" * 70)
        return self.failed == 0


def test_blockchain_gettransaction_invalid_txid(client: DineroRPCClient, results: TestResults):
    """Test blockchain.gettransaction with invalid TXID"""
    result = client.call("blockchain.gettransaction", ["invalid_txid_format"])

    if "error" in result:
        # Expected - should reject invalid format
        results.add_pass("blockchain.gettransaction rejects invalid TXID format")
    else:
        results.add_fail("blockchain.gettransaction invalid TXID", "Should reject invalid format")


def test_blockchain_gettransaction_not_found(client: DineroRPCClient, results: TestResults):
    """Test blockchain.gettransaction with non-existent TXID"""
    fake_txid = "0000000000000000000000000000000000000000000000000000000000000000"
    result = client.call("blockchain.gettransaction", [fake_txid])

    if "result" in result and result["result"].get("status") == "not_found":
        results.add_pass("blockchain.gettransaction returns not_found for missing TX")
    elif "error" in result:
        results.add_pass("blockchain.gettransaction returns error for missing TX")
    else:
        results.add_fail("blockchain.gettransaction not found", f"Unexpected: {result}")


def test_blockchain_gettransaction_premine(client: DineroRPCClient, results: TestResults):
    """Test blockchain.gettransaction with premine transaction"""
    # First get block 1 to find the premine TXID
    block_hash_result = client.call("blockchain.getblockhash", [1])
    if "error" in block_hash_result or "result" not in block_hash_result:
        results.add_skip("blockchain.gettransaction premine", "Cannot get block 1 hash")
        return

    block_result = client.call("blockchain.getblock", [block_hash_result["result"]])
    if "error" in block_result or "result" not in block_result:
        results.add_skip("blockchain.gettransaction premine", "Cannot get block 1")
        return

    tx_array = block_result["result"].get("tx", [])
    if not tx_array:
        results.add_skip("blockchain.gettransaction premine", "No transactions in block 1")
        return

    premine_txid = tx_array[0]

    # Now test gettransaction
    tx_result = client.call("blockchain.gettransaction", [premine_txid])

    if "error" in tx_result:
        results.add_fail("blockchain.gettransaction premine", f"Error: {tx_result['error']}")
        return

    if "result" not in tx_result:
        results.add_fail("blockchain.gettransaction premine", "Missing result field")
        return

    tx = tx_result["result"]

    # Verify fields
    checks = [
        ("txid" in tx, "txid field present"),
        ("blockhash" in tx, "blockhash field present"),
        ("blockheight" in tx, "blockheight field present"),
        (tx.get("blockheight") == 1, "blockheight is 1"),
        ("confirmations" in tx, "confirmations field present"),
        (tx.get("confirmations", 0) > 0, "confirmations > 0"),
        ("status" in tx, "status field present"),
        (tx.get("status") == "confirmed", "status is confirmed"),
        ("is_coinbase" in tx, "is_coinbase field present"),
        (tx.get("is_coinbase") == True, "is_coinbase is true"),
        ("inputs" in tx, "inputs field present"),
        ("outputs" in tx, "outputs field present"),
        (len(tx.get("outputs", [])) > 0, "has at least one output"),
    ]

    all_passed = True
    for check, desc in checks:
        if not check:
            results.add_fail(f"blockchain.gettransaction premine ({desc})", f"Check failed")
            all_passed = False

    if all_passed:
        results.add_pass("blockchain.gettransaction premine (all fields)")


def test_blockchain_gettransaction_taproot_output(client: DineroRPCClient, results: TestResults):
    """Test that Taproot outputs are correctly identified"""
    # Use premine transaction which should be Taproot
    block_hash_result = client.call("blockchain.getblockhash", [1])
    if "result" not in block_hash_result:
        results.add_skip("blockchain.gettransaction Taproot", "Cannot get block hash")
        return

    block_result = client.call("blockchain.getblock", [block_hash_result["result"]])
    if "result" not in block_result:
        results.add_skip("blockchain.gettransaction Taproot", "Cannot get block")
        return

    premine_txid = block_result["result"]["tx"][0]
    tx_result = client.call("blockchain.gettransaction", [premine_txid])

    if "result" not in tx_result:
        results.add_skip("blockchain.gettransaction Taproot", "Cannot get transaction")
        return

    tx = tx_result["result"]
    outputs = tx.get("outputs", [])

    if not outputs:
        results.add_fail("blockchain.gettransaction Taproot", "No outputs found")
        return

    # Check first output type
    output_type = outputs[0].get("type")
    if output_type == "taproot":
        results.add_pass("blockchain.gettransaction detects Taproot output")
    else:
        results.add_fail("blockchain.gettransaction Taproot", f"Expected taproot, got {output_type}")


def test_wallet_gettransaction_not_in_wallet(client: DineroRPCClient, results: TestResults):
    """Test wallet.gettransaction with TX not in wallet"""
    fake_txid = "0000000000000000000000000000000000000000000000000000000000000000"
    result = client.call("wallet.gettransaction", [fake_txid])

    if "result" in result and "error" in result["result"]:
        error_msg = result["result"]["error"]
        if "not found" in error_msg.lower():
            results.add_pass("wallet.gettransaction returns not found for missing TX")
        else:
            results.add_fail("wallet.gettransaction not found", f"Wrong error: {error_msg}")
    else:
        results.add_fail("wallet.gettransaction not found", "Should return error")


def test_wallet_gettransaction_exists(client: DineroRPCClient, results: TestResults):
    """Test wallet.gettransaction with actual wallet transaction"""
    # Get recent transactions from wallet
    txs_result = client.call("wallet.listtransactions", [10])

    if "result" not in txs_result or not txs_result["result"]:
        results.add_skip("wallet.gettransaction exists", "No transactions in wallet")
        return

    # Use first transaction
    first_tx = txs_result["result"][0]
    txid = first_tx["txid"]

    # Get detailed transaction
    tx_result = client.call("wallet.gettransaction", [txid])

    if "error" in tx_result:
        results.add_fail("wallet.gettransaction exists", f"Error: {tx_result['error']}")
        return

    if "result" not in tx_result:
        results.add_fail("wallet.gettransaction exists", "Missing result")
        return

    tx = tx_result["result"]

    # Verify enrichment with blockchain data
    checks = [
        ("txid" in tx, "has txid"),
        ("confirmations" in tx, "has confirmations"),
        ("category" in tx, "has category"),
        ("amount" in tx, "has amount"),
    ]

    all_passed = True
    for check, desc in checks:
        if not check:
            results.add_fail(f"wallet.gettransaction exists ({desc})", "Check failed")
            all_passed = False

    if all_passed:
        results.add_pass("wallet.gettransaction exists (all fields)")


def test_consistency_between_rpcs(client: DineroRPCClient, results: TestResults):
    """Test that blockchain and wallet views are consistent"""
    # Get a transaction from wallet
    txs_result = client.call("wallet.listtransactions", [1])

    if "result" not in txs_result or not txs_result["result"]:
        results.add_skip("RPC consistency", "No wallet transactions")
        return

    txid = txs_result["result"][0]["txid"]

    # Get from both RPCs
    blockchain_result = client.call("blockchain.gettransaction", [txid])
    wallet_result = client.call("wallet.gettransaction", [txid])

    if "result" not in blockchain_result or "result" not in wallet_result:
        results.add_skip("RPC consistency", "Cannot fetch from both RPCs")
        return

    blockchain_tx = blockchain_result["result"]
    wallet_tx = wallet_result["result"]

    # Check consistency
    checks = [
        (blockchain_tx.get("txid") == wallet_tx.get("txid"), "txid matches"),
        (blockchain_tx.get("confirmations") == wallet_tx.get("confirmations"), "confirmations match"),
    ]

    # Only check blockhash if wallet.gettransaction includes it
    if "blockhash" in wallet_tx and "blockhash" in blockchain_tx:
        checks.append((blockchain_tx["blockhash"] == wallet_tx["blockhash"], "blockhash matches"))

    all_passed = True
    for check, desc in checks:
        if not check:
            results.add_fail(f"RPC consistency ({desc})", "Mismatch")
            all_passed = False

    if all_passed:
        results.add_pass("RPC consistency (blockchain vs wallet)")


def test_wallet_listtransactions_type_filter(client: DineroRPCClient, results: TestResults):
    """Test wallet.listtransactions supports type filters."""
    # Sent view
    sent_result = client.call("wallet.listtransactions", [{"count": 50, "type": "sent"}])
    if "error" in sent_result:
        results.add_fail("wallet.listtransactions type=sent", f"RPC error: {sent_result['error']}")
        return
    if "result" not in sent_result or not isinstance(sent_result["result"], list):
        results.add_fail("wallet.listtransactions type=sent", "Missing/invalid result array")
        return
    for tx in sent_result["result"]:
        tx_type = tx.get("type")
        category = tx.get("category", "")
        if tx_type != "sent" and category != "send":
            results.add_fail("wallet.listtransactions type=sent", f"Unexpected tx type/category: {tx}")
            return
    results.add_pass("wallet.listtransactions type=sent")

    # Received view
    recv_result = client.call("wallet.listtransactions", [{"count": 50, "type": "received"}])
    if "error" in recv_result:
        results.add_fail("wallet.listtransactions type=received", f"RPC error: {recv_result['error']}")
        return
    if "result" not in recv_result or not isinstance(recv_result["result"], list):
        results.add_fail("wallet.listtransactions type=received", "Missing/invalid result array")
        return
    for tx in recv_result["result"]:
        if tx.get("type") != "received":
            results.add_fail("wallet.listtransactions type=received", f"Unexpected tx type: {tx}")
            return
    results.add_pass("wallet.listtransactions type=received")


def test_wallet_listtransactions_invalid_type(client: DineroRPCClient, results: TestResults):
    """Test wallet.listtransactions rejects invalid type values."""
    invalid = client.call("wallet.listtransactions", [{"count": 5, "type": "bogus"}])
    rpc_result = invalid.get("result", {})
    if isinstance(rpc_result, dict) and "error" in rpc_result:
        results.add_pass("wallet.listtransactions rejects invalid type")
    else:
        results.add_fail("wallet.listtransactions invalid type", f"Unexpected response: {invalid}")


def main():
    print("=" * 70)
    print("RPC gettransaction Integration Tests")
    print("=" * 70)
    print()

    # Create client
    client = DineroRPCClient()

    # Check if daemon is running
    info_result = client.call("blockchain.getblockcount")
    if "error" in info_result:
        print(f"❌ Cannot connect to daemon: {info_result['error']}")
        print("   Make sure dinerod is running with -datadir=/tmp/dinero_data")
        return 1

    current_height = info_result.get("result", 0)
    print(f"Connected to daemon at height {current_height}")
    print()

    results = TestResults()

    # Run tests
    print("blockchain.gettransaction Tests:")
    test_blockchain_gettransaction_invalid_txid(client, results)
    test_blockchain_gettransaction_not_found(client, results)
    test_blockchain_gettransaction_premine(client, results)
    test_blockchain_gettransaction_taproot_output(client, results)
    print()

    print("wallet.gettransaction Tests:")
    test_wallet_gettransaction_not_in_wallet(client, results)
    test_wallet_gettransaction_exists(client, results)
    print()

    print("Consistency Tests:")
    test_consistency_between_rpcs(client, results)
    print()

    print("wallet.listtransactions Filter Tests:")
    test_wallet_listtransactions_type_filter(client, results)
    test_wallet_listtransactions_invalid_type(client, results)
    print()

    # Summary
    success = results.summary()
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())

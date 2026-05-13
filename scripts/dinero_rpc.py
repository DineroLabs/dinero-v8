#!/usr/bin/env python3
"""
Dinero RPC Client
=================
A Python client for the Dinero RPC interface with automatic cookie authentication.

Usage:
    from dinero_rpc import DineroRPC

    rpc = DineroRPC()  # Uses default datadir
    # or
    rpc = DineroRPC(datadir="/custom/path")

    # Call RPC methods
    info = rpc.getblockchaininfo()
    tx = rpc.blockchain.gettransaction(txid)
    wallet_tx = rpc.wallet.gettransaction(txid)
"""

import os
import json
import base64
import http.client
from typing import Dict, Any, Optional
from pathlib import Path


class RPCError(Exception):
    """RPC call returned an error"""
    def __init__(self, error: Dict[str, Any]):
        self.code = error.get('code', -1)
        self.message = error.get('message', 'Unknown error')
        super().__init__(f"RPC Error {self.code}: {self.message}")


class DineroRPC:
    """
    Dinero RPC client with automatic cookie-based authentication.

    Attributes:
        host: RPC server host (default: 127.0.0.1)
        port: RPC server port (default: 9998)
        datadir: Dinero data directory for cookie file
    """

    def __init__(
        self,
        datadir: Optional[str] = None,
        host: str = "127.0.0.1",
        port: int = 20998,
        timeout: int = 30
    ):
        """
        Initialize the RPC client.

        Args:
            datadir: Path to Dinero data directory (auto-detected if None)
            host: RPC server hostname
            port: RPC server port
            timeout: Request timeout in seconds
        """
        self.host = host
        self.port = port
        self.timeout = timeout
        self.datadir = self._detect_datadir(datadir)
        self._user, self._password = self._load_cookie()

        # Create namespace objects for namespaced RPC methods
        self.blockchain = self._Namespace(self, "blockchain")
        self.wallet = self._Namespace(self, "wallet")
        self.mining = self._Namespace(self, "mining")
        self.network = self._Namespace(self, "network")

    class _Namespace:
        """Helper class for namespaced RPC methods"""
        def __init__(self, client: 'DineroRPC', namespace: str):
            self._client = client
            self._namespace = namespace

        def __getattr__(self, name: str):
            method = f"{self._namespace}.{name}"
            return lambda *args, **kwargs: self._client._call(method, *args, **kwargs)

    def _detect_datadir(self, datadir: Optional[str]) -> Path:
        """
        Detect or validate the Dinero data directory.

        Args:
            datadir: User-provided datadir or None for auto-detection

        Returns:
            Path to the data directory

        Raises:
            FileNotFoundError: If datadir doesn't exist
        """
        if datadir:
            path = Path(datadir)
        else:
            # Auto-detect based on platform
            home = Path.home()
            if os.name == 'nt':  # Windows
                path = home / 'AppData' / 'Roaming' / 'Dinero'
            elif os.uname().sysname == 'Darwin':  # macOS
                path = home / 'Library' / 'Application Support' / 'Dinero'
            else:  # Linux/Unix
                path = home / '.dinero'

        if not path.exists():
            raise FileNotFoundError(
                f"Dinero data directory not found: {path}\n"
                "Provide datadir parameter or ensure daemon is running."
            )

        return path

    def _load_cookie(self) -> tuple[str, str]:
        """
        Load RPC credentials from .cookie file.

        Returns:
            Tuple of (username, password)

        Raises:
            FileNotFoundError: If cookie file doesn't exist
            ValueError: If cookie file has invalid format
        """
        cookie_file = self.datadir / ".cookie"

        if not cookie_file.exists():
            raise FileNotFoundError(
                f"Cookie file not found: {cookie_file}\n"
                "Is the daemon running?"
            )

        cookie = cookie_file.read_text().strip()

        if ':' not in cookie:
            raise ValueError(f"Invalid cookie format in {cookie_file}")

        user, password = cookie.split(':', 1)
        return user, password

    def _get_auth_header(self) -> str:
        """
        Generate HTTP Basic Auth header value.

        Returns:
            Base64-encoded authorization header value
        """
        credentials = f"{self._user}:{self._password}"
        encoded = base64.b64encode(credentials.encode()).decode('ascii')
        return f"Basic {encoded}"

    def _call(self, method: str, *params) -> Any:
        """
        Make an RPC call.

        Args:
            method: RPC method name (e.g., "getblockchaininfo" or "blockchain.gettransaction")
            *params: Method parameters

        Returns:
            The 'result' field from the JSON-RPC response

        Raises:
            RPCError: If the RPC call returns an error
            http.client.HTTPException: If the HTTP request fails
        """
        # Build JSON-RPC request
        payload = {
            "jsonrpc": "2.0",
            "id": "python-rpc",
            "method": method,
            "params": list(params) if params else []
        }

        headers = {
            "Content-Type": "application/json",
            "Authorization": self._get_auth_header(),
            "Connection": "close"
        }

        # Make HTTP request
        conn = http.client.HTTPConnection(
            self.host,
            self.port,
            timeout=self.timeout
        )

        try:
            conn.request(
                "POST",
                "/",
                body=json.dumps(payload),
                headers=headers
            )

            response = conn.getresponse()
            response_data = response.read().decode('utf-8')

            # Debug: print response if empty or invalid
            if not response_data or not response_data.strip():
                raise ValueError(
                    f"Empty response from server (HTTP {response.status} {response.reason})\n"
                    f"Method: {method}, Params: {params}"
                )

            # Parse JSON response
            try:
                result = json.loads(response_data)
            except json.JSONDecodeError as e:
                raise ValueError(
                    f"Invalid JSON response from server:\n"
                    f"Method: {method}\n"
                    f"Response: {response_data[:500]}\n"
                    f"Error: {e}"
                )

            # Check for RPC error
            if 'error' in result and result['error'] is not None:
                raise RPCError(result['error'])

            return result.get('result')

        finally:
            conn.close()

    def __getattr__(self, name: str):
        """
        Allow calling RPC methods directly as attributes.

        Example:
            rpc.getblockchaininfo() instead of rpc._call("getblockchaininfo")
        """
        return lambda *args, **kwargs: self._call(name, *args, **kwargs)


def main():
    """Example usage and basic tests"""
    import sys

    # Parse command line args
    datadir = sys.argv[1] if len(sys.argv) > 1 else None

    try:
        # Initialize client
        rpc = DineroRPC(datadir=datadir)
        print(f"✓ Connected to Dinero RPC at {rpc.host}:{rpc.port}")
        print(f"✓ Using datadir: {rpc.datadir}")
        print()

        # Test basic RPC call
        print("Testing getblockchaininfo...")
        info = rpc.getblockchaininfo()
        print(f"✓ Chain: {info['chain']}")
        print(f"✓ Blocks: {info['blocks']}")
        print(f"✓ Best block: {info['bestblockhash']}")
        print()

        # Test namespaced call
        print("Testing blockchain.gettransaction...")
        block1_hash = rpc.getblockhash(1)
        block1 = rpc.getblock(block1_hash)
        tx1 = block1['tx'][0]

        tx_info = rpc.blockchain.gettransaction(tx1)
        print(f"✓ TXID: {tx_info['txid']}")
        print(f"✓ Coinbase: {tx_info['is_coinbase']}")
        print(f"✓ Confirmations: {tx_info['confirmations']}")
        print()

        # Test wallet call
        print("Testing wallet.listtransactions...")
        wallet_txs = rpc.wallet.listtransactions()
        if wallet_txs:
            print(f"✓ Found {len(wallet_txs)} wallet transactions")

            # Test wallet.gettransaction on first tx
            first_tx = wallet_txs[0]['txid']
            wallet_tx = rpc.wallet.gettransaction(first_tx)
            print(f"✓ Wallet TX: {wallet_tx['txid']}")
            print(f"✓ Category: {wallet_tx.get('category', 'N/A')}")
        else:
            print("✓ No wallet transactions (empty wallet)")
        print()

        print("✓ All tests passed!")

    except FileNotFoundError as e:
        print(f"✗ Error: {e}", file=sys.stderr)
        sys.exit(1)
    except RPCError as e:
        print(f"✗ RPC Error: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"✗ Unexpected error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == '__main__':
    main()

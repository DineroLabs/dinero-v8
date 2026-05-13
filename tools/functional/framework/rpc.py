"""
DineroCoin RPC Client for Functional Tests

JSON-RPC client for communicating with dinerod.
Tailored to DineroCoin's specific RPC interface.
"""

import json
import urllib.request
import urllib.error
import base64
import time
from typing import Any, Optional, Dict, List, Union


class RPCError(Exception):
    """RPC error from dinerod"""
    def __init__(self, code: int, message: str):
        self.code = code
        self.message = message
        super().__init__(f"RPC Error {code}: {message}")


class DineroRPC:
    """
    JSON-RPC client for dinerod.

    Usage:
        rpc = DineroRPC("http://127.0.0.1:20996", "user", "pass")
        height = rpc.getblockcount()
        block_hash = rpc.getbestblockhash()
    """

    def __init__(self, url: str, user: str = "", password: str = "", timeout: int = 60):
        self.url = url
        self.user = user
        self.password = password
        self.timeout = timeout
        self._id = 0

    def _get_auth_header(self) -> str:
        if self.user or self.password:
            credentials = f"{self.user}:{self.password}"
            encoded = base64.b64encode(credentials.encode()).decode()
            return f"Basic {encoded}"
        return ""

    def _call(self, method: str, params: Union[List, Dict, None] = None, retries: int = 3) -> Any:
        """Make a JSON-RPC call to dinerod with automatic retry for transient errors"""
        import os

        last_error = None
        for attempt in range(retries):
            self._id += 1

            payload = {
                "jsonrpc": "2.0",
                "id": self._id,
                "method": method,
                "params": params if params is not None else []
            }

            data = json.dumps(payload).encode('utf-8')

            headers = {
                "Content-Type": "application/json",
                "Connection": "close",
            }

            auth = self._get_auth_header()
            if auth:
                headers["Authorization"] = auth

            # Debug output
            if os.environ.get("RPC_DEBUG"):
                print(f"[RPC DEBUG] URL: {self.url}")
                print(f"[RPC DEBUG] Auth: {auth[:30]}..." if auth else "[RPC DEBUG] No auth")
                print(f"[RPC DEBUG] Data: {data}")

            req = urllib.request.Request(self.url, data=data, headers=headers, method='POST')

            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as response:
                    raw = response.read()
                    if os.environ.get("RPC_DEBUG"):
                        print(f"[RPC DEBUG] Response: {raw[:200]}")
                    result = json.loads(raw.decode('utf-8'))

                    if "error" in result and result["error"] is not None:
                        err = result["error"]
                        code = err.get("code", -1)
                        message = err.get("message", str(err))
                        raise RPCError(code, message)

                    return result.get("result")

            except urllib.error.HTTPError as e:
                body = e.read().decode('utf-8') if e.fp else ""
                if os.environ.get("RPC_DEBUG"):
                    print(f"[RPC DEBUG] HTTP Error {e.code}: {body[:200]} (attempt {attempt + 1}/{retries})")

                # Retry on transient HTTP errors (400, 500, 503)
                if e.code in (400, 500, 503) and attempt < retries - 1:
                    last_error = RPCError(-1, f"HTTP {e.code}: {body}")
                    time.sleep(0.1 * (attempt + 1))  # Backoff
                    continue

                try:
                    result = json.loads(body)
                    if "error" in result and result["error"] is not None:
                        err = result["error"]
                        raise RPCError(err.get("code", -1), err.get("message", str(err)))
                except json.JSONDecodeError:
                    raise RPCError(-1, f"HTTP {e.code}: {body}")

            except urllib.error.URLError as e:
                if attempt < retries - 1:
                    last_error = RPCError(-1, f"Connection failed: {e.reason}")
                    time.sleep(0.1 * (attempt + 1))
                    continue
                raise RPCError(-1, f"Connection failed: {e.reason}")
            except TimeoutError:
                if attempt < retries - 1:
                    last_error = RPCError(-1, "Request timed out")
                    time.sleep(0.1 * (attempt + 1))
                    continue
                raise RPCError(-1, "Request timed out")

        # Should not reach here, but just in case
        if last_error:
            raise last_error
        raise RPCError(-1, "Unknown error")

    def __getattr__(self, method: str):
        """
        Allow calling RPC methods as attributes.

        Example:
            rpc.getblockcount()  # calls getblockcount RPC
            rpc.getblock(hash, 2)  # calls getblock with params
        """
        def rpc_method(*args, **kwargs):
            # Convert args to list, kwargs to dict if needed
            if kwargs:
                params = kwargs
            elif args:
                params = list(args)
            else:
                params = []
            return self._call(method, params)
        return rpc_method

    # Explicit methods for IDE autocomplete and documentation

    def getblockcount(self) -> int:
        """Get the current block height"""
        return self._call("getblockcount")

    def getbestblockhash(self) -> str:
        """Get the hash of the best (tip) block"""
        return self._call("getbestblockhash")

    def getblockhash(self, height: int) -> str:
        """Get block hash at given height"""
        return self._call("getblockhash", [height])

    def getblock(self, blockhash: str, verbosity: int = 1) -> Union[str, Dict]:
        """Get block data. verbosity: 0=hex, 1=json, 2=json+tx"""
        return self._call("getblock", [blockhash, verbosity])

    def getblockchaininfo(self) -> Dict:
        """Get blockchain state info"""
        return self._call("getblockchaininfo")

    def getmininginfo(self) -> Dict:
        """Get mining-related info"""
        return self._call("getmininginfo")

    def getblocktemplate(self, template_request: Optional[Dict] = None) -> Dict:
        """Get block template for mining"""
        params = [template_request] if template_request else []
        return self._call("getblocktemplate", params)

    def submitblock(self, hexdata: str) -> Optional[str]:
        """Submit a mined block"""
        return self._call("submitblock", [hexdata])

    def getbalance(self, account: str = "", minconf: int = 1) -> float:
        """Get wallet balance"""
        result = self._call("getbalance", [account, minconf])
        # DineroCoin returns detailed balance info, extract total
        if isinstance(result, dict):
            return result.get("total", result.get("spendable", 0.0))
        return result

    def getnewaddress(self, label: str = "", address_type: str = "") -> str:
        """Generate a new address"""
        params = []
        if label:
            params.append(label)
        if address_type:
            params.append(address_type)
        result = self._call("getnewaddress", params)
        # DineroCoin returns {"address": "...", "address_type": "...", ...}
        # Extract just the address string for compatibility
        if isinstance(result, dict) and "address" in result:
            return result["address"]
        return result

    def sendtoaddress(self, address: str, amount: float) -> str:
        """Send coins to an address, returns txid"""
        return self._call("sendtoaddress", [address, amount])

    def generate(self, nblocks: int, maxtries: int = 1000000) -> List[str]:
        """Mine blocks (regtest only), returns block hashes"""
        result = self._call("generate", [nblocks, maxtries])
        # DineroCoin returns {"blocks": [...]} instead of just [...]
        if isinstance(result, dict) and "blocks" in result:
            return result["blocks"]
        return result

    def generatetoaddress(self, nblocks: int, address: str, maxtries: int = 1000000) -> List[str]:
        """Mine blocks to specific address (regtest only)"""
        result = self._call("generatetoaddress", [nblocks, address, maxtries])
        # DineroCoin returns {"blocks": [...]} instead of just [...]
        if isinstance(result, dict) and "blocks" in result:
            return result["blocks"]
        return result

    def getconnectioncount(self) -> int:
        """Get number of P2P connections"""
        return self._call("getconnectioncount")

    def getpeerinfo(self) -> List[Dict]:
        """Get info about connected peers"""
        return self._call("getpeerinfo")

    def addnode(self, node: str, command: str) -> None:
        """Add/remove/try a node. command: add|remove|onetry"""
        return self._call("addnode", [node, command])

    def disconnectnode(self, address: str = "", nodeid: int = 0) -> None:
        """Disconnect a peer"""
        if address:
            return self._call("disconnectnode", [address])
        else:
            return self._call("disconnectnode", ["", nodeid])

    def setnetworkactive(self, state: bool) -> bool:
        """Enable or disable P2P networking"""
        return self._call("setnetworkactive", [state])

    def setban(self, subnet: str, command: str, bantime: int = 0, absolute: bool = False) -> Dict:
        """Add/remove ban for a subnet. command: add|remove"""
        return self._call("setban", [subnet, command, bantime, absolute])

    def listbanned(self) -> List[Dict]:
        """List all banned addresses"""
        return self._call("listbanned")

    def clearbanned(self) -> None:
        """Clear all banned addresses"""
        return self._call("clearbanned")

    def getmempooolinfo(self) -> Dict:
        """Get mempool state"""
        return self._call("getmempoolinfo")

    def getrawmempool(self, verbose: bool = False) -> Union[List[str], Dict]:
        """Get mempool contents"""
        return self._call("getrawmempool", [verbose])

    def stop(self) -> str:
        """Stop the daemon"""
        return self._call("stop")

    def ping(self) -> bool:
        """Check if daemon is responsive"""
        try:
            self.getblockcount()
            return True
        except:
            return False

    def wait_for_rpc_connection(self, timeout: int = 60) -> bool:
        """Wait for RPC to become available"""
        start = time.time()
        while time.time() - start < timeout:
            if self.ping():
                return True
            time.sleep(0.5)
        return False

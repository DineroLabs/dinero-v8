#!/usr/bin/env python3
"""
DineroCoin WebSocket RPC Client Example

This example demonstrates how to connect to the DineroCoin daemon's WebSocket RPC
interface and perform wallet and mining operations.
"""

import asyncio
import websockets
import json
import base64
from pathlib import Path

class DineroWebSocketClient:
    def __init__(self, ws_url, cookie_path):
        self.ws_url = ws_url
        self.cookie_path = cookie_path
        self.auth_header = self._load_auth()
        
    def _load_auth(self):
        """Load authentication from cookie file"""
        with open(self.cookie_path) as f:
            cookie = f.read().strip()
        return "Basic " + base64.b64encode(cookie.encode()).decode()
        
    async def call_rpc(self, method, params=None):
        """Make a single RPC call via WebSocket"""
        if params is None:
            params = []
            
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": method,
            "params": params
        }
        
        headers = {"Authorization": self.auth_header}
        
        async with websockets.connect(self.ws_url, extra_headers=headers) as websocket:
            await websocket.send(json.dumps(request))
            response = await websocket.recv()
            result = json.loads(response)
            
            if "error" in result and result["error"]:
                raise Exception(f"RPC Error: {result['error']}")
                
            return result.get("result")

async def main():
    # Load connection info from nodeinfo.json
    nodeinfo_path = Path.home() / ".dinero" / "regtest" / "nodeinfo.json"
    
    with open(nodeinfo_path) as f:
        nodeinfo = json.load(f)
        
    ws_url = nodeinfo["ws"]["url"]
    cookie_path = nodeinfo["cookie"]
    
    client = DineroWebSocketClient(ws_url, cookie_path)
    
    print("🔗 Connected to DineroCoin WebSocket RPC")
    print(f"   URL: {ws_url}")
    
    try:
        # Get blockchain info
        best_hash = await client.call_rpc("getbestblockhash")
        print(f"📦 Best block: {best_hash}")
        
        # Generate new address
        address = await client.call_rpc("wallet.getnewaddress")
        print(f"🏠 New address: {address}")
        
        # Validate address
        validation = await client.call_rpc("wallet.validateaddress", [address])
        print(f"✅ Address valid: {validation['isvalid']}, mine: {validation['ismine']}")
        
        # Set mining address
        await client.call_rpc("mining.setaddress", [address])
        print(f"⛏️  Mining address set to: {address}")
        
        # Get mining address
        mining_info = await client.call_rpc("mining.getaddress")
        print(f"⛏️  Current mining: {mining_info['address']} (mine: {mining_info['ismine']})")
        
        # Generate test blocks (regtest only)
        blocks = await client.call_rpc("mining.generatetoaddress", [3, address])
        print(f"🎯 Generated {len(blocks)} blocks")
        
    except Exception as e:
        print(f"❌ Error: {e}")

if __name__ == "__main__":
    asyncio.run(main())

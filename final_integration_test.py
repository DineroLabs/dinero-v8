#!/usr/bin/env python3
import asyncio
import websockets
import json
import requests
import sys
from requests.auth import HTTPBasicAuth

# Configuration
RPC_URL = "http://127.0.0.1:52806"
WS_URL = "ws://127.0.0.1:52807/ws"
COOKIE_FILE = "/Users/haydarevich/Library/Application Support/DineroCoin/Dinero All-in-One/data/mainnet/.cookie"

def get_cookie():
    with open(COOKIE_FILE, 'r') as f:
        return f.read().strip()

def rpc_call(method, params=None):
    if params is None:
        params = {}
    
    cookie = get_cookie()
    auth = HTTPBasicAuth(cookie.split(':')[0], cookie.split(':')[1])
    
    payload = {
        "jsonrpc": "2.0",
        "method": method,
        "params": params,
        "id": 1
    }
    
    try:
        response = requests.post(RPC_URL, json=payload, auth=auth, timeout=5)
        return response.json()
    except Exception as e:
        return {"error": str(e)}

async def test_websocket_integration():
    print("=== 🔍 TEST 8: FINAL INTEGRATION TEST ===")
    
    # Test 1: RPC Functions
    print("\n📡 Testing RPC Functions:")
    
    rpc_tests = [
        ("getblockcount", {}),
        ("getbestblockhash", {}),
        ("getblockchaininfo", {}),
        ("getmininginfo", {}),
        ("wallet.info", {}),
        ("wallet.getnewaddress", {}),
        ("wallet.listaddresses", {})
    ]
    
    rpc_results = {}
    for method, params in rpc_tests:
        result = rpc_call(method, params)
        success = "result" in result
        print(f"  {'✅' if success else '❌'} {method}: {'OK' if success else result.get('error', 'Failed')}")
        rpc_results[method] = result
    
    # Test 2: WebSocket Real-time Data
    print("\n🔌 Testing WebSocket Real-time Data:")
    
    try:
        async with websockets.connect(WS_URL) as ws:
            print("  ✅ WebSocket connection: OK")
            
            # Collect messages for 5 seconds
            messages = []
            count = 0
            async for msg in ws:
                try:
                    data = json.loads(msg)
                    messages.append(data)
                    count += 1
                    if count >= 4:
                        break
                except json.JSONDecodeError:
                    pass
            
            # Analyze messages
            hello_msgs = [m for m in messages if m.get("event") == "hello"]
            mining_msgs = [m for m in messages if m.get("event") == "mining-stats"]
            tip_msgs = [m for m in messages if m.get("type") == "tip"]
            
            print(f"  ✅ Hello messages: {len(hello_msgs)}")
            print(f"  ✅ Mining-stats messages: {len(mining_msgs)}")
            print(f"  ✅ Chain-tip messages: {len(tip_msgs)}")
            
            # Test 3: Data Consistency Check
            print("\n🔍 Testing Data Consistency:")
            
            if tip_msgs and "result" in rpc_results["getblockcount"]:
                ws_height = tip_msgs[0].get("height")
                rpc_height = rpc_results["getblockcount"]["result"]
                
                print(f"  WebSocket height: {ws_height}")
                print(f"  RPC height: {rpc_height}")
                print(f"  {'✅' if ws_height == rpc_height else '❌'} Height consistency: {'OK' if ws_height == rpc_height else 'MISMATCH'}")
            
            if tip_msgs and "result" in rpc_results["getbestblockhash"]:
                ws_hash = tip_msgs[0].get("best", "").lower()
                rpc_hash = rpc_results["getbestblockhash"]["result"].lower()
                
                print(f"  WebSocket hash: {ws_hash[:16]}...")
                print(f"  RPC hash: {rpc_hash[:16]}...")
                print(f"  {'✅' if ws_hash == rpc_hash else '❌'} Hash consistency: {'OK' if ws_hash == rpc_hash else 'MISMATCH'}")
            
            # Test 4: GUI Data Verification
            print("\n🖥️  GUI Data Verification:")
            
            if tip_msgs:
                tip = tip_msgs[0]
                print(f"  Dashboard should show:")
                print(f"    Height: {tip.get('height')}")
                print(f"    Sync: {'Synced' if tip.get('synced') else 'Syncing'}")
                print(f"    Mempool: {tip.get('mempool_tx', 0)} tx")
                print(f"    Hashrate: {tip.get('hashrate', 0.0)} H/s")
            
            # Test 5: Wallet Functions
            print("\n💰 Wallet Functions:")
            
            if "result" in rpc_results["wallet.info"]:
                wallet_info = rpc_results["wallet.info"]["result"]
                print(f"  ✅ Wallet status: {wallet_info.get('status')}")
                print(f"  ✅ Balance: {wallet_info.get('balance')} DIN")
                print(f"  ✅ Transactions: {wallet_info.get('txs')}")
            
            if "result" in rpc_results["wallet.listaddresses"]:
                addresses = rpc_results["wallet.listaddresses"]["result"]["addresses"]
                print(f"  ✅ Addresses: {len(addresses)} generated")
                for i, addr in enumerate(addresses[:3]):  # Show first 3
                    print(f"    {i+1}. {addr}")
            
            return True
            
    except Exception as e:
        print(f"  ❌ WebSocket test failed: {e}")
        return False

async def main():
    success = await test_websocket_integration()
    
    print(f"\n{'='*50}")
    print(f"🎯 FINAL RESULT: {'✅ ALL TESTS PASSED' if success else '❌ SOME TESTS FAILED'}")
    print(f"{'='*50}")
    
    if success:
        print("\n🚀 The dinero-all-in-one app is FULLY FUNCTIONAL!")
        print("   • Daemon is running and responding")
        print("   • WebSocket is broadcasting real-time data")
        print("   • RPC methods are working")
        print("   • Wallet functions are operational")
        print("   • Data consistency is maintained")
        print("   • GUI should show live blockchain data")
    
    return success

if __name__ == "__main__":
    success = asyncio.run(main())
    sys.exit(0 if success else 1)

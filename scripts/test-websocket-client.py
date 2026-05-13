#!/usr/bin/env python3
"""
Simple WebSocket test client for testing the separated WebSocket server
"""

import asyncio
import websockets
import json
import base64
import sys
import pathlib

async def test_websocket():
    # Read cookie from file
    cookie_path = pathlib.Path("/private/tmp/test-dir2/.cookie")
    if not cookie_path.exists():
        print(f"❌ Cookie file not found: {cookie_path}")
        return
    
    COOKIE = cookie_path.read_text().strip()
    TOKEN = base64.b64encode(COOKIE.encode()).decode()
    
    print(f"🔑 Cookie: {COOKIE}")
    print(f"🔑 Token: {TOKEN}")
    
    # WebSocket URI
    uri = "ws://127.0.0.1:21001"
    
    try:
        # Connect to WebSocket server
        headers = [("Authorization", f"Basic {TOKEN}")]
        async with websockets.connect(uri, additional_headers=headers) as websocket:
            print("✅ Connected to WebSocket server!")
            
            # Test ping
            ping_msg = {"op": "ping"}
            print(f"📤 Sending: {json.dumps(ping_msg)}")
            await websocket.send(json.dumps(ping_msg))
            
            # Wait for pong response
            response = await asyncio.wait_for(websocket.recv(), timeout=5.0)
            print(f"📥 Received: {response}")
            
            # Test subscription
            subscribe_msg = {
                "op": "subscribe",
                "topic": "blocks"
            }
            
            print(f"📤 Sending: {json.dumps(subscribe_msg)}")
            await websocket.send(json.dumps(subscribe_msg))
            
            # Wait for subscription response
            response = await asyncio.wait_for(websocket.recv(), timeout=5.0)
            print(f"📥 Received: {response}")
            
            # Test unsubscribe
            unsubscribe_msg = {
                "op": "unsubscribe", 
                "topic": "blocks"
            }
            
            print(f"📤 Sending: {json.dumps(unsubscribe_msg)}")
            await websocket.send(json.dumps(unsubscribe_msg))
            
            # Wait for unsubscribe response
            response = await asyncio.wait_for(websocket.recv(), timeout=5.0)
            print(f"📥 Received: {response}")
            
            print("✅ WebSocket test completed successfully!")
            
    except websockets.exceptions.InvalidStatusCode as e:
        print(f"❌ WebSocket connection failed with status {e.status_code}")
        print(f"Response: {e.headers}")
    except Exception as e:
        print(f"❌ WebSocket test failed: {e}")

if __name__ == "__main__":
    print("🧪 Testing Separated WebSocket Server")
    print("=" * 40)
    
    try:
        asyncio.run(test_websocket())
    except KeyboardInterrupt:
        print("\n⏹️  Test interrupted by user")
    except Exception as e:
        print(f"❌ Test failed: {e}")
        sys.exit(1)

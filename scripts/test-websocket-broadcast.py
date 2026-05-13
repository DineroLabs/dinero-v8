#!/usr/bin/env python3
"""
Test WebSocket event broadcasting when new blocks are found
"""

import asyncio
import websockets
import json
import base64
import pathlib
import sys

async def test_broadcasting():
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
            
            # Subscribe to blocks topic
            subscribe_msg = {
                "op": "subscribe",
                "topic": "blocks"
            }
            
            print(f"📤 Subscribing to blocks: {json.dumps(subscribe_msg)}")
            await websocket.send(json.dumps(subscribe_msg))
            
            # Wait for subscription confirmation
            response = await asyncio.wait_for(websocket.recv(), timeout=5.0)
            print(f"📥 Subscription response: {response}")
            
            print("🔄 Waiting for block events... (mining is active)")
            print("   Press Ctrl+C to stop")
            
            # Wait for broadcast events
            try:
                while True:
                    response = await asyncio.wait_for(websocket.recv(), timeout=30.0)
                    print(f"📡 BROADCAST RECEIVED: {response}")
            except asyncio.TimeoutError:
                print("⏰ No events received in 30 seconds")
            
    except websockets.exceptions.InvalidStatusCode as e:
        print(f"❌ WebSocket connection failed with status {e.status_code}")
        print(f"Response: {e.headers}")
    except Exception as e:
        print(f"❌ WebSocket test failed: {e}")

if __name__ == "__main__":
    print("🧪 Testing WebSocket Event Broadcasting")
    print("=" * 40)
    
    try:
        asyncio.run(test_broadcasting())
    except KeyboardInterrupt:
        print("\n⏹️  Test interrupted by user")
    except Exception as e:
        print(f"❌ Test failed: {e}")
        sys.exit(1)

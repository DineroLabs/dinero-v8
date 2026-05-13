#!/usr/bin/env python3
"""
WebSocket Stress Testing Script
Tests the WebSocket server with multiple concurrent connections
"""

import asyncio
import websockets
import base64
import pathlib
import json
import os
import time
import signal
import sys

# Configuration
COOKIE_PATH = "/private/tmp/test-dir2/.cookie"
WS_URI = "ws://127.0.0.1:21001"
DEFAULT_CONNECTIONS = 200

# Global state
active_connections = 0
total_messages = 0
start_time = time.time()

def signal_handler(signum, frame):
    print(f"\n🛑 Received signal {signum}, shutting down...")
    print(f"📊 Final Stats: {active_connections} connections, {total_messages} messages")
    sys.exit(0)

signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)

async def client(connection_id):
    """Individual WebSocket client connection"""
    global active_connections, total_messages
    
    try:
        # Read cookie and create auth header
        if not pathlib.Path(COOKIE_PATH).exists():
            print(f"❌ Cookie file not found: {COOKIE_PATH}")
            return
        
        cookie = pathlib.Path(COOKIE_PATH).read_text().strip()
        token = base64.b64encode(cookie.encode()).decode()
        headers = [("Authorization", f"Basic {token}")]
        
        # Connect to WebSocket server
        async with websockets.connect(
            WS_URI, 
            extra_headers=headers, 
            ping_interval=20, 
            ping_timeout=30
        ) as ws:
            active_connections += 1
            print(f"✅ Client {connection_id:3d} connected (total: {active_connections})")
            
            # Subscribe to blocks topic
            subscribe_msg = {"op": "subscribe", "topic": "blocks"}
            await ws.send(json.dumps(subscribe_msg))
            
            # Wait for subscription ack
            ack = await asyncio.wait_for(ws.recv(), timeout=5.0)
            if "subscribed" in ack:
                print(f"📡 Client {connection_id:3d} subscribed to blocks")
            
            # Wait for broadcasts (up to 3 messages)
            try:
                for msg_count in range(3):
                    message = await asyncio.wait_for(ws.recv(), timeout=30.0)
                    total_messages += 1
                    print(f"📥 Client {connection_id:3d} received message {msg_count + 1}: {message[:100]}...")
            except asyncio.TimeoutError:
                print(f"⏰ Client {connection_id:3d} timed out waiting for messages")
            
    except Exception as e:
        print(f"❌ Client {connection_id:3d} error: {e}")
    finally:
        active_connections -= 1
        print(f"🔌 Client {connection_id:3d} disconnected (remaining: {active_connections})")

async def main():
    """Main stress test function"""
    # Get number of connections from environment or use default
    num_connections = int(os.environ.get("N", str(DEFAULT_CONNECTIONS)))
    
    print("🧪 WebSocket Stress Test")
    print("=" * 40)
    print(f"🎯 Target connections: {num_connections}")
    print(f"🌐 WebSocket URI: {WS_URI}")
    print(f"🍪 Cookie file: {COOKIE_PATH}")
    print(f"⏰ Start time: {time.strftime('%H:%M:%S')}")
    print()
    
    # Create all client tasks
    tasks = [client(i) for i in range(num_connections)]
    
    # Run all clients concurrently
    print("🚀 Starting stress test...")
    await asyncio.gather(*tasks, return_exceptions=True)
    
    # Final stats
    duration = time.time() - start_time
    print()
    print("🏁 Stress test completed!")
    print(f"⏱️  Duration: {duration:.2f} seconds")
    print(f"📊 Total messages received: {total_messages}")
    print(f"📈 Messages per second: {total_messages / duration:.2f}")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n⏹️  Test interrupted by user")
    except Exception as e:
        print(f"❌ Test failed: {e}")
        sys.exit(1)

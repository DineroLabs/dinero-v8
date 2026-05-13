#!/usr/bin/env python3
import asyncio
import websockets
import json
import sys

async def test_websocket():
    print("=== 🔍 TEST 2: WEBSOCKET CONNECTION TEST ===")
    try:
        async with websockets.connect("ws://127.0.0.1:52807/ws") as ws:
            print("✅ WebSocket connected successfully!")
            
            # Collect messages for 8 seconds
            messages = []
            count = 0
            async for msg in ws:
                try:
                    data = json.loads(msg)
                    messages.append(data)
                    
                    if data.get("event") == "hello":
                        print(f"✅ HELLO: version={data.get('version')}")
                    elif data.get("event") == "mining-stats":
                        print(f"✅ MINING-STATS: height={data.get('height')}, mining={data.get('mining')}, hps1={data.get('hps1')}")
                    elif data.get("type") == "tip":
                        print(f"✅ CHAIN-TIP: height={data.get('height')}, synced={data.get('synced')}, mempool={data.get('mempool_tx')}, hashrate={data.get('hashrate')}")
                    
                    count += 1
                    if count >= 6:  # Get 6 messages
                        break
                        
                except json.JSONDecodeError:
                    print(f"❌ Invalid JSON: {msg}")
                    
            print(f"✅ Received {len(messages)} WebSocket messages")
            return messages
            
    except Exception as e:
        print(f"❌ WebSocket test failed: {e}")
        return []

async def main():
    messages = await test_websocket()
    
    print("\n=== 📊 WEBSOCKET MESSAGE ANALYSIS ===")
    hello_count = sum(1 for m in messages if m.get("event") == "hello")
    mining_count = sum(1 for m in messages if m.get("event") == "mining-stats")
    tip_count = sum(1 for m in messages if m.get("type") == "tip")
    
    print(f"Hello messages: {hello_count}")
    print(f"Mining-stats messages: {mining_count}")
    print(f"Chain-tip messages: {tip_count}")
    
    if tip_count > 0:
        tip_msg = next(m for m in messages if m.get("type") == "tip")
        print(f"\n=== 🎯 LATEST CHAIN DATA ===")
        print(f"Height: {tip_msg.get('height')}")
        print(f"Best Block: {tip_msg.get('best', '')[:16]}...")
        print(f"Synced: {tip_msg.get('synced')}")
        print(f"Mempool TX: {tip_msg.get('mempool_tx')}")
        print(f"Hashrate: {tip_msg.get('hashrate')} H/s")
        
        return tip_msg
    
    return None

if __name__ == "__main__":
    asyncio.run(main())

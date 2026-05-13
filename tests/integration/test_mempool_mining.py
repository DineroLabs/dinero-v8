#!/usr/bin/env python3
"""
Integration Test: Mine block with mempool transaction
=====================================================

Tests the complete integration:
- Genesis/premine UTXOs in ChainDB
- Mempool accepts spend tx
- generatetoaddress builds block with mempool txs
- getCoin() finds the UTXOs being spent
- Block connects successfully

Usage:
    python3 test_mempool_mining.py [--datadir PATH] [--fresh]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

# Configuration
RPC_PORT = 20996
P2P_PORT = 21001
TEST_ADDRESS = "rdin1pef20rtetydu8nttnkf2pqnm0egjzug46v2xphhjejcd233jevs4shvspjd"
MINING_ADDRESS = "rdin1p86vgfwjzv8wdk8wyfvyfafhkk02vkjutd4sqygqurqyfkktthhxqxpc8nz"

class DineroTest:
    def __init__(self, datadir: str, dinerod_path: str, cli_path: str):
        self.datadir = Path(datadir)
        self.dinerod = dinerod_path
        self.cli = cli_path
        self.daemon_proc = None
        
    def cli_cmd(self, *args) -> dict:
        """Run CLI command and return JSON result"""
        cmd = [
            self.cli,
            f"-datadir={self.datadir}",
            f"-rpcport={RPC_PORT}",
        ] + list(args)
        
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            if result.returncode != 0:
                print(f"CLI Error: {result.stderr}")
                return {"error": result.stderr}
            
            # Try to parse as JSON
            try:
                return json.loads(result.stdout)
            except json.JSONDecodeError:
                return {"result": result.stdout.strip()}
        except subprocess.TimeoutExpired:
            return {"error": "timeout"}
        except Exception as e:
            return {"error": str(e)}
    
    def start_daemon(self, fresh: bool = False):
        """Start dinerod daemon"""
        if fresh:
            print(f"🗑️  Removing existing data: {self.datadir}")
            if self.datadir.exists():
                shutil.rmtree(self.datadir)
            self.datadir.mkdir(parents=True, exist_ok=True)
            
            # Create config
            config = self.datadir / "dinero.conf"
            config.write_text(f"""regtest=1
server=1
daemon=0
rpcport={RPC_PORT}
port={P2P_PORT}
fallbackfee=0.00001
txindex=1
""")
        
        print(f"🚀 Starting daemon...")
        self.daemon_proc = subprocess.Popen(
            [
                self.dinerod,
                f"-datadir={self.datadir}",
                "-regtest",
                f"-rpcport={RPC_PORT}",
                f"-port={P2P_PORT}",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        
        # Wait for startup
        for i in range(30):
            time.sleep(1)
            info = self.cli_cmd("getinfo")
            if "error" not in info or "connect" not in str(info.get("error", "")):
                print(f"✅ Daemon started at block {info.get('blocks', '?')}")
                return True
            print(f"   Waiting for daemon... ({i+1}s)")
        
        print("❌ Daemon failed to start")
        return False
    
    def stop_daemon(self):
        """Stop daemon"""
        if self.daemon_proc:
            self.daemon_proc.terminate()
            self.daemon_proc.wait(timeout=10)
            print("🛑 Daemon stopped")
    
    def mine_initial_blocks(self, count: int = 101):
        """Mine initial blocks to mature coinbase"""
        print(f"⛏️  Mining {count} initial blocks...")
        
        # Set mining address
        self.cli_cmd("mining.setaddress", MINING_ADDRESS)
        
        # Mine blocks
        for i in range(0, count, 10):
            batch = min(10, count - i)
            result = self.cli_cmd("mining.generatetoaddress", str(batch), MINING_ADDRESS)
            if "error" in result:
                print(f"   Mining error at block {i}: {result}")
                # Try alternative
                self.cli_cmd("mining.start", str(batch))
                time.sleep(5)
            print(f"   Mined {i + batch}/{count} blocks")
        
        info = self.cli_cmd("getblockcount")
        print(f"✅ At block height: {info}")
        return True
    
    def check_balance(self) -> float:
        """Get wallet balance"""
        result = self.cli_cmd("getbalance")
        if isinstance(result, dict) and "spendable" in result:
            return float(result["spendable"])
        return 0.0
    
    def send_coins(self, address: str, amount: float) -> str:
        """Send coins and return txid"""
        print(f"💸 Sending {amount} DIN to {address[:20]}...")
        
        result = self.cli_cmd("sendtoaddress", address, str(amount))
        
        if isinstance(result, dict):
            if "txid" in result:
                txid = result["txid"]
                print(f"✅ Transaction sent: {txid[:16]}...")
                return txid
            elif "error" in result:
                print(f"❌ Send failed: {result['error']}")
                return None
        
        return None
    
    def mine_block_with_mempool(self) -> bool:
        """Mine a block including mempool transactions"""
        print("⛏️  Mining block with mempool transactions...")
        
        # Check mempool first
        mempool = self.cli_cmd("getrawmempool")
        if isinstance(mempool, list):
            print(f"   Mempool has {len(mempool)} transactions")
        
        # Mine
        result = self.cli_cmd("mining.generatetoaddress", "1", MINING_ADDRESS)
        
        if "error" in str(result):
            print(f"❌ Mining failed: {result}")
            return False
        
        print("✅ Block mined")
        return True
    
    def verify_transaction(self, txid: str) -> bool:
        """Verify transaction is confirmed"""
        print(f"🔍 Verifying transaction {txid[:16]}...")
        
        result = self.cli_cmd("gettransaction", txid)
        
        if isinstance(result, dict):
            confirmations = result.get("confirmations", 0)
            status = result.get("status", "unknown")
            
            print(f"   Confirmations: {confirmations}")
            print(f"   Status: {status}")
            
            if confirmations > 0 or status == "confirmed":
                print("✅ Transaction confirmed!")
                return True
        
        print("❌ Transaction not confirmed")
        return False


def main():
    parser = argparse.ArgumentParser(description="Test mempool mining integration")
    parser.add_argument("--datadir", default="./data-regtest-test", help="Data directory")
    parser.add_argument("--fresh", action="store_true", help="Start with fresh chain")
    parser.add_argument("--dinerod", default="./build/dinerod", help="Path to dinerod")
    parser.add_argument("--cli", default="./dcli-regtest", help="Path to CLI")
    args = parser.parse_args()
    
    print("=" * 60)
    print("Integration Test: Mine block with mempool tx")
    print("=" * 60)
    
    test = DineroTest(args.datadir, args.dinerod, args.cli)
    
    try:
        # Step 1: Start daemon
        if not test.start_daemon(fresh=args.fresh):
            return 1
        
        # Step 2: Check initial state
        info = test.cli_cmd("getinfo")
        blocks = info.get("blocks", 0)
        print(f"\n📊 Initial state: {blocks} blocks")
        
        # Step 3: Mine initial blocks if fresh
        if args.fresh or blocks < 101:
            test.mine_initial_blocks(101 - blocks)
        
        # Step 4: Check balance
        balance = test.check_balance()
        print(f"\n💰 Wallet balance: {balance} DIN")
        
        if balance < 5:
            print("❌ Insufficient balance for test")
            return 1
        
        # Step 5: Send 5 DIN
        print("\n" + "=" * 40)
        print("TEST: Send 5 DIN and mine block")
        print("=" * 40)
        
        txid = test.send_coins(TEST_ADDRESS, 5.0)
        if not txid:
            print("❌ Failed to create transaction")
            return 1
        
        # Step 6: Check mempool
        mempool = test.cli_cmd("getrawmempool")
        print(f"\n📦 Mempool: {mempool}")
        
        # Step 7: Mine block with tx
        if not test.mine_block_with_mempool():
            print("❌ Failed to mine block")
            return 1
        
        # Step 8: Verify transaction confirmed
        time.sleep(2)
        if not test.verify_transaction(txid):
            print("❌ Transaction not confirmed after mining")
            return 1
        
        # Step 9: Final balance check
        final_balance = test.check_balance()
        print(f"\n💰 Final balance: {final_balance} DIN")
        
        print("\n" + "=" * 60)
        print("✅ TEST PASSED: Mempool tx mined successfully!")
        print("=" * 60)
        return 0
        
    except KeyboardInterrupt:
        print("\n⚠️  Interrupted")
        return 1
    finally:
        test.stop_daemon()


if __name__ == "__main__":
    sys.exit(main())

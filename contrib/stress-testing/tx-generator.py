#!/usr/bin/env python3
"""
Dinero Transaction Generator
Stress testing tool for generating high transaction loads
"""

import asyncio
import json
import time
import random
import logging
from typing import List, Dict, Any
import aiohttp
from aiohttp import BasicAuth
import argparse
from dataclasses import dataclass
from concurrent.futures import ThreadPoolExecutor
import statistics

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

@dataclass
class TestResult:
    """Results from a stress test run"""
    total_transactions: int
    successful_transactions: int
    failed_transactions: int
    duration: float
    tps: float
    avg_response_time: float
    min_response_time: float
    max_response_time: float
    p95_response_time: float
    p99_response_time: float

class DineroCoinStressTester:
    """Stress tester for Dinero network"""
    
    def __init__(self, rpc_url: str, rpc_user: str = None, rpc_password: str = None):
        self.rpc_url = rpc_url
        self.auth = BasicAuth(rpc_user, rpc_password) if rpc_user and rpc_password else None
        self.session = None
        
        # Test wallets and addresses (pre-generated for testing)
        self.test_addresses = []
        self.utxos = []
        
    async def __aenter__(self):
        """Async context manager entry"""
        self.session = aiohttp.ClientSession()
        return self
        
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        """Async context manager exit"""
        if self.session:
            await self.session.close()
    
    async def rpc_call(self, method: str, params: List = None) -> Dict[str, Any]:
        """Make async RPC call"""
        if params is None:
            params = []
            
        payload = {
            "jsonrpc": "2.0",
            "id": random.randint(1, 1000000),
            "method": method,
            "params": params
        }
        
        start_time = time.time()
        
        try:
            async with self.session.post(
                self.rpc_url,
                json=payload,
                auth=self.auth,
                timeout=aiohttp.ClientTimeout(total=30)
            ) as response:
                response_time = time.time() - start_time
                
                if response.status != 200:
                    raise Exception(f"HTTP {response.status}")
                
                result = await response.json()
                
                if 'error' in result and result['error'] is not None:
                    raise Exception(f"RPC error: {result['error']}")
                
                return {
                    'result': result.get('result'),
                    'response_time': response_time
                }
                
        except Exception as e:
            response_time = time.time() - start_time
            return {
                'error': str(e),
                'response_time': response_time
            }
    
    async def setup_test_environment(self):
        """Set up test environment with addresses and UTXOs"""
        logger.info("Setting up test environment...")
        
        try:
            # Create test wallet if it doesn't exist
            await self.rpc_call('createwallet', ['stress_test'])
        except:
            # Wallet might already exist
            pass
        
        # Generate test addresses
        for i in range(100):
            result = await self.rpc_call('getnewaddress', ['stress_test'])
            if 'result' in result:
                self.test_addresses.append(result['result'])
        
        logger.info(f"Generated {len(self.test_addresses)} test addresses")
        
        # Get available UTXOs
        utxo_result = await self.rpc_call('listunspent')
        if 'result' in utxo_result:
            self.utxos = utxo_result['result'][:1000]  # Limit to 1000 UTXOs
        
        logger.info(f"Found {len(self.utxos)} available UTXOs")
    
    async def generate_transaction(self) -> Dict[str, Any]:
        """Generate a single test transaction"""
        if not self.utxos or not self.test_addresses:
            return {'error': 'No UTXOs or addresses available'}
        
        try:
            # Select random UTXO and destination
            utxo = random.choice(self.utxos)
            dest_address = random.choice(self.test_addresses)
            
            # Create transaction inputs
            inputs = [{
                'txid': utxo['txid'],
                'vout': utxo['vout']
            }]
            
            # Calculate amount (leave some for fees)
            amount = max(0.001, utxo['amount'] - 0.0001)  # 0.0001 DIN fee
            
            # Create transaction outputs
            outputs = {dest_address: amount}
            
            # Create raw transaction
            raw_tx_result = await self.rpc_call('createrawtransaction', [inputs, outputs])
            if 'error' in raw_tx_result:
                return raw_tx_result
            
            raw_tx = raw_tx_result['result']
            
            # Sign transaction
            signed_result = await self.rpc_call('signrawtransactionwithwallet', [raw_tx])
            if 'error' in signed_result:
                return signed_result
            
            if not signed_result['result'].get('complete', False):
                return {'error': 'Transaction signing incomplete'}
            
            signed_tx = signed_result['result']['hex']
            
            # Send transaction
            send_result = await self.rpc_call('sendrawtransaction', [signed_tx])
            return send_result
            
        except Exception as e:
            return {'error': str(e)}
    
    async def stress_test_burst(self, num_transactions: int, concurrent_workers: int = 10) -> TestResult:
        """Run burst stress test"""
        logger.info(f"Starting burst test: {num_transactions} transactions, {concurrent_workers} workers")
        
        start_time = time.time()
        results = []
        response_times = []
        
        # Create semaphore to limit concurrent requests
        semaphore = asyncio.Semaphore(concurrent_workers)
        
        async def worker():
            async with semaphore:
                result = await self.generate_transaction()
                results.append(result)
                if 'response_time' in result:
                    response_times.append(result['response_time'])
                return result
        
        # Execute all transactions concurrently
        tasks = [worker() for _ in range(num_transactions)]
        await asyncio.gather(*tasks, return_exceptions=True)
        
        end_time = time.time()
        duration = end_time - start_time
        
        # Calculate statistics
        successful = sum(1 for r in results if 'result' in r and 'error' not in r)
        failed = len(results) - successful
        tps = num_transactions / duration if duration > 0 else 0
        
        if response_times:
            avg_response_time = statistics.mean(response_times)
            min_response_time = min(response_times)
            max_response_time = max(response_times)
            p95_response_time = statistics.quantiles(response_times, n=20)[18]  # 95th percentile
            p99_response_time = statistics.quantiles(response_times, n=100)[98]  # 99th percentile
        else:
            avg_response_time = min_response_time = max_response_time = 0
            p95_response_time = p99_response_time = 0
        
        return TestResult(
            total_transactions=num_transactions,
            successful_transactions=successful,
            failed_transactions=failed,
            duration=duration,
            tps=tps,
            avg_response_time=avg_response_time,
            min_response_time=min_response_time,
            max_response_time=max_response_time,
            p95_response_time=p95_response_time,
            p99_response_time=p99_response_time
        )
    
    async def stress_test_sustained(self, target_tps: float, duration_seconds: int) -> TestResult:
        """Run sustained load stress test"""
        logger.info(f"Starting sustained test: {target_tps} TPS for {duration_seconds} seconds")
        
        start_time = time.time()
        end_time = start_time + duration_seconds
        results = []
        response_times = []
        
        interval = 1.0 / target_tps  # Time between transactions
        
        while time.time() < end_time:
            batch_start = time.time()
            
            # Generate transaction
            result = await self.generate_transaction()
            results.append(result)
            
            if 'response_time' in result:
                response_times.append(result['response_time'])
            
            # Wait for next interval
            elapsed = time.time() - batch_start
            if elapsed < interval:
                await asyncio.sleep(interval - elapsed)
        
        total_duration = time.time() - start_time
        
        # Calculate statistics
        successful = sum(1 for r in results if 'result' in r and 'error' not in r)
        failed = len(results) - successful
        actual_tps = len(results) / total_duration if total_duration > 0 else 0
        
        if response_times:
            avg_response_time = statistics.mean(response_times)
            min_response_time = min(response_times)
            max_response_time = max(response_times)
            p95_response_time = statistics.quantiles(response_times, n=20)[18] if len(response_times) >= 20 else max_response_time
            p99_response_time = statistics.quantiles(response_times, n=100)[98] if len(response_times) >= 100 else max_response_time
        else:
            avg_response_time = min_response_time = max_response_time = 0
            p95_response_time = p99_response_time = 0
        
        return TestResult(
            total_transactions=len(results),
            successful_transactions=successful,
            failed_transactions=failed,
            duration=total_duration,
            tps=actual_tps,
            avg_response_time=avg_response_time,
            min_response_time=min_response_time,
            max_response_time=max_response_time,
            p95_response_time=p95_response_time,
            p99_response_time=p99_response_time
        )
    
    def print_results(self, result: TestResult, test_name: str):
        """Print test results"""
        print(f"\n{'='*60}")
        print(f"STRESS TEST RESULTS: {test_name}")
        print(f"{'='*60}")
        print(f"Total Transactions:     {result.total_transactions:,}")
        print(f"Successful:             {result.successful_transactions:,} ({result.successful_transactions/result.total_transactions*100:.1f}%)")
        print(f"Failed:                 {result.failed_transactions:,} ({result.failed_transactions/result.total_transactions*100:.1f}%)")
        print(f"Duration:               {result.duration:.2f} seconds")
        print(f"Throughput:             {result.tps:.2f} TPS")
        print(f"")
        print(f"Response Times:")
        print(f"  Average:              {result.avg_response_time*1000:.2f} ms")
        print(f"  Minimum:              {result.min_response_time*1000:.2f} ms")
        print(f"  Maximum:              {result.max_response_time*1000:.2f} ms")
        print(f"  95th Percentile:      {result.p95_response_time*1000:.2f} ms")
        print(f"  99th Percentile:      {result.p99_response_time*1000:.2f} ms")
        print(f"{'='*60}")

async def main():
    """Main stress testing application"""
    parser = argparse.ArgumentParser(description='Dinero Stress Tester')
    parser.add_argument('--rpc-url', default='http://127.0.0.1:8332/', help='RPC URL')
    parser.add_argument('--rpc-user', help='RPC username')
    parser.add_argument('--rpc-password', help='RPC password')
    parser.add_argument('--test-type', choices=['burst', 'sustained'], default='burst', help='Test type')
    parser.add_argument('--transactions', type=int, default=100, help='Number of transactions (burst test)')
    parser.add_argument('--workers', type=int, default=10, help='Concurrent workers (burst test)')
    parser.add_argument('--tps', type=float, default=10.0, help='Target TPS (sustained test)')
    parser.add_argument('--duration', type=int, default=60, help='Test duration in seconds (sustained test)')
    
    args = parser.parse_args()
    
    async with DineroCoinStressTester(args.rpc_url, args.rpc_user, args.rpc_password) as tester:
        # Set up test environment
        await tester.setup_test_environment()
        
        if args.test_type == 'burst':
            result = await tester.stress_test_burst(args.transactions, args.workers)
            tester.print_results(result, f"Burst Test ({args.transactions} transactions, {args.workers} workers)")
        else:
            result = await tester.stress_test_sustained(args.tps, args.duration)
            tester.print_results(result, f"Sustained Test ({args.tps} TPS, {args.duration}s)")

if __name__ == '__main__':
    asyncio.run(main())

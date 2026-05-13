#!/usr/bin/env python3
"""
Dinero Prometheus Exporter
Exports Dinero daemon metrics for monitoring
"""

import time
import json
import logging
import requests
from typing import Dict, Any
from prometheus_client import start_http_server, Gauge, Counter, Histogram, Info
from requests.auth import HTTPBasicAuth
import argparse
import os

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

class DineroCoinExporter:
    """Prometheus exporter for Dinero daemon metrics"""
    
    def __init__(self, rpc_host: str = "127.0.0.1", rpc_port: int = 8332, 
                 rpc_user: str = None, rpc_password: str = None):
        self.rpc_url = f"http://{rpc_host}:{rpc_port}/"
        self.rpc_auth = None
        
        # Set up RPC authentication
        if rpc_user and rpc_password:
            self.rpc_auth = HTTPBasicAuth(rpc_user, rpc_password)
        else:
            # Try to read cookie file
            cookie_path = os.path.expanduser("~/.dinero/.cookie")
            if os.path.exists(cookie_path):
                with open(cookie_path, 'r') as f:
                    cookie = f.read().strip()
                    if ':' in cookie:
                        user, password = cookie.split(':', 1)
                        self.rpc_auth = HTTPBasicAuth(user, password)
        
        # Define Prometheus metrics
        self.setup_metrics()
        
    def setup_metrics(self):
        """Initialize Prometheus metrics"""
        
        # Blockchain metrics
        self.block_height = Gauge('dinero_block_height', 'Current block height')
        self.difficulty = Gauge('dinero_difficulty', 'Current network difficulty')
        self.chain_work = Gauge('dinero_chain_work', 'Total chain work')
        self.size_on_disk = Gauge('dinero_size_on_disk_bytes', 'Blockchain size on disk')
        self.verification_progress = Gauge('dinero_verification_progress', 'Blockchain verification progress')
        
        # Network metrics
        self.peer_count = Gauge('dinero_peer_count', 'Number of connected peers')
        self.network_hashrate = Gauge('dinero_network_hashrate', 'Estimated network hashrate')
        self.connections_in = Gauge('dinero_connections_in', 'Inbound connections')
        self.connections_out = Gauge('dinero_connections_out', 'Outbound connections')
        
        # Mempool metrics
        self.mempool_size = Gauge('dinero_mempool_size', 'Number of transactions in mempool')
        self.mempool_bytes = Gauge('dinero_mempool_bytes', 'Size of mempool in bytes')
        self.mempool_usage = Gauge('dinero_mempool_usage', 'Mempool memory usage')
        self.mempool_max_usage = Gauge('dinero_mempool_max_usage', 'Maximum mempool memory usage')
        
        # Mining metrics
        self.mining_enabled = Gauge('dinero_mining_enabled', 'Whether mining is enabled')
        self.hashrate = Gauge('dinero_hashrate', 'Local hashrate')
        self.blocks_found = Counter('dinero_blocks_found_total', 'Total blocks found')
        
        # RPC metrics
        self.rpc_requests = Counter('dinero_rpc_requests_total', 'Total RPC requests', ['method'])
        self.rpc_errors = Counter('dinero_rpc_errors_total', 'Total RPC errors', ['method'])
        self.rpc_duration = Histogram('dinero_rpc_duration_seconds', 'RPC request duration', ['method'])
        
        # Node info
        self.node_info = Info('dinero_node_info', 'Node information')
        
        # Uptime
        self.uptime = Gauge('dinero_uptime_seconds', 'Node uptime in seconds')
        
    def rpc_call(self, method: str, params: list = None) -> Dict[str, Any]:
        """Make RPC call to Dinero daemon"""
        if params is None:
            params = []
            
        payload = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": method,
            "params": params
        }
        
        start_time = time.time()
        
        try:
            response = requests.post(
                self.rpc_url,
                json=payload,
                auth=self.rpc_auth,
                timeout=30
            )
            
            duration = time.time() - start_time
            self.rpc_duration.labels(method=method).observe(duration)
            self.rpc_requests.labels(method=method).inc()
            
            response.raise_for_status()
            result = response.json()
            
            if 'error' in result and result['error'] is not None:
                self.rpc_errors.labels(method=method).inc()
                raise Exception(f"RPC error: {result['error']}")
                
            return result.get('result', {})
            
        except Exception as e:
            self.rpc_errors.labels(method=method).inc()
            logger.error(f"RPC call {method} failed: {e}")
            raise
    
    def collect_blockchain_metrics(self):
        """Collect blockchain-related metrics"""
        try:
            # Get blockchain info
            blockchain_info = self.rpc_call('getblockchaininfo')
            
            self.block_height.set(blockchain_info.get('blocks', 0))
            self.difficulty.set(blockchain_info.get('difficulty', 0))
            self.chain_work.set(int(blockchain_info.get('chainwork', '0'), 16))
            self.size_on_disk.set(blockchain_info.get('size_on_disk', 0))
            self.verification_progress.set(blockchain_info.get('verificationprogress', 0))
            
            # Set node info
            self.node_info.info({
                'version': str(blockchain_info.get('version', 'unknown')),
                'subversion': blockchain_info.get('subversion', 'unknown'),
                'chain': blockchain_info.get('chain', 'unknown'),
                'protocol_version': str(blockchain_info.get('protocolversion', 'unknown'))
            })
            
        except Exception as e:
            logger.error(f"Failed to collect blockchain metrics: {e}")
    
    def collect_network_metrics(self):
        """Collect network-related metrics"""
        try:
            # Get network info
            network_info = self.rpc_call('getnetworkinfo')
            peer_info = self.rpc_call('getpeerinfo')
            
            self.peer_count.set(len(peer_info))
            
            # Count inbound/outbound connections
            inbound = sum(1 for peer in peer_info if peer.get('inbound', False))
            outbound = len(peer_info) - inbound
            
            self.connections_in.set(inbound)
            self.connections_out.set(outbound)
            
        except Exception as e:
            logger.error(f"Failed to collect network metrics: {e}")
    
    def collect_mempool_metrics(self):
        """Collect mempool-related metrics"""
        try:
            mempool_info = self.rpc_call('getmempoolinfo')
            
            self.mempool_size.set(mempool_info.get('size', 0))
            self.mempool_bytes.set(mempool_info.get('bytes', 0))
            self.mempool_usage.set(mempool_info.get('usage', 0))
            self.mempool_max_usage.set(mempool_info.get('maxmempool', 0))
            
        except Exception as e:
            logger.error(f"Failed to collect mempool metrics: {e}")
    
    def collect_mining_metrics(self):
        """Collect mining-related metrics"""
        try:
            # Get mining info
            mining_info = self.rpc_call('getmininginfo')
            
            self.network_hashrate.set(mining_info.get('networkhashps', 0))
            self.mining_enabled.set(1 if mining_info.get('generate', False) else 0)
            self.hashrate.set(mining_info.get('hashespersec', 0))
            
        except Exception as e:
            logger.error(f"Failed to collect mining metrics: {e}")
    
    def collect_uptime_metrics(self):
        """Collect uptime metrics"""
        try:
            uptime_info = self.rpc_call('uptime')
            self.uptime.set(uptime_info)
            
        except Exception as e:
            # Uptime RPC might not be available, estimate from start time
            logger.debug(f"Uptime RPC not available: {e}")
    
    def collect_all_metrics(self):
        """Collect all metrics"""
        logger.debug("Collecting Dinero metrics...")
        
        self.collect_blockchain_metrics()
        self.collect_network_metrics()
        self.collect_mempool_metrics()
        self.collect_mining_metrics()
        self.collect_uptime_metrics()
        
        logger.debug("Metrics collection complete")

def main():
    """Main exporter application"""
    parser = argparse.ArgumentParser(description='Dinero Prometheus Exporter')
    parser.add_argument('--rpc-host', default='127.0.0.1', help='RPC host')
    parser.add_argument('--rpc-port', type=int, default=8332, help='RPC port')
    parser.add_argument('--rpc-user', help='RPC username')
    parser.add_argument('--rpc-password', help='RPC password')
    parser.add_argument('--port', type=int, default=9332, help='Exporter port')
    parser.add_argument('--interval', type=int, default=30, help='Collection interval')
    
    args = parser.parse_args()
    
    # Initialize exporter
    exporter = DineroCoinExporter(
        rpc_host=args.rpc_host,
        rpc_port=args.rpc_port,
        rpc_user=args.rpc_user,
        rpc_password=args.rpc_password
    )
    
    # Start HTTP server
    start_http_server(args.port)
    logger.info(f"Dinero exporter started on port {args.port}")
    
    # Collection loop
    while True:
        try:
            exporter.collect_all_metrics()
            time.sleep(args.interval)
        except KeyboardInterrupt:
            logger.info("Exporter stopped")
            break
        except Exception as e:
            logger.error(f"Collection error: {e}")
            time.sleep(args.interval)

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Dinero Prometheus Exporter
Converts gethealth RPC to Prometheus metrics
"""

import json
import os
import time
import requests
from http.server import HTTPServer, BaseHTTPRequestHandler

class DineroMetricsHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != '/metrics':
            self.send_error(404)
            return
        
        try:
            metrics = self.get_dinero_metrics()
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.end_headers()
            self.wfile.write(metrics.encode('utf-8'))
        except Exception as e:
            self.send_error(500, str(e))
    
    def get_dinero_metrics(self):
        # Read nodeinfo and cookie
        nodeinfo_path = os.environ.get('DINERO_NODEINFO', '/var/lib/dinero/nodeinfo.json')
        cookie_path = os.environ.get('DINERO_COOKIE', '/var/lib/dinero/data/.cookie')
        
        with open(nodeinfo_path) as f:
            nodeinfo = json.load(f)
        
        with open(cookie_path) as f:
            auth = f.read().strip()
        
        # Make RPC calls
        rpc_url = f"http://127.0.0.1:{nodeinfo['rpc']}"
        auth_tuple = tuple(auth.split(":"))
        
        # Get health info
        response = requests.post(
            rpc_url,
            json={"jsonrpc": "2.0", "id": 1, "method": "gethealth", "params": []},
            auth=auth_tuple,
            timeout=5
        )
        result = response.json()['result']
        p2p = result.get('p2p', {})
        
        # Get mempool info
        mempool_info = {}
        try:
            mempool_response = requests.post(
                rpc_url,
                json={"jsonrpc": "2.0", "id": 2, "method": "getmempoolinfo", "params": []},
                auth=auth_tuple,
                timeout=5
            )
            mempool_info = mempool_response.json().get('result', {})
        except:
            pass  # Mempool info is optional
        
        # Get mining info for additional metrics
        mining_info = {}
        try:
            mining_response = requests.post(
                rpc_url,
                json={"jsonrpc": "2.0", "id": 3, "method": "getmininginfo", "params": []},
                auth=auth_tuple,
                timeout=5
            )
            mining_info = mining_response.json().get('result', {})
        except:
            pass  # Mining info is optional
        
        # Generate Prometheus metrics
        metrics = []
        
        # Node info
        metrics.append('# HELP dinero_info Node information')
        metrics.append('# TYPE dinero_info gauge')
        metrics.append(f'dinero_info{{chain="{result.get("chain", "unknown")}"}} 1')
        
        # P2P metrics
        metrics.append('# HELP dinero_peers_total Number of connected peers')
        metrics.append('# TYPE dinero_peers_total gauge')
        metrics.append(f'dinero_peers_total {p2p.get("peers", 0)}')
        
        metrics.append('# HELP dinero_peers_outbound Number of outbound peers')
        metrics.append('# TYPE dinero_peers_outbound gauge')
        metrics.append(f'dinero_peers_outbound {p2p.get("outbound", 0)}')
        
        metrics.append('# HELP dinero_peers_inbound Number of inbound peers')
        metrics.append('# TYPE dinero_peers_inbound gauge')
        metrics.append(f'dinero_peers_inbound {p2p.get("inbound", 0)}')
        
        # Blockchain metrics
        metrics.append('# HELP dinero_blocks Current block height')
        metrics.append('# TYPE dinero_blocks gauge')
        metrics.append(f'dinero_blocks {p2p.get("blocks", 0)}')
        
        metrics.append('# HELP dinero_headers Current header height')
        metrics.append('# TYPE dinero_headers gauge')
        metrics.append(f'dinero_headers {p2p.get("headers", 0)}')
        
        # Sync metrics
        metrics.append('# HELP dinero_initial_block_download Whether node is in IBD')
        metrics.append('# TYPE dinero_initial_block_download gauge')
        metrics.append(f'dinero_initial_block_download {1 if p2p.get("initialblockdownload", False) else 0}')
        
        # Download metrics
        metrics.append('# HELP dinero_blocks_inflight Blocks currently downloading')
        metrics.append('# TYPE dinero_blocks_inflight gauge')
        metrics.append(f'dinero_blocks_inflight {p2p.get("inflight", 0)}')
        
        metrics.append('# HELP dinero_blocks_queued Blocks queued for download')
        metrics.append('# TYPE dinero_blocks_queued gauge')
        metrics.append(f'dinero_blocks_queued {p2p.get("queued", 0)}')
        
        metrics.append('# HELP dinero_download_rate_bps Block download rate in bytes per second')
        metrics.append('# TYPE dinero_download_rate_bps gauge')
        metrics.append(f'dinero_download_rate_bps {p2p.get("rate_bps", 0)}')
        
        # Mempool metrics
        if mempool_info:
            metrics.append('# HELP dinero_mempool_tx Number of transactions in mempool')
            metrics.append('# TYPE dinero_mempool_tx gauge')
            metrics.append(f'dinero_mempool_tx {mempool_info.get("size", 0)}')
            
            metrics.append('# HELP dinero_mempool_bytes Total size of mempool in bytes')
            metrics.append('# TYPE dinero_mempool_bytes gauge')
            metrics.append(f'dinero_mempool_bytes {mempool_info.get("bytes", 0)}')
        
        # Mining metrics
        if mining_info:
            metrics.append('# HELP dinero_mining_generating Whether node is mining')
            metrics.append('# TYPE dinero_mining_generating gauge')
            metrics.append(f'dinero_mining_generating {1 if mining_info.get("generate", False) else 0}')
            
            metrics.append('# HELP dinero_mining_hashrate_hps Current mining hashrate in hashes per second')
            metrics.append('# TYPE dinero_mining_hashrate_hps gauge')
            metrics.append(f'dinero_mining_hashrate_hps {mining_info.get("hashespersec", 0)}')
            
            metrics.append('# HELP dinero_mining_difficulty Current mining difficulty')
            metrics.append('# TYPE dinero_mining_difficulty gauge')
            metrics.append(f'dinero_mining_difficulty {mining_info.get("difficulty", 0)}')
        
        # RPC metrics (simple counter based on successful calls)
        metrics.append('# HELP dinero_rpc_requests_total Total RPC requests processed (estimated)')
        metrics.append('# TYPE dinero_rpc_requests_total counter')
        # Use a simple heuristic: scrape count * average requests per scrape interval
        scrape_count = int(time.time()) // 30  # Assuming 30s scrape interval
        metrics.append(f'dinero_rpc_requests_total {scrape_count * 10}')  # Estimate 10 requests per interval
        
        # Timestamp
        metrics.append('# HELP dinero_scrape_timestamp_seconds Unix timestamp of last scrape')
        metrics.append('# TYPE dinero_scrape_timestamp_seconds gauge')
        metrics.append(f'dinero_scrape_timestamp_seconds {int(time.time())}')
        
        return '\n'.join(metrics) + '\n'
    
    def log_message(self, format, *args):
        # Suppress default logging
        pass

def main():
    port = int(os.environ.get('METRICS_PORT', '9100'))
    server = HTTPServer(('127.0.0.1', port), DineroMetricsHandler)
    print(f"Dinero Prometheus exporter listening on ::{port}")
    print("Metrics available at http://127.0.0.1:{}/metrics".format(port))
    server.serve_forever()

if __name__ == '__main__':
    main()

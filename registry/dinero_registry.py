#!/usr/bin/env python3
"""
Dinero Global Node Registry
A lightweight service that periodically fetches serverinfo.json from known nodes
and exposes a unified network map via REST API.
"""

import http.server
import json
import threading
import time
import requests
from urllib.parse import urlparse
from datetime import datetime
from typing import Dict, List, Optional
import argparse
import sys

# --- CONFIGURATION ---
DEFAULT_NODES = [
    "http://173.249.195.59:21999/serverinfo.json",  # Virginia
    "http://172.93.160.131:21999/serverinfo.json",  # California
    "http://127.0.0.1:21999/serverinfo.json"        # Local
]

REFRESH_INTERVAL = 60  # seconds
REQUEST_TIMEOUT = 5    # seconds
MAX_HISTORY = 100      # Keep last N snapshots for uptime tracking

# --- GLOBAL STATE ---
node_data = {
    "timestamp": None,
    "total_nodes": 0,
    "nodes": []
}
node_history: List[Dict] = []  # Historical snapshots
node_stats: Dict[str, Dict] = {}  # Per-node statistics
lock = threading.Lock()

# --- UTILITY FUNCTIONS ---
def measure_latency(url: str, timeout: int = 3) -> Optional[float]:
    """Measure HTTP request latency in milliseconds"""
    try:
        start = time.time()
        r = requests.get(url, timeout=timeout)
        latency = (time.time() - start) * 1000
        return round(latency, 2) if r.status_code == 200 else None
    except Exception:
        return None

def extract_node_id(url: str) -> str:
    """Extract unique identifier from URL (IP:port)"""
    parsed = urlparse(url)
    return f"{parsed.hostname}:{parsed.port}"

def update_node_stats(node_id: str, success: bool, latency: Optional[float]):
    """Track per-node reliability statistics"""
    if node_id not in node_stats:
        node_stats[node_id] = {
            "total_queries": 0,
            "successful_queries": 0,
            "failed_queries": 0,
            "avg_latency_ms": 0,
            "last_seen": None,
            "uptime_percentage": 100.0
        }

    stats = node_stats[node_id]
    stats["total_queries"] += 1

    if success:
        stats["successful_queries"] += 1
        stats["last_seen"] = datetime.utcnow().isoformat() + "Z"

        # Update rolling average latency
        if latency is not None:
            current_avg = stats["avg_latency_ms"]
            total = stats["successful_queries"]
            stats["avg_latency_ms"] = round(
                ((current_avg * (total - 1)) + latency) / total, 2
            )
    else:
        stats["failed_queries"] += 1

    # Calculate uptime percentage
    if stats["total_queries"] > 0:
        stats["uptime_percentage"] = round(
            (stats["successful_queries"] / stats["total_queries"]) * 100, 2
        )

# --- REGISTRY REFRESH THREAD ---
def refresh_registry(node_urls: List[str]):
    """Periodically fetch and aggregate node information"""
    global node_data, node_history

    print(f"[Registry] Monitoring {len(node_urls)} nodes")
    print(f"[Registry] Refresh interval: {REFRESH_INTERVAL}s")

    while True:
        new_data = {
            "timestamp": datetime.utcnow().isoformat() + "Z",
            "nodes": []
        }

        for url in node_urls:
            node_id = extract_node_id(url)
            try:
                # Measure latency
                start_time = time.time()
                r = requests.get(url, timeout=REQUEST_TIMEOUT)
                latency_ms = round((time.time() - start_time) * 1000, 2)

                if r.status_code == 200:
                    node = r.json()
                    node["source_url"] = url
                    node["latency_ms"] = latency_ms

                    # Add reliability stats
                    if node_id in node_stats:
                        node["uptime_percentage"] = node_stats[node_id]["uptime_percentage"]
                        node["avg_latency_ms"] = node_stats[node_id]["avg_latency_ms"]

                    new_data["nodes"].append(node)
                    update_node_stats(node_id, True, latency_ms)
                    print(f"[OK] {node_id} - {latency_ms}ms - {node.get('name', 'Unknown')}")
                else:
                    update_node_stats(node_id, False, None)
                    print(f"[WARN] {node_id} returned HTTP {r.status_code}")

            except requests.exceptions.Timeout:
                update_node_stats(node_id, False, None)
                print(f"[WARN] {node_id} - timeout after {REQUEST_TIMEOUT}s")
            except requests.exceptions.ConnectionError:
                update_node_stats(node_id, False, None)
                print(f"[WARN] {node_id} - connection failed")
            except Exception as e:
                update_node_stats(node_id, False, None)
                print(f"[ERROR] {node_id} - {type(e).__name__}: {e}")

        new_data["total_nodes"] = len(new_data["nodes"])
        new_data["total_configured"] = len(node_urls)

        with lock:
            node_data = new_data

            # Store in history (circular buffer)
            node_history.append({
                "timestamp": new_data["timestamp"],
                "total_nodes": new_data["total_nodes"]
            })
            if len(node_history) > MAX_HISTORY:
                node_history.pop(0)

        print(f"[Registry] Updated: {new_data['total_nodes']}/{len(node_urls)} nodes alive\n")
        time.sleep(REFRESH_INTERVAL)

# --- HTTP SERVER ---
class RegistryHandler(http.server.BaseHTTPRequestHandler):
    def _set_headers(self, code=200, content_type='application/json'):
        self.send_response(code)
        self.send_header('Content-type', content_type)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def do_OPTIONS(self):
        """Handle CORS preflight"""
        self._set_headers(204)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == '/nodes.json' or path == '/api/nodes':
            # Main registry endpoint
            with lock:
                data = json.dumps(node_data, indent=2)
            self._set_headers()
            self.wfile.write(data.encode('utf-8'))

        elif path == '/api/status':
            # Quick status check
            with lock:
                total = node_data.get("total_nodes", 0)
                configured = node_data.get("total_configured", 0)
                timestamp = node_data.get("timestamp", "unknown")

            status = {
                "status": "ok",
                "total_nodes_alive": total,
                "total_nodes_configured": configured,
                "last_update": timestamp
            }
            self._set_headers()
            self.wfile.write(json.dumps(status, indent=2).encode('utf-8'))

        elif path == '/api/stats':
            # Per-node statistics
            with lock:
                stats_copy = dict(node_stats)
            self._set_headers()
            self.wfile.write(json.dumps(stats_copy, indent=2).encode('utf-8'))

        elif path == '/api/history':
            # Historical uptime data
            with lock:
                history_copy = list(node_history)
            self._set_headers()
            self.wfile.write(json.dumps(history_copy, indent=2).encode('utf-8'))

        elif path == '/' or path == '/index.html':
            # Simple status page
            html = self._generate_status_page()
            self._set_headers(content_type='text/html')
            self.wfile.write(html.encode('utf-8'))

        else:
            self._set_headers(404)
            error = {"error": "not found", "path": path}
            self.wfile.write(json.dumps(error).encode('utf-8'))

    def _generate_status_page(self) -> str:
        """Generate a simple HTML status dashboard"""
        with lock:
            nodes = node_data.get("nodes", [])
            timestamp = node_data.get("timestamp", "N/A")
            total = node_data.get("total_nodes", 0)

        rows = ""
        for node in nodes:
            name = node.get("name", "Unknown")
            network = node.get("network", "?")
            connections = node.get("connections", 0)
            ws_port = node.get("ws_port", "?")
            uptime = node.get("uptime", 0)
            latency = node.get("latency_ms", "?")
            uptime_pct = node.get("uptime_percentage", "?")
            features = ", ".join(node.get("features", []))

            # Format uptime as human-readable
            uptime_str = f"{uptime // 3600}h {(uptime % 3600) // 60}m"

            rows += f"""
            <tr>
                <td><strong>{name}</strong></td>
                <td>{network}</td>
                <td>{connections}</td>
                <td>{ws_port}</td>
                <td>{uptime_str}</td>
                <td>{latency}ms</td>
                <td>{uptime_pct}%</td>
                <td><small>{features}</small></td>
            </tr>
            """

        return f"""
        <!DOCTYPE html>
        <html>
        <head>
            <title>Dinero Node Registry</title>
            <meta charset="UTF-8">
            <meta http-equiv="refresh" content="60">
            <style>
                body {{ font-family: monospace; margin: 20px; background: #1a1a1a; color: #e0e0e0; }}
                h1 {{ color: #4fc3f7; }}
                table {{ border-collapse: collapse; width: 100%; margin-top: 20px; }}
                th, td {{ border: 1px solid #444; padding: 8px; text-align: left; }}
                th {{ background-color: #2c2c2c; color: #4fc3f7; }}
                tr:hover {{ background-color: #2a2a2a; }}
                .meta {{ color: #888; font-size: 0.9em; }}
                .api {{ margin-top: 30px; padding: 15px; background: #2c2c2c; border-radius: 5px; }}
                code {{ color: #4fc3f7; }}
            </style>
        </head>
        <body>
            <h1>🌐 Dinero Global Node Registry</h1>
            <p class="meta">Last Update: {timestamp} | Total Nodes: {total} | Auto-refresh: 60s</p>

            <table>
                <tr>
                    <th>Node Name</th>
                    <th>Network</th>
                    <th>Peers</th>
                    <th>WebSocket</th>
                    <th>Uptime</th>
                    <th>Latency</th>
                    <th>Reliability</th>
                    <th>Features</th>
                </tr>
                {rows if rows else '<tr><td colspan="8">No nodes available</td></tr>'}
            </table>

            <div class="api">
                <h3>API Endpoints</h3>
                <ul>
                    <li><code>GET /nodes.json</code> - Full node registry</li>
                    <li><code>GET /api/status</code> - Quick status summary</li>
                    <li><code>GET /api/stats</code> - Per-node statistics</li>
                    <li><code>GET /api/history</code> - Historical uptime data</li>
                </ul>
            </div>
        </body>
        </html>
        """

    def log_message(self, format, *args):
        """Suppress default HTTP logging"""
        pass

def run_server(port: int, node_urls: List[str]):
    """Start the registry server"""
    # Start background refresh thread
    threading.Thread(target=refresh_registry, args=(node_urls,), daemon=True).start()

    # Give it time to fetch initial data
    time.sleep(2)

    # Start HTTP server
    server = http.server.HTTPServer(('0.0.0.0', port), RegistryHandler)
    print(f"\n{'='*60}")
    print(f"[Registry] Server running at http://0.0.0.0:{port}")
    print(f"[Registry] Access at:")
    print(f"           http://localhost:{port}/")
    print(f"           http://localhost:{port}/nodes.json")
    print(f"           http://localhost:{port}/api/status")
    print(f"{'='*60}\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[Registry] Shutting down...")
        server.shutdown()

def main():
    parser = argparse.ArgumentParser(description='Dinero Global Node Registry')
    parser.add_argument('-p', '--port', type=int, default=8080,
                        help='HTTP server port (default: 8080)')
    parser.add_argument('-n', '--nodes', type=str, nargs='+',
                        help='List of node URLs to monitor')
    parser.add_argument('-i', '--interval', type=int, default=60,
                        help='Refresh interval in seconds (default: 60)')
    parser.add_argument('-t', '--timeout', type=int, default=5,
                        help='Request timeout in seconds (default: 5)')

    args = parser.parse_args()

    # Update global config
    global REFRESH_INTERVAL, REQUEST_TIMEOUT
    REFRESH_INTERVAL = args.interval
    REQUEST_TIMEOUT = args.timeout

    # Use provided nodes or defaults
    node_urls = args.nodes if args.nodes else DEFAULT_NODES

    print("Dinero Global Node Registry")
    print("=" * 60)

    run_server(args.port, node_urls)

if __name__ == '__main__':
    main()

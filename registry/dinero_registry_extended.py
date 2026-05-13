#!/usr/bin/env python3
"""
Dinero Global Node Registry (Extended Version)
Supports both manual node configuration and self-registration via POST /api/register
"""

import http.server
import json
import threading
import time
import requests
from urllib.parse import urlparse
from datetime import datetime
from typing import Dict, List, Optional, Set
import argparse
import hashlib
import hmac

# --- CONFIGURATION ---
DEFAULT_NODES = [
    "http://173.249.195.59:21999/serverinfo.json",
    "http://172.93.160.131:21999/serverinfo.json",
]

REFRESH_INTERVAL = 60
REQUEST_TIMEOUT = 5
MAX_HISTORY = 100
MAX_REGISTERED_NODES = 100  # Prevent spam
REGISTRATION_SECRET = None   # Set via --secret flag for HMAC validation

# --- GLOBAL STATE ---
node_data = {"timestamp": None, "total_nodes": 0, "nodes": []}
node_history: List[Dict] = []
node_stats: Dict[str, Dict] = {}
registered_nodes: Set[str] = set()  # Nodes added via self-registration
lock = threading.Lock()

# --- UTILITY FUNCTIONS ---
def extract_node_id(url: str) -> str:
    """Extract unique identifier from URL"""
    parsed = urlparse(url)
    return f"{parsed.hostname}:{parsed.port}"

def validate_url(url: str) -> bool:
    """Validate URL format and scheme"""
    try:
        parsed = urlparse(url)
        return parsed.scheme in ['http', 'https'] and bool(parsed.hostname)
    except Exception:
        return False

def validate_hmac(payload: bytes, signature: str, secret: str) -> bool:
    """Validate HMAC signature for node registration"""
    if not secret:
        return True  # No secret configured, skip validation

    expected = hmac.new(
        secret.encode('utf-8'),
        payload,
        hashlib.sha256
    ).hexdigest()

    return hmac.compare_digest(expected, signature)

def update_node_stats(node_id: str, success: bool, latency: Optional[float]):
    """Track per-node reliability statistics"""
    if node_id not in node_stats:
        node_stats[node_id] = {
            "total_queries": 0,
            "successful_queries": 0,
            "failed_queries": 0,
            "avg_latency_ms": 0,
            "last_seen": None,
            "uptime_percentage": 100.0,
            "first_seen": datetime.utcnow().isoformat() + "Z",
            "registered": node_id in registered_nodes
        }

    stats = node_stats[node_id]
    stats["total_queries"] += 1

    if success:
        stats["successful_queries"] += 1
        stats["last_seen"] = datetime.utcnow().isoformat() + "Z"

        if latency is not None:
            current_avg = stats["avg_latency_ms"]
            total = stats["successful_queries"]
            stats["avg_latency_ms"] = round(
                ((current_avg * (total - 1)) + latency) / total, 2
            )
    else:
        stats["failed_queries"] += 1

    if stats["total_queries"] > 0:
        stats["uptime_percentage"] = round(
            (stats["successful_queries"] / stats["total_queries"]) * 100, 2
        )

# --- REGISTRY REFRESH THREAD ---
def refresh_registry(node_urls: List[str]):
    """Periodically fetch and aggregate node information"""
    global node_data, node_history

    print(f"[Registry] Monitoring nodes (refresh: {REFRESH_INTERVAL}s)")

    while True:
        new_data = {
            "timestamp": datetime.utcnow().isoformat() + "Z",
            "nodes": []
        }

        # Get current node list (manual + registered)
        with lock:
            all_nodes = set(node_urls) | registered_nodes

        for url in all_nodes:
            node_id = extract_node_id(url)
            try:
                start_time = time.time()
                r = requests.get(url, timeout=REQUEST_TIMEOUT)
                latency_ms = round((time.time() - start_time) * 1000, 2)

                if r.status_code == 200:
                    node = r.json()
                    node["source_url"] = url
                    node["latency_ms"] = latency_ms

                    # Add stats
                    if node_id in node_stats:
                        node["uptime_percentage"] = node_stats[node_id]["uptime_percentage"]
                        node["avg_latency_ms"] = node_stats[node_id]["avg_latency_ms"]
                        node["registered"] = node_stats[node_id].get("registered", False)

                    new_data["nodes"].append(node)
                    update_node_stats(node_id, True, latency_ms)

                    status = "✓" if node_id in registered_nodes else "●"
                    print(f"[{status}] {node_id} - {latency_ms}ms - {node.get('name', 'Unknown')}")
                else:
                    update_node_stats(node_id, False, None)

            except Exception as e:
                update_node_stats(node_id, False, None)

        new_data["total_nodes"] = len(new_data["nodes"])
        new_data["total_configured"] = len(node_urls)
        new_data["total_registered"] = len(registered_nodes)

        with lock:
            node_data = new_data
            node_history.append({
                "timestamp": new_data["timestamp"],
                "total_nodes": new_data["total_nodes"]
            })
            if len(node_history) > MAX_HISTORY:
                node_history.pop(0)

        alive = new_data['total_nodes']
        total = len(all_nodes)
        print(f"[Registry] {alive}/{total} nodes alive (manual: {len(node_urls)}, registered: {len(registered_nodes)})\n")

        time.sleep(REFRESH_INTERVAL)

# --- HTTP SERVER ---
class RegistryHandler(http.server.BaseHTTPRequestHandler):
    def _set_headers(self, code=200, content_type='application/json'):
        self.send_response(code)
        self.send_header('Content-type', content_type)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type, X-Signature')
        self.end_headers()

    def do_OPTIONS(self):
        self._set_headers(204)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == '/nodes.json' or path == '/api/nodes':
            with lock:
                data = json.dumps(node_data, indent=2)
            self._set_headers()
            self.wfile.write(data.encode('utf-8'))

        elif path == '/api/status':
            with lock:
                status = {
                    "status": "ok",
                    "total_nodes_alive": node_data.get("total_nodes", 0),
                    "total_nodes_configured": node_data.get("total_configured", 0),
                    "total_nodes_registered": node_data.get("total_registered", 0),
                    "last_update": node_data.get("timestamp", "unknown"),
                    "registration_enabled": True,
                    "max_registered_nodes": MAX_REGISTERED_NODES
                }
            self._set_headers()
            self.wfile.write(json.dumps(status, indent=2).encode('utf-8'))

        elif path == '/api/stats':
            with lock:
                stats_copy = dict(node_stats)
            self._set_headers()
            self.wfile.write(json.dumps(stats_copy, indent=2).encode('utf-8'))

        elif path == '/api/history':
            with lock:
                history_copy = list(node_history)
            self._set_headers()
            self.wfile.write(json.dumps(history_copy, indent=2).encode('utf-8'))

        elif path == '/api/registered':
            # List all registered nodes
            with lock:
                reg_list = list(registered_nodes)
            response = {
                "total": len(reg_list),
                "nodes": reg_list
            }
            self._set_headers()
            self.wfile.write(json.dumps(response, indent=2).encode('utf-8'))

        elif path == '/' or path == '/index.html':
            html = self._generate_status_page()
            self._set_headers(content_type='text/html')
            self.wfile.write(html.encode('utf-8'))

        else:
            self._set_headers(404)
            self.wfile.write(json.dumps({"error": "not found"}).encode('utf-8'))

    def do_POST(self):
        path = self.path

        if path == '/api/register':
            self._handle_registration()
        else:
            self._set_headers(404)
            self.wfile.write(json.dumps({"error": "not found"}).encode('utf-8'))

    def _handle_registration(self):
        """Handle node self-registration"""
        try:
            # Read request body
            content_length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(content_length)

            # Validate HMAC if secret is configured
            if REGISTRATION_SECRET:
                signature = self.headers.get('X-Signature', '')
                if not validate_hmac(body, signature, REGISTRATION_SECRET):
                    self._set_headers(403)
                    self.wfile.write(json.dumps({
                        "error": "invalid signature",
                        "hint": "Include X-Signature header with HMAC-SHA256"
                    }).encode('utf-8'))
                    return

            # Parse node info
            try:
                node_info = json.loads(body)
            except json.JSONDecodeError:
                self._set_headers(400)
                self.wfile.write(json.dumps({"error": "invalid json"}).encode('utf-8'))
                return

            # Extract serverinfo URL
            # Option 1: Explicit URL provided
            if 'serverinfo_url' in node_info:
                url = node_info['serverinfo_url']
            # Option 2: Construct from node info
            elif 'ip' in node_info and 'rpc_port' in node_info:
                ip = node_info['ip']
                port = node_info['rpc_port']
                url = f"http://{ip}:{port}/serverinfo.json"
            else:
                self._set_headers(400)
                self.wfile.write(json.dumps({
                    "error": "missing serverinfo_url or ip/rpc_port"
                }).encode('utf-8'))
                return

            # Validate URL
            if not validate_url(url):
                self._set_headers(400)
                self.wfile.write(json.dumps({"error": "invalid url format"}).encode('utf-8'))
                return

            # Check limit
            with lock:
                if len(registered_nodes) >= MAX_REGISTERED_NODES and url not in registered_nodes:
                    self._set_headers(429)
                    self.wfile.write(json.dumps({
                        "error": "registry full",
                        "max_nodes": MAX_REGISTERED_NODES
                    }).encode('utf-8'))
                    return

                # Add to registered nodes
                was_new = url not in registered_nodes
                registered_nodes.add(url)

            node_id = extract_node_id(url)
            status_code = 201 if was_new else 200
            response = {
                "status": "registered" if was_new else "updated",
                "node_id": node_id,
                "url": url,
                "total_registered": len(registered_nodes)
            }

            self._set_headers(status_code)
            self.wfile.write(json.dumps(response, indent=2).encode('utf-8'))

            print(f"[✓] Registered: {node_id} - {node_info.get('name', 'Unknown')}")

        except Exception as e:
            print(f"[ERROR] Registration failed: {e}")
            self._set_headers(500)
            self.wfile.write(json.dumps({"error": str(e)}).encode('utf-8'))

    def _generate_status_page(self) -> str:
        """Generate HTML status dashboard"""
        with lock:
            nodes = node_data.get("nodes", [])
            timestamp = node_data.get("timestamp", "N/A")
            total = node_data.get("total_nodes", 0)
            registered_count = node_data.get("total_registered", 0)

        rows = ""
        for node in nodes:
            name = node.get("name", "Unknown")
            network = node.get("network", "?")
            connections = node.get("connections", 0)
            ws_port = node.get("ws_port", "?")
            uptime = node.get("uptime", 0)
            latency = node.get("latency_ms", "?")
            uptime_pct = node.get("uptime_percentage", "?")
            is_registered = node.get("registered", False)
            features = ", ".join(node.get("features", []))

            uptime_str = f"{uptime // 3600}h {(uptime % 3600) // 60}m"
            reg_badge = "✓ Self-Reg" if is_registered else "Manual"

            rows += f"""
            <tr>
                <td><strong>{name}</strong> <small>({reg_badge})</small></td>
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
                .badge {{ background: #2c2c2c; padding: 5px 10px; border-radius: 3px; margin: 5px; display: inline-block; }}
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
            <div>
                <span class="badge">Total Nodes: {total}</span>
                <span class="badge">Self-Registered: {registered_count}</span>
                <span class="badge">Last Update: {timestamp}</span>
            </div>

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
                    <li><code>GET /api/registered</code> - List self-registered nodes</li>
                    <li><code>POST /api/register</code> - Node self-registration</li>
                </ul>
                <h4>Example Registration</h4>
                <pre><code>curl -X POST http://localhost:8080/api/register \\
  -H "Content-Type: application/json" \\
  -d '{{"serverinfo_url": "http://YOUR_IP:21999/serverinfo.json"}}'</code></pre>
            </div>
        </body>
        </html>
        """

    def log_message(self, format, *args):
        pass

def run_server(port: int, node_urls: List[str], secret: Optional[str]):
    global REGISTRATION_SECRET
    REGISTRATION_SECRET = secret

    if secret:
        print(f"[Security] HMAC validation enabled")
    else:
        print(f"[Security] HMAC validation disabled (any node can register)")

    threading.Thread(target=refresh_registry, args=(node_urls,), daemon=True).start()
    time.sleep(2)

    server = http.server.HTTPServer(('0.0.0.0', port), RegistryHandler)
    print(f"\n{'='*60}")
    print(f"[Registry] Server running at http://0.0.0.0:{port}")
    print(f"[Registry] Self-registration: POST /api/register")
    print(f"{'='*60}\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[Registry] Shutting down...")
        server.shutdown()

def main():
    parser = argparse.ArgumentParser(description='Dinero Global Node Registry (Extended)')
    parser.add_argument('-p', '--port', type=int, default=8080)
    parser.add_argument('-n', '--nodes', type=str, nargs='+',
                        help='Initial node URLs to monitor')
    parser.add_argument('-i', '--interval', type=int, default=60)
    parser.add_argument('-t', '--timeout', type=int, default=5)
    parser.add_argument('-s', '--secret', type=str,
                        help='HMAC secret for signature validation')
    parser.add_argument('--max-nodes', type=int, default=100,
                        help='Max registered nodes (default: 100)')

    args = parser.parse_args()

    global REFRESH_INTERVAL, REQUEST_TIMEOUT, MAX_REGISTERED_NODES
    REFRESH_INTERVAL = args.interval
    REQUEST_TIMEOUT = args.timeout
    MAX_REGISTERED_NODES = args.max_nodes

    node_urls = args.nodes if args.nodes else DEFAULT_NODES

    print("Dinero Global Node Registry (Extended)")
    print("=" * 60)

    run_server(args.port, node_urls, args.secret)

if __name__ == '__main__':
    main()

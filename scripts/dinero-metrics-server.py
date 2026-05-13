#!/usr/bin/env python3
"""
Dinero Metrics HTTP Server
Serves Prometheus-format metrics on HTTP endpoint for scraping
"""

import subprocess
import os
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

# Configuration
METRICS_SCRIPT = "/usr/local/bin/dinero-metrics-exporter.sh"
HTTP_PORT = 9100  # Standard Prometheus node exporter port
NODE_NAME = os.environ.get("NODE_NAME", "unknown")

class MetricsHandler(BaseHTTPRequestHandler):
    """HTTP handler that serves Prometheus metrics"""

    def do_GET(self):
        """Handle GET requests"""
        if self.path == "/metrics":
            try:
                # Execute metrics exporter script
                env = os.environ.copy()
                env["NODE_NAME"] = NODE_NAME
                result = subprocess.run(
                    [METRICS_SCRIPT],
                    capture_output=True,
                    text=True,
                    timeout=10,
                    env=env
                )

                if result.returncode == 0:
                    # Success - return metrics
                    self.send_response(200)
                    self.send_header("Content-Type", "text/plain; version=0.0.4")
                    self.end_headers()
                    self.wfile.write(result.stdout.encode())
                else:
                    # Script failed
                    self.send_response(500)
                    self.send_header("Content-Type", "text/plain")
                    self.end_headers()
                    error_msg = f"Error: Metrics script failed\n{result.stderr}"
                    self.wfile.write(error_msg.encode())

            except subprocess.TimeoutExpired:
                self.send_response(504)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"Error: Metrics collection timeout")
            except Exception as e:
                self.send_response(500)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(f"Error: {str(e)}".encode())

        elif self.path == "/health" or self.path == "/":
            # Health check endpoint
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"Dinero Metrics Exporter OK\n")
            self.wfile.write(f"Node: {NODE_NAME}\n".encode())
            self.wfile.write(f"Metrics available at: /metrics\n".encode())

        else:
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"Not Found\n")
            self.wfile.write(b"Available endpoints: /metrics, /health\n")

    def log_message(self, format, *args):
        """Custom log format"""
        print(f"[{self.log_date_time_string()}] {format % args}")

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    """Threaded HTTP server for handling concurrent requests"""
    daemon_threads = True

def main():
    """Start the metrics HTTP server"""
    try:
        server = ThreadedHTTPServer(("0.0.0.0", HTTP_PORT), MetricsHandler)
        print(f"Dinero Metrics Exporter")
        print(f"Node: {NODE_NAME}")
        print(f"Listening on port {HTTP_PORT}")
        print(f"Metrics endpoint: http://0.0.0.0:{HTTP_PORT}/metrics")
        print(f"Health endpoint: http://0.0.0.0:{HTTP_PORT}/health")
        print("Press Ctrl+C to stop")
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        server.shutdown()
    except Exception as e:
        print(f"Error starting server: {e}")
        exit(1)

if __name__ == "__main__":
    main()

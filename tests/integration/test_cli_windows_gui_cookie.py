#!/usr/bin/env python3
"""The packaged Windows CLI must find the GUI daemon's default RPC cookie."""

import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main(binary: Path) -> None:
    if sys.platform != "win32":
        raise SystemExit("This check requires the native Windows CLI")

    observed = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            observed.append((self.headers.get("Authorization"), request["method"]))
            method = observed[-1][1]
            result = 168956 if method == "blockchain.getblockcount" else {"orchard_history": {"ready": True}}
            body = json.dumps({"jsonrpc": "2.0", "id": 1, "result": result, "error": None}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            appdata = root / "Roaming"
            gui_data = appdata / "Dinero"
            gui_data.mkdir(parents=True)
            gui_data.joinpath(".cookie").write_text("gui:cookie-from-gui\n", encoding="ascii")
            other_data = root / "explicit"
            other_data.mkdir()
            other_data.joinpath(".cookie").write_text("operator:cookie-explicit\n", encoding="ascii")
            env = os.environ.copy()
            env["APPDATA"] = str(appdata)
            env["USERPROFILE"] = str(root / "different-profile")

            def check(args, credential):
                observed.clear()
                completed = subprocess.run(
                    [str(binary), f"-rpcport={server.server_port}", *args,
                     "blockchain.getsnapshotbootstrapstatus"],
                    env=env, capture_output=True, text=True, timeout=15)
                assert completed.returncode == 0, completed.stderr
                assert json.loads(completed.stdout)["orchard_history"]["ready"] is True
                assert [method for _, method in observed] == [
                    "blockchain.getblockcount", "blockchain.getsnapshotbootstrapstatus"]
                expected = "Basic " + base64.b64encode(credential.encode()).decode()
                assert all(auth == expected for auth, _ in observed), observed

            check([], "gui:cookie-from-gui")
            check([f"-datadir={other_data}"], "operator:cookie-explicit")
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)

    print("PASS Windows CLI GUI-default and explicit RPC cookies")


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve(strict=True))

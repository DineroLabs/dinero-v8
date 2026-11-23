#!/usr/bin/env bash
# The light Bitcoin HTTPS transport against local servers: size limit, time
# limit, no redirects, no plain http to other machines, and a certificate that
# does not check out.
#   usage: swap_btc_http_test.sh <swap_btc_http_tool>
set -euo pipefail
TOOL=${1:?swap_btc_http_tool}
command -v python3 >/dev/null && command -v openssl >/dev/null || { echo "SKIP: python3 and openssl needed"; exit 77; }
DIR=$(mktemp -d "${TMPDIR:-/tmp}/swap-btc-http.XXXXXX")
PORT=$(( 21000 + RANDOM % 20000 )); TLS=$(( PORT + 1 ))
cleanup() { [[ -n "${SPID:-}" ]] && kill "$SPID" 2>/dev/null || true; [[ -n "${TPID:-}" ]] && kill "$TPID" 2>/dev/null || true; rm -rf "$DIR"; }
trap cleanup EXIT
FAILED=0
check() { if eval "$2"; then echo "  OK   $1"; else echo "  FAIL $1"; FAILED=1; fi; }

cat >"$DIR/server.py" <<'PY'
import http.server, ssl, sys, time
connections = [0]
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'  # connections stay open between requests, as on the real servers
    def setup(self):
        connections[0] += 1
        super().setup()
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == '/connections':
            body = str(connections[0]).encode()
            self.close_connection = True
            self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
        elif self.path == '/ok':
            body = b'hello'
            self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
        elif self.path == '/big':
            self.close_connection = True
            self.send_response(200); self.end_headers()
            try:
                for _ in range(2000): self.wfile.write(b'x' * 10000)
            except Exception: pass
        elif self.path == '/redirect':
            self.send_response(302); self.send_header('Location', '/ok'); self.send_header('Content-Length', '0'); self.end_headers()
        elif self.path == '/slow':
            time.sleep(8); self.send_response(200); self.send_header('Content-Length', '2'); self.end_headers(); self.wfile.write(b'ok')
        elif self.path == '/half':
            self.send_response(200); self.send_header('Content-Length', '100'); self.end_headers(); self.wfile.write(b'short')
        else:
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers()
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', 0)))
        self.send_response(200); self.send_header('Content-Length', '2'); self.end_headers(); self.wfile.write(b'ok')
port = int(sys.argv[1])
srv = http.server.ThreadingHTTPServer(('127.0.0.1', port), H)
if len(sys.argv) > 2:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[2], sys.argv[3])
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()
PY
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$DIR/key.pem" -out "$DIR/cert.pem" -days 2 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1   # for the name "localhost" only
python3 "$DIR/server.py" $PORT & SPID=$!
python3 "$DIR/server.py" $TLS "$DIR/cert.pem" "$DIR/key.pem" & TPID=$!
for _ in $(seq 1 50); do (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null && (echo > /dev/tcp/127.0.0.1/$TLS) 2>/dev/null && break; sleep 0.2; done
B="http://127.0.0.1:$PORT"

check "a normal reply"                              '[[ "$("$TOOL" get $B/ok 1000 5)" == "status 200 bytes 5" ]]'
check "a missing page keeps its status"             '[[ "$("$TOOL" get $B/none 1000 5)" == "status 404 bytes 0" ]]'
check "a reply over the limit is cut one byte past" '[[ "$("$TOOL" get $B/big 50000 10)" == "status 200 bytes 50001" ]]'
check "a redirect is reported, not followed"        '[[ "$("$TOOL" get $B/redirect 1000 5)" == "status 302 bytes 0" ]]'
START=$(date +%s)
check "a server that stalls is given up on"         '[[ "$("$TOOL" get $B/slow 1000 2)" == "status 0 bytes 0" ]]'
check "within the time limit"                       '(( $(date +%s) - START <= 5 ))'
check "a reply cut short is no answer"              '[[ "$("$TOOL" get $B/half 1000 3)" == "status 0 bytes 0" ]]'
check "nobody listening is no answer"               '[[ "$("$TOOL" get http://127.0.0.1:$((PORT + 5))/ok 1000 3)" == "status 0 bytes 0" ]]'
check "plain http to another machine is never sent" '[[ "$("$TOOL" get http://example.com/ 1000 5)" == "status 0 bytes 0" ]]'
check "other schemes are refused"                   '[[ "$("$TOOL" get file:///etc/hosts 100000 5)" == "status 0 bytes 0" ]]'

echo "=== one connection for a run of requests; a stop interrupts ==="
conns() { curl -s -m 5 "$B/connections"; }
before=$(conns)
check "eight requests in a row all answer"          '[[ "$("$TOOL" gets $B/ok 8)" == "ok 8" ]]'
after=$(conns)   # each look at the counter is itself one connection
check "over one connection, not eight ($((after - before - 1)))" '(( after - before - 1 == 1 ))'
check "a stalled request is interrupted at once, and nothing is sent after" \
  '[[ "$("$TOOL" interrupt $B/slow)" == "status 0 then 0 quickly" ]]'

check "a server that hung is not asked again for a while; others, and anything being sent, are" \
  '[[ "$("$TOOL" rest $B/slow $B/ok http://localhost:$PORT/ok)" == "hung 0 same 0 at once other 200 post 200" ]]'
check "a refusal on the spot does not rest a server" '[[ "$("$TOOL" rest http://127.0.0.1:$((PORT + 5))/ok $B/ok $B/ok)" == *"other 200"* ]]'

echo "=== certificates ==="
check "a certificate nobody vouches for is no answer" '[[ "$("$TOOL" get https://localhost:$TLS/ok 1000 5)" == "status 0 bytes 0" ]]'
check "the same server, trusted explicitly, answers"  '[[ "$("$TOOL" get https://localhost:$TLS/ok 1000 5 "$DIR/cert.pem")" == "status 200 bytes 5" ]]'
# The same trusted certificate, reached under a name it was not issued for.
check "a trusted certificate for another name is no answer" \
  '[[ "$("$TOOL" get https://127.0.0.1:$TLS/ok 1000 5 "$DIR/cert.pem")" == "status 0 bytes 0" ]]'

if [[ $FAILED == 0 ]]; then echo "SWAP BTC HTTP: PASS"; else echo "SWAP BTC HTTP: FAIL"; exit 1; fi

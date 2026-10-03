# Retain parallel-download startup failure evidence

The failed Linux run at f3f46f5f showed node C failing to start, but its final 80 log lines contained shutdown and omitted the initiating error. This change uses the existing failure-log helper to include the beginning of each node log and startup error matches as well as the prior final tail. The full-test workflow losslessly compresses the preserved exact test stdout .log files into its existing test artifact. It does not collect cookies, wallet or database files.

Node startup, RPC/peer assertions, port selection, polling counts and deadlines remain unchanged. This repairs diagnostic retention; the historical startup cause remains unconfirmed. A passing subsequent run is not proof of that cause or its repair. Mainnet and deployment remain unchanged.

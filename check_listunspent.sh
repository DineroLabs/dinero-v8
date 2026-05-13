#!/bin/bash
COOKIE=$(cat ~/.dinero-regtest/.cookie | cut -d: -f2)
curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.listunspent","params":[],"id":99}'

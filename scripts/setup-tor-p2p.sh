#!/usr/bin/env bash
set -euo pipefail

DATADIR="${DINERO_DATADIR:-$HOME/.dinero}"
CONF="${DATADIR}/dinero.conf"

mkdir -p "$DATADIR"
touch "$CONF"

set_config() {
  local key="$1"
  local value="$2"
  if grep -Eq "^[[:space:]]*${key}[[:space:]]*=" "$CONF"; then
    sed -i.bak -E "s|^[[:space:]]*${key}[[:space:]]*=.*|${key}=${value}|" "$CONF"
  else
    printf '%s=%s\n' "$key" "$value" >> "$CONF"
  fi
}

set_config "listen" "1"
set_config "onion" "auto"

echo "Updated $CONF"
echo "  listen=1"
echo "  onion=auto"
echo

if command -v nc >/dev/null 2>&1; then
  if nc -z 127.0.0.1 9050 >/dev/null 2>&1; then
    echo "Detected system Tor SOCKS5 on 127.0.0.1:9050."
    exit 0
  fi
  if nc -z 127.0.0.1 9150 >/dev/null 2>&1; then
    echo "Detected Tor Browser SOCKS5 on 127.0.0.1:9150."
    exit 0
  fi
fi

echo "Tor SOCKS5 was not detected yet."
case "$(uname -s)" in
  Darwin)
    echo "macOS: brew install tor && brew services start tor"
    ;;
  Linux)
    echo "Debian/Ubuntu: sudo apt install tor && sudo systemctl enable --now tor"
    ;;
  *)
    echo "Start Tor locally on 127.0.0.1:9050, or Tor Browser on 127.0.0.1:9150."
    ;;
esac

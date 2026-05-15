#!/usr/bin/env bash
# Drift test for the P2P network magic values.
#
# Asserts two things:
#
#  1. seeder/include/dinero/seeder/network_constants_generated.h is in
#     sync with src/consensus/chainparams_impl.cpp. Reruns the generator
#     in --check mode; non-zero exit means a developer changed the
#     canonical magic without regenerating the seeder header.
#
#  2. No file outside the canonical sources contains a hardcoded value
#     for any of the three magics that DISAGREES with chainparams. This
#     catches the "main daemon uses 0xD1A0C0DE but seeder probes
#     0xD1A0C0DF" silent-divergence bug. Allowed sites: the canonical
#     chainparams source itself, the generated seeder header, and the
#     generator script that reads / writes them.
#
# The "no copies anywhere" stronger check (forbid even matching copies)
# is intentionally NOT enforced here because the daemon-side refactor to
# read from Params().magic at runtime is being landed incrementally;
# enforcing zero copies would block the build until every site is done.
# The drift-only check still catches the real wire-protocol risk.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CHAINPARAMS="$ROOT/src/consensus/chainparams_impl.cpp"

python3 "$ROOT/tools/sync_network_constants_headers.py" --check

extract_magic() {
  # Find `.name = "<chain>"` then grab the first `.magic = 0xN` that
  # follows. sed's hold/pattern space lets us do this in one pass.
  local chain="$1"
  sed -nE '
    /^[[:space:]]*\.name[[:space:]]*=[[:space:]]*"'"$chain"'"/,/\.magic[[:space:]]*=/{
      s/^[[:space:]]*\.magic[[:space:]]*=[[:space:]]*0x([0-9A-Fa-f]+)u?.*/\1/p
    }
  ' "$CHAINPARAMS" | head -1 | tr '[:lower:]' '[:upper:]'
}

MAINNET="$(extract_magic mainnet)"
TESTNET="$(extract_magic testnet)"
REGTEST="$(extract_magic regtest)"

if [[ -z "$MAINNET" || -z "$TESTNET" || -z "$REGTEST" ]]; then
  echo "FAIL: could not extract one or more magic values from $CHAINPARAMS" >&2
  echo "  mainnet=$MAINNET testnet=$TESTNET regtest=$REGTEST" >&2
  exit 1
fi

# Sites allowed to contain a literal magic value:
#   - chainparams_impl.cpp (the canonical source)
#   - the generated seeder header
#   - the generator script
#   - this drift test
#   - test fixtures (allow params_stub.cpp + anything under tests/)
ALLOWED_GLOBS=(
  "src/consensus/chainparams_impl.cpp"
  "seeder/include/dinero/seeder/network_constants_generated.h"
  "tools/sync_network_constants_headers.py"
  "tests/integration/test_network_magic_sync.sh"
  "src/crypto/params_stub.cpp"
  "tests/"
  # rc/markdown notes can mention the values too — allow docs/release/etc.
  "*.md"
)

build_ignore_args() {
  local args=()
  for glob in "${ALLOWED_GLOBS[@]}"; do
    args+=("--glob=!$glob")
  done
  printf '%s\n' "${args[@]}"
}
IGNORE_ARGS=()
while IFS= read -r line; do IGNORE_ARGS+=("$line"); done < <(build_ignore_args)

# Drift scan: look for occurrences of the *canonical* magic values
# anywhere outside the allowed sites — those occurrences are duplicates,
# but per the comment at the top we don't fail on duplicates yet. What
# we DO fail on is any source file that contains a value that LOOKS LIKE
# a Dinero magic (8-hex-digit literal in a `magic` / `NetworkMagic` /
# `NET_MAGIC` / `MAGIC_BYTES` context) but is NOT one of the three
# canonical values — that's silent divergence in flight.
SUSPECT_PATTERN='0x[0-9A-Fa-f]{8}u?[^0-9A-Fa-f]*//.*magic|(\bNetworkMagic\b|\bNET_MAGIC\b|\bMAGIC_BYTES\b|\bMAGIC_MAINNET\b|\bMAGIC_TESTNET\b|\bMAGIC_REGTEST\b)[^;]*0x([0-9A-Fa-f]+)'

if rg --line-number --no-heading --pcre2 \
      "${IGNORE_ARGS[@]}" \
      --type=cpp --type=h \
      "$SUSPECT_PATTERN" "$ROOT" >/tmp/network_magic_hits 2>/dev/null
then
  drift=0
  while IFS=: read -r file lineno text; do
    # Extract every 8-hex-digit literal from this line and compare.
    while IFS= read -r hit; do
      val="$(echo "$hit" | tr '[:lower:]' '[:upper:]')"
      case "$val" in
        "$MAINNET"|"$TESTNET"|"$REGTEST") ;;
        *)
          echo "DRIFT: $file:$lineno  literal 0x$val does not match canonical {mainnet=$MAINNET, testnet=$TESTNET, regtest=$REGTEST}" >&2
          echo "       line: $text" >&2
          drift=$((drift + 1))
          ;;
      esac
    done < <(echo "$text" | grep -oE '0x[0-9A-Fa-f]{8}' | sed 's/^0x//')
  done < /tmp/network_magic_hits
  rm -f /tmp/network_magic_hits
  if [[ $drift -gt 0 ]]; then
    echo "FAIL: $drift network-magic drift site(s) detected." >&2
    exit 1
  fi
fi

echo "NETWORK_MAGIC_SYNC=PASS (canonical: mainnet=0x$MAINNET testnet=0x$TESTNET regtest=0x$REGTEST)"

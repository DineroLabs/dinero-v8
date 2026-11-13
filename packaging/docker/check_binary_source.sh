#!/bin/sh
# Check the compiled daemon's identity independently of its archive receipt.
set -eu
binary="${1:?daemon path required}"
expected_sha="${2:?source SHA required}"
expected_version="${3:?version required}"

printf '%s\n' "$expected_sha" | grep -Eq '^[0-9a-f]{40}$' || {
    echo 'invalid expected source SHA' >&2
    exit 1
}
identity="$("$binary" --version)"
printf '%s\n' "$identity" | grep -Fx "dinerod $expected_version" >/dev/null || {
    echo 'daemon version differs from the qualified source version' >&2
    exit 1
}
printf '%s\n' "$identity" | grep -Fx 'component: dinerod' >/dev/null || {
    echo 'daemon build identity has the wrong component' >&2
    exit 1
}
printf '%s\n' "$identity" | grep -Fx "commit: $expected_sha" >/dev/null || {
    echo 'daemon commit differs from the qualified source SHA' >&2
    exit 1
}

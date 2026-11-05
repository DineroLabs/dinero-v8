#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 )) || [[ ! $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || [[ ! $2 =~ ^[0-9a-f]{40}$ ]]; then
  echo "usage: $0 VERSION EXPECTED_SOURCE_SHA" >&2
  exit 2
fi

version=$1
expected_source=$2
source_root=$(git rev-parse --show-toplevel)
if [[ $(pwd -P) != "$(cd "$source_root" && pwd -P)" || $(git rev-parse HEAD) != "$expected_source" ]]; then
  echo "release source identity or working directory changed before Debian packaging" >&2
  exit 1
fi

out="dinero-linux-x86_64-$version"
debug_stage="$out-debug-symbols-stage"
release_archive="$out.tar.gz"
debug_archive="$out-debug-symbols.tar.gz"
release_sums="SHA256SUMS-linux-x86_64-$version"
debug_sums="SHA256SUMS-linux-x86_64-debug-symbols-$version"
disposable=(build-release "$out" "$debug_stage" tower-tar-check swap-tower-install)

for path in "$release_archive" "$debug_archive" "$release_sums" "$debug_sums"; do
  test -s "$path" || { echo "missing release artifact: $path" >&2; exit 1; }
done
for path in "${disposable[@]}"; do
  test -d "$path" && test ! -L "$path" || {
    echo "missing or linked disposable build directory: $path" >&2
    exit 1
  }
done

echo "Disk space before first-build cleanup:"
df -h .
du -sh "${disposable[@]}" "$release_archive" "$debug_archive"

# Check both the compressed archives and their constituent binaries before
# deleting the unpacked trees. The debug member checksums are relative to
# debug_stage, unlike the archive checksum on the first line.
sha256sum -c "$release_sums"
head -n 1 "$debug_sums" | sha256sum -c -
tail -n +2 "$debug_sums" | (cd "$debug_stage" && sha256sum -c -)
tar -tzf "$release_archive" >/dev/null
tar -tzf "$debug_archive" >/dev/null

rm -rf -- "${disposable[@]}"

echo "Disk space after first-build cleanup:"
df -h .
du -sh "$release_archive" "$debug_archive" "$release_sums" "$debug_sums"

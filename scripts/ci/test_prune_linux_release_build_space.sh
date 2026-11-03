#!/usr/bin/env bash
set -euo pipefail

script=$(cd "$(dirname "$0")" && pwd)/prune_linux_release_build_space.sh
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
version=8.2.0
out="dinero-linux-x86_64-$version"
debug_stage="$out-debug-symbols-stage"

make_fixture() {
  local dir=$1 bin
  mkdir -p "$dir"
  git -C "$dir" init -q
  printf 'fixture\n' > "$dir/README"
  git -C "$dir" add README
  git -C "$dir" -c user.name=CI -c user.email=ci@example.invalid commit -qm fixture
  mkdir -p "$dir/build-release" "$dir/$out" "$dir/$debug_stage/$out/.debug" \
    "$dir/tower-tar-check" "$dir/swap-tower-install"
  printf 'large disposable build\n' > "$dir/build-release/objects"
  for bin in dinerod dinero-cli dinero-swap-tower; do
    printf '%s release binary\n' "$bin" > "$dir/$out/$bin"
    printf '%s debug symbols\n' "$bin" > "$dir/$debug_stage/$out/.debug/$bin.debug"
  done
  (
    cd "$dir"
    tar -czf "$out.tar.gz" "$out"
    tar -czf "$out-debug-symbols.tar.gz" -C "$debug_stage" "$out"
    sha256sum "$out.tar.gz" "$out/dinerod" "$out/dinero-cli" \
      "$out/dinero-swap-tower" > "SHA256SUMS-linux-x86_64-$version"
    sha256sum "$out-debug-symbols.tar.gz" > "SHA256SUMS-linux-x86_64-debug-symbols-$version"
    (
      cd "$debug_stage"
      sha256sum "$out/.debug/dinerod.debug" "$out/.debug/dinero-cli.debug" \
        "$out/.debug/dinero-swap-tower.debug"
    ) >> "SHA256SUMS-linux-x86_64-debug-symbols-$version"
  )
}

make_fixture "$tmp/pass"
sha=$(git -C "$tmp/pass" rev-parse HEAD)
(
  cd "$tmp/pass"
  sha256sum "$out.tar.gz" "$out-debug-symbols.tar.gz" \
    "SHA256SUMS-linux-x86_64-$version" "SHA256SUMS-linux-x86_64-debug-symbols-$version" \
    > "$tmp/before.sha256"
  bash "$script" "$version" "$sha" > "$tmp/pass.log"
  for path in build-release "$out" "$debug_stage" tower-tar-check swap-tower-install; do
    test ! -e "$path"
  done
  sha256sum -c "$tmp/before.sha256" > /dev/null
)

make_fixture "$tmp/wrong-source"
if (cd "$tmp/wrong-source" && bash "$script" "$version" "$(printf '0%.0s' {1..40})") \
  > "$tmp/wrong-source.log" 2>&1; then
  echo 'wrong source SHA was accepted' >&2
  exit 1
fi
test -d "$tmp/wrong-source/build-release"

make_fixture "$tmp/corrupt-archive"
printf 'corruption' >> "$tmp/corrupt-archive/$out.tar.gz"
sha=$(git -C "$tmp/corrupt-archive" rev-parse HEAD)
if (cd "$tmp/corrupt-archive" && bash "$script" "$version" "$sha") \
  > "$tmp/corrupt-archive.log" 2>&1; then
  echo 'corrupt archive was accepted' >&2
  exit 1
fi
test -d "$tmp/corrupt-archive/build-release"

echo 'first-build cleanup preserves verified archives and fails closed on bad source or archive'

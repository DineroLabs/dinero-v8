#!/usr/bin/env bash
# Build and run the covenant fuzz harness under fail-fast UBSan and a separately
# triaged unsigned-overflow pass. Mainnet activation parameters are untouched.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
iterations=200000
jobs=4
ubsan_build="${repo_root}/build-covfuzz-ubsan"
uio_build="${repo_root}/build-covfuzz-uio"

usage() {
    echo "Usage: $0 [--iterations N] [--jobs N]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iterations)
            [[ $# -ge 2 && "$2" =~ ^[1-9][0-9]*$ ]] || { usage >&2; exit 2; }
            iterations="$2"
            shift 2
            ;;
        --jobs)
            [[ $# -ge 2 && "$2" =~ ^[1-9][0-9]*$ ]] || { usage >&2; exit 2; }
            jobs="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

common_cmake=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_C_COMPILER=clang
    -DCMAKE_CXX_COMPILER=clang++
    -DDINERO_BUILD_QT=OFF
    -DDINERO_BUILD_MINER=OFF
)

echo "== Fail-fast undefined-behavior pass =="
cmake -S "${repo_root}" -B "${ubsan_build}" "${common_cmake[@]}" \
    -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
cmake --build "${ubsan_build}" --target test_covenant_fuzz --parallel "${jobs}"
env DINERO_COVENANT_FUZZ_ITERATIONS="${iterations}" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    "${ubsan_build}/test_covenant_fuzz"

echo "== Unsigned-overflow collection pass =="
cmake -S "${repo_root}" -B "${uio_build}" "${common_cmake[@]}" \
    -DCMAKE_C_FLAGS="-fsanitize=undefined,unsigned-integer-overflow -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fsanitize=undefined,unsigned-integer-overflow -fno-omit-frame-pointer"
cmake --build "${uio_build}" --target test_covenant_fuzz --parallel "${jobs}"

uio_log="${uio_build}/covenant-unsigned-overflow.log"
set +e
env DINERO_COVENANT_FUZZ_ITERATIONS="${iterations}" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0" \
    "${uio_build}/test_covenant_fuzz" >"${uio_log}" 2>&1
uio_status=$?
set -e
if [[ ${uio_status} -ne 0 ]]; then
    tail -100 "${uio_log}" >&2
    echo "Unsigned-overflow campaign exited ${uio_status}" >&2
    exit "${uio_status}"
fi

primary_diagnostics="${uio_build}/covenant-unsigned-overflow-primary.txt"
sed -n '/^\/.*: runtime error:/p' "${uio_log}" >"${primary_diagnostics}"

# SHA-256 and secp256k1 implement arithmetic modulo fixed-width integers, so
# unsigned wrap there is expected. Any primary diagnostic elsewhere is a new
# finding even when its stack later passes through covenant code.
unexpected="${uio_build}/covenant-unsigned-overflow-unexpected.txt"
if grep -Ev '/src/crypto/sha256\.cpp:|/third_party/secp256k1-zkp/' \
        "${primary_diagnostics}" >"${unexpected}"; then
    echo "Unexpected unsigned-overflow diagnostics:" >&2
    cat "${unexpected}" >&2
    exit 1
fi

tail -20 "${uio_log}"
echo "Primary unsigned-overflow locations:"
sed -E 's/: runtime error:.*//' "${primary_diagnostics}" | sort | uniq -c
echo "PASS: no primary sanitizer diagnostic in covenant production paths"
echo "Logs: ${uio_log}"

#!/usr/bin/env bash
# Enforce the first-party C++ language contract without imposing Dinero's
# toolchain policy on vendored projects.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

FILES="$(git ls-files -- ':!:third_party/**' ':!:build*/**' \
    'CMakeLists.txt' 'CMakeLists*.txt' '**/CMakeLists.txt' '**/CMakeLists*.txt' \
    '*.cmake' '*.sh' '*.yml' '*.yaml' \
    | grep -vE '^(cmake/ThirdParty\.cmake|tools/check_first_party_cxx20\.sh)$')"

if printf '%s\n' "${FILES}" | xargs grep -nE \
    'CXX_STANDARD[[:space:]]+(98|03|11|14|17)|cxx_std_(98|11|14|17)|std=(c|gnu)\+\+(98|03|11|14|17)|/std:c\+\+(14|17)' \
    2>/dev/null; then
    echo "ERROR: first-party code requests a C++ language mode older than C++20" >&2
    exit 1
fi

if printf '%s\n' "${FILES}" | xargs grep -nE 'std=gnu\+\+|CXX_EXTENSIONS[[:space:]]+ON' 2>/dev/null; then
    echo "ERROR: first-party code enables non-standard C++ compiler extensions" >&2
    exit 1
fi

require_policy() {
    local file="$1"
    grep -Eq 'CMAKE_CXX_STANDARD[[:space:]]+20' "${file}" || {
        echo "ERROR: ${file} does not declare C++20" >&2
        exit 1
    }
    grep -Eq 'CMAKE_CXX_STANDARD_REQUIRED[[:space:]]+ON' "${file}" || {
        echo "ERROR: ${file} does not require its declared C++ standard" >&2
        exit 1
    }
    grep -Eq 'CMAKE_CXX_EXTENSIONS[[:space:]]+OFF' "${file}" || {
        echo "ERROR: ${file} does not disable C++ compiler extensions" >&2
        exit 1
    }
}

require_policy CMakeLists.txt
require_policy qt/CMakeLists.txt
require_policy cli/CMakeLists.txt
require_policy miner/CMakeLists.txt
require_policy seeder/CMakeLists.txt

echo "PASS: first-party CMake policy consistently requires strict C++20"

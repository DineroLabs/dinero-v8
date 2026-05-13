#!/bin/bash
# Wrapper for g++ that force-includes our MinGW compatibility header
exec /usr/bin/x86_64-w64-mingw32-g++ -include /build/third_party/rocksdb/mingw_compat.h "$@"

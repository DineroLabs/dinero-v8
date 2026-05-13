#!/usr/bin/env python3
"""
Patch RocksDB source files for MinGW compatibility.
Adds required threading headers to files that use std::mutex, std::condition_variable, etc.
This script modifies files IN PLACE - use only in Docker builds.
"""

import os
import sys
import re
from pathlib import Path

# Headers to inject
THREADING_HEADERS = """#ifndef MINGW_THREADING_COMPAT_INJECTED
#define MINGW_THREADING_COMPAT_INJECTED
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#endif
"""

def needs_threading_headers(content):
    """Check if file uses threading primitives without including headers."""
    # Look for std::mutex, std::condition_variable, std::thread usage
    patterns = [
        r'std::mutex\b',
        r'std::condition_variable\b',
        r'std::thread\b',
        r'std::unique_lock\b',
        r'std::lock_guard\b',
    ]
    
    for pattern in patterns:
        if re.search(pattern, content):
            # Check if already includes the headers
            if not re.search(r'#include\s*[<"]mutex[>"]', content):
                return True
            if not re.search(r'#include\s*[<"]condition_variable[>"]', content):
                return True
    
    return False

def patch_file(filepath):
    """Patch a single file by injecting threading headers after first include."""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
        
        # Skip if already patched
        if 'MINGW_THREADING_COMPAT_INJECTED' in content:
            return False
        
        # Check if file needs patching
        if not needs_threading_headers(content):
            return False
        
        # Find first #include statement
        include_pattern = r'^(#include\s+[<"][^>"]+[>"])'
        match = re.search(include_pattern, content, re.MULTILINE)
        
        if match:
            # Insert headers after first include
            insert_pos = match.end()
            patched_content = (
                content[:insert_pos] + '\n' + THREADING_HEADERS + content[insert_pos:]
            )
            
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(patched_content)
            
            return True
        else:
            # No includes found, prepend to file
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(THREADING_HEADERS + '\n' + content)
            return True
            
    except Exception as e:
        print(f"⚠️  Error patching {filepath}: {e}", file=sys.stderr)
        return False

def patch_rocksdb_directory(rocksdb_path):
    """Recursively patch all .h and .cc files in RocksDB directory."""
    rocksdb_path = Path(rocksdb_path)
    
    if not rocksdb_path.exists():
        print(f"❌ RocksDB path not found: {rocksdb_path}", file=sys.stderr)
        return 1
    
    print(f"🔍 Scanning RocksDB directory: {rocksdb_path}")
    
    # Find all source files
    patterns = ['**/*.h', '**/*.cc', '**/*.hpp', '**/*.cpp']
    files_to_check = []
    
    for pattern in patterns:
        files_to_check.extend(rocksdb_path.glob(pattern))
    
    print(f"📁 Found {len(files_to_check)} source files")
    
    patched_count = 0
    for filepath in files_to_check:
        if patch_file(filepath):
            patched_count += 1
            print(f"✅ Patched: {filepath.relative_to(rocksdb_path)}")
    
    print(f"\n🎉 Patching complete: {patched_count} files modified")
    return 0

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 patch_rocksdb_mingw.py <rocksdb_directory>")
        print("Example: python3 patch_rocksdb_mingw.py third_party/rocksdb")
        sys.exit(1)
    
    rocksdb_dir = sys.argv[1]
    sys.exit(patch_rocksdb_directory(rocksdb_dir))

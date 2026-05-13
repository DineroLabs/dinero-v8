#!/usr/bin/env python3
"""
Update CMakeLists.txt to use vendored secp256k1 library
Replaces all Homebrew references with vendored static library
"""

import sys
import re
from pathlib import Path

def update_cmake(cmake_file):
    """Update CMakeLists.txt with vendored library references"""
    
    # Read original
    with open(cmake_file, 'r') as f:
        content = f.read()
    
    # Create backup
    backup_file = cmake_file.parent / f"{cmake_file.name}.before-vendored"
    with open(backup_file, 'w') as f:
        f.write(content)
    print(f"📋 Backup created: {backup_file}")
    
    # Count original references
    homebrew_refs = content.count('/opt/homebrew')
    print(f"\n🔍 Found {homebrew_refs} Homebrew references")
    
    # Replace /opt/homebrew/lib/libsecp256k1.dylib with vendored path
    content = content.replace(
        '/opt/homebrew/lib/libsecp256k1.dylib',
        '${CMAKE_SOURCE_DIR}/vendor/lib/libsecp256k1.a'
    )
    
    # Replace standalone secp256k1 in target_link_libraries
    # Be careful to only replace in linking contexts
    content = re.sub(
        r'(\s+)secp256k1(\s*\))',
        r'\1${CMAKE_SOURCE_DIR}/vendor/lib/libsecp256k1.a\2',
        content
    )
    
    # Add vendor include directory after "Include directories" comment
    if '${CMAKE_SOURCE_DIR}/vendor/include' not in content:
        content = content.replace(
            '# Include directories\n',
            '# Include directories\n'
            'include_directories(${CMAKE_SOURCE_DIR}/vendor/include)\n'
        )
    
    # Write updated file
    with open(cmake_file, 'w') as f:
        f.write(content)
    
    # Count new references
    vendored_refs = content.count('vendor/lib/libsecp256k1.a')
    homebrew_refs_after = content.count('/opt/homebrew')
    
    print(f"\n✅ CMakeLists.txt updated!")
    print(f"   • Vendored references: {vendored_refs}")
    print(f"   • Remaining Homebrew refs: {homebrew_refs_after}")
    
    return True

def main():
    script_dir = Path(__file__).parent
    cmake_file = script_dir / "CMakeLists.txt"
    
    if not cmake_file.exists():
        print(f"❌ CMakeLists.txt not found at {cmake_file}")
        return 1
    
    # Check if vendor lib exists
    vendor_lib = script_dir / "vendor" / "lib" / "libsecp256k1.a"
    if not vendor_lib.exists():
        print(f"❌ Vendored library not found at {vendor_lib}")
        print("   Run: ./build-vendored-secp256k1.sh first")
        return 1
    
    print("🔧 Updating CMakeLists.txt for Vendored secp256k1")
    print("=" * 50)
    print()
    
    try:
        update_cmake(cmake_file)
        print("\n🎯 Next step: Test the build")
        print("   mkdir -p build-vendored && cd build-vendored")
        print("   cmake .. && make -j$(sysctl -n hw.ncpu)")
        return 0
    except Exception as e:
        print(f"\n❌ Error: {e}")
        return 1

if __name__ == "__main__":
    sys.exit(main())

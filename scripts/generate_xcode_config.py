#!/usr/bin/env python3
"""
Xcode Project Configuration Generator
Generates a configuration file that can be referenced when configuring Xcode
"""

import os
import json

config = {
    "project_root": "/Users/haydarevich/Documents/DineroiOS",
    "ffi_path": "$(SRCROOT)/Dinero/Dinero/FFI",
    "other_linker_flags": [
        "-force_load $(SRCROOT)/Dinero/Dinero/FFI/libdinero_wallet_ffi.a",
        "-force_load $(SRCROOT)/Dinero/Dinero/FFI/libjsoncpp.a",
        "-lc++",
        "-lz",
        "-lsqlite3",
        "-ObjC"
    ],
    "link_binary_with_libraries": [
        {
            "name": "libdinero_wallet_ffi.a",
            "path": "$(SRCROOT)/Dinero/Dinero/FFI/libdinero_wallet_ffi.a",
            "embed": False
        },
        {
            "name": "libjsoncpp.a",
            "path": "$(SRCROOT)/Dinero/Dinero/FFI/libjsoncpp.a",
            "embed": False
        },
        {
            "name": "libdinero_wallet.a",
            "path": "$(SRCROOT)/Dinero/Dinero/FFI/libdinero_wallet.a",
            "embed": False
        },
        {
            "name": "libdinero_crypto.a",
            "path": "$(SRCROOT)/Dinero/Dinero/FFI/libdinero_crypto.a",
            "embed": False
        }
    ],
    "library_search_paths": [
        "$(SRCROOT)/Dinero/Dinero/FFI"
    ],
    "header_search_paths": [
        "$(SRCROOT)/Dinero/Dinero/FFI"
    ]
}

# Verify libraries exist
ffi_dir = "/Users/haydarevich/Documents/DineroiOS/Dinero/Dinero/FFI"
missing_libs = []

for lib in config["link_binary_with_libraries"]:
    lib_path = os.path.join(ffi_dir, lib["name"])
    if not os.path.exists(lib_path):
        missing_libs.append(lib["name"])
    else:
        size = os.path.getsize(lib_path)
        lib["size_bytes"] = size
        lib["size_mb"] = round(size / 1024 / 1024, 2)

# Save configuration
output_file = "/Users/haydarevich/Documents/DineroCoin/xcode_config.json"
with open(output_file, 'w') as f:
    json.dump(config, f, indent=2)

print("✅ Generated Xcode configuration file:")
print(f"   {output_file}")
print("")
if missing_libs:
    print("⚠️  Missing libraries:")
    for lib in missing_libs:
        print(f"   • {lib}")
else:
    print("✅ All libraries found:")
    for lib in config["link_binary_with_libraries"]:
        print(f"   • {lib['name']} ({lib.get('size_mb', 0)} MB)")
print("")
print("📋 Copy these settings to Xcode:")
print("")
print("Other Linker Flags:")
for flag in config["other_linker_flags"]:
    print(f"   {flag}")
print("")
print("Library Search Paths:")
for path in config["library_search_paths"]:
    print(f"   {path}")


# OpenSSL Removal Script

This script systematically removes OpenSSL dependencies from the entire Dinero Coin codebase and replaces them with our internal Bitcoin Core-style crypto implementation.

## Features

- **Automatic Detection**: Finds all source files that contain OpenSSL dependencies
- **Comprehensive Replacement**: Removes includes, replaces function calls, and updates build system
- **Smart Processing**: Handles C++, C, and CMake files appropriately
- **Detailed Reporting**: Generates comprehensive reports of all changes made
- **Dry Run Mode**: Preview changes before applying them
- **Safe Operation**: Excludes build artifacts and binary files

## Usage

### Basic Usage
```bash
# Run the script to remove OpenSSL
python3 scripts/remove_openssl.py

# Or use the executable directly
./scripts/remove_openssl.py
```

### Dry Run Mode (Recommended First)
```bash
# See what would be changed without modifying files
python3 scripts/remove_openssl.py --dry-run

# With verbose output
python3 scripts/remove_openssl.py --dry-run --verbose
```

### Verbose Mode
```bash
# See detailed processing information
python3 scripts/remove_openssl.py --verbose
```

## What Gets Replaced

### OpenSSL Includes
- `#include <openssl/...>` → Removed
- `#include "openssl/...` → Removed
- `#include <OpenSSL/...` → Removed

### Function Replacements
- `RAND_bytes()` → `CF_GeneratePrivKey()`
- `SHA256()` → `sha256()`
- `RIPEMD160()` → `ripemd160()`
- `HMAC()` → `hmac_sha512()`
- `ECDSA_sign()` → `CF_SignDER()`
- `ECDSA_verify()` → `CF_VerifyDER()`
- And many more...

### Build System Updates
- `find_package(OpenSSL)` → Commented out
- `OpenSSL::SSL` → `dinero_crypto`
- `OpenSSL::Crypto` → `dinero_crypto`
- `OPENSSL_INCLUDE_DIR` → Removed

## Safety Features

- **Excludes build directories**: Won't touch `build/`, `.git/`, etc.
- **Backup-friendly**: Only modifies source files, not binaries
- **Dry run mode**: Preview all changes before applying
- **Error handling**: Reports any issues encountered
- **Comprehensive logging**: Tracks all changes made

## Output

The script generates:
1. **Console output**: Real-time progress and summary
2. **Report file**: `openssl_removal_report.txt` with detailed changes
3. **Error log**: Any issues encountered during processing

## Example Output

```
OpenSSL Removal Report
======================

Summary:
- Total files processed: 45
- Files modified: 23
- Total changes made: 67
- Errors encountered: 0

Modified Files:
===============

src/wallet/address.cpp:
  - Removed 15 OpenSSL includes
  - Made 8 function replacements

src/daemon/rpc_server.cpp:
  - Removed 8 OpenSSL includes
  - Made 12 function replacements
  - Replaced OpenSSL::SSL/OpenSSL::Crypto with dinero_crypto
```

## After Running

1. **Review the report** to understand what was changed
2. **Build the project** to ensure everything compiles
3. **Run tests** to verify functionality works correctly
4. **Commit changes** if everything looks good

## Troubleshooting

### Common Issues

- **Permission errors**: Make sure the script is executable (`chmod +x`)
- **Python version**: Requires Python 3.6+
- **File encoding**: Script handles UTF-8 files automatically

### If Something Goes Wrong

1. Check the error log in the console output
2. Review the detailed report file
3. Use `git status` to see what files were modified
4. Use `git checkout -- <file>` to revert specific files if needed

## Integration with Build System

After running the script, you may need to:

1. **Clean build directory**: `rm -rf build/`
2. **Reconfigure**: `cmake -B build`
3. **Rebuild**: `cmake --build build`

## Notes

- The script is designed to be safe and non-destructive
- It automatically adds `#include "crypto/dinero_crypto_minimal.h"` where needed
- Complex OpenSSL patterns may need manual review after automated replacement
- Always test thoroughly after running the script

## Support

If you encounter issues:
1. Check the console output for error messages
2. Review the generated report file
3. Ensure your internal crypto implementation is complete
4. Consider running in dry-run mode first to preview changes

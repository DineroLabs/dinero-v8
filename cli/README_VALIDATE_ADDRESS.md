# Address Validation CLI Tool

A standalone command-line tool for validating Base58Check addresses in the Dinero project.

## Features

- ✅ **Base58Check Decoding**: Decodes Base58Check addresses and validates checksums
- ✅ **Address Type Detection**: Identifies P2PKH and P2SH address types
- ✅ **Comprehensive Validation**: Shows version byte, payload, and full decoded data
- ✅ **Round-trip Verification**: Re-encodes addresses to verify correctness
- ✅ **Error Handling**: Provides clear error messages for invalid addresses
- ✅ **Beautiful Output**: Uses Unicode symbols and formatted output

## Usage

```bash
# Basic usage
./validate_address <Base58Check address>

# Examples
./validate_address 1EFN6ZQDF4xLH8wQNtreE5GoCQzaYA8zd4
./validate_address 1HvqfCdrYytWB9X9PDkEgEU7RRjxepSoLa
./validate_address 3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy

# Show usage help
./validate_address
```

## Output Example

```
🔍 Validating address: 1EFN6ZQDF4xLH8wQNtreE5GoCQzaYA8zd4
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ Address is valid!
• Version byte: 0x00
• Payload (usually pubkey hash): 9151118e030b790a4e001d2393300e8683afa299
• Total decoded length: 21 bytes
• Type: P2PKH (Pay-to-PubKey-Hash) - Mainnet
• Hash160 length: 20 bytes (correct)
• Full decoded data: 009151118e030b790a4e001d2393300e8683afa299
• Re-encoding verification: ✅ PASSED
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
🎯 Address validation completed successfully!
```

## Error Example

```
🔍 Validating address: invalid_address
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
❌ Invalid Base58Check address: invalid_address
   • Check if the address format is correct
   • Verify the checksum is valid
```

## Supported Address Types

- **P2PKH (Pay-to-PubKey-Hash)**: Version byte `0x00` - Legacy addresses starting with '1'
- **P2SH (Pay-to-Script-Hash)**: Version byte `0x05` - Script addresses starting with '3'

## Technical Details

The tool integrates with the existing Dinero project structure:

- Uses the `dinero::Address` class for address validation
- Leverages the robust Base58Check implementation
- Integrates with OpenSSL 3.0 for cryptographic operations
- Built as part of the main CMake build system

## Building

The tool is automatically built as part of the main Dinero project:

```bash
# Build the entire project (includes validate_address)
cd build
cmake ..
make

# The tool will be available at:
./src/cli/validate_address
```

## Integration

This tool is designed to be:
- **Standalone**: Can be used independently of other Dinero components
- **Integrated**: Built as part of the main project structure
- **Extensible**: Easy to add new address types and validation rules
- **Production-ready**: Includes comprehensive error handling and testing

## Future Enhancements

Potential future features:
- WIF (Wallet Import Format) decoding
- Address checksum regeneration
- Public key to address conversion pipeline
- Support for Bech32 addresses
- Batch validation of multiple addresses 
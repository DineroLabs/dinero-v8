# Dinero RPC Standards Specification

**Version:** 1.0.0  
**Date:** August 20, 2025  
**Status:** Active  
**Compliance Level:** Required

## Overview

This document defines the standards for Dinero RPC (Remote Procedure Call) responses. All RPC endpoints must comply with these standards to ensure consistency, compatibility, and maintainability across the Dinero ecosystem.

## Core Requirements

### 1. Custom Header Requirement

**Rule:** All RPC responses MUST include the `X-Dinero-RPC-Engine: v2` header.

**Rationale:** This header identifies the RPC engine version, enabling clients to adapt to API changes and maintain backward compatibility.

**Implementation:**
```http
HTTP/1.1 200 OK
Content-Type: application/json
X-Dinero-RPC-Engine: v2
Connection: close
Content-Length: 123

{"jsonrpc":"2.0","id":1,"result":...}
```

**Validation:**
```bash
curl -sD - -o /dev/null \
  --user "$(cat /tmp/test-dir2/mainnet/.cookie)" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
  http://127.0.0.1:20998 \
| tr -d '\r' | grep -i '^x-dinero-rpc-engine: v2' && echo "✅ header OK" || echo "❌ header missing"
```

### 2. nextblockhash Field Omission

**Rule:** The `nextblockhash` field MUST be omitted entirely when not available, not set to an empty string or null.

**Rationale:** This follows Bitcoin Core's pattern and ensures clean JSON responses without meaningless fields.

**Correct Implementation:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "hash": "00006798be6323904ac5a06c903b4a11dd8e5575634d6c32584d2cc3e3fe360b",
    "height": 1,
    "version": 1,
    "merkleroot": "...",
    "time": 1640995200,
    "bits": "1e0ffff0",
    "nonce": 12345,
    "tx": ["..."]
    // nextblockhash field is omitted entirely
  }
}
```

**Incorrect Implementation:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "hash": "00006798be6323904ac5a06c903b4a11dd8e5575634d6c32584d2cc3e3fe360b",
    "height": 1,
    "nextblockhash": "",  // ❌ Empty string
    "nextblockhash": null // ❌ Null value
  }
}
```

**Validation:**
```bash
H=$(curl -s --user "$(cat /tmp/test-dir2/mainnet/.cookie)" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' http://127.0.0.1:20998 | jq -r '.result')
BH=$(curl -s --user "$(cat /tmp/test-dir2/mainnet/.cookie)" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockhash","params":['"$H""]}' http://127.0.0.1:20998 | jq -r '.result')
curl -s --user "$(cat /tmp/test-dir2/mainnet/.cookie)" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblock","params":["'"$BH"'",true]}' http://127.0.0.1:20998 \
| jq -e '.result | has("nextblockhash") | not' >/dev/null && echo "✅ nextblockhash omitted" || echo "❌ nextblockhash present"
```

### 3. Clean JSON-RPC Response

**Rule:** Successful responses MUST NOT include an `error` field. The `error` field should only be present when there's an actual error.

**Rationale:** This ensures clean, predictable JSON responses and follows JSON-RPC 2.0 best practices.

**Correct Implementation (Success):**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": 12345
  // No error field present
}
```

**Correct Implementation (Error):**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32601,
    "message": "Method not found"
  }
}
```

**Incorrect Implementation:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": 12345,
  "error": null  // ❌ Error field with null value
}
```

**Validation:**
```bash
curl -s --user "$(cat /tmp/test-dir2/mainnet/.cookie)" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' http://127.0.0.1:20998 \
| jq -e 'has("error") | not' >/dev/null && echo "✅ no error field" || echo "❌ error field present"
```

## JSON-RPC 2.0 Compliance

All RPC responses must comply with JSON-RPC 2.0 specification:

- **Required Fields:** `jsonrpc`, `id`
- **Success Response:** Must include `result` field
- **Error Response:** Must include `error` field with `code` and `message`
- **Version:** Always `"2.0"`

## Response Headers

### Required Headers
- `Content-Type: application/json`
- `X-Dinero-RPC-Engine: v2`

### Optional Headers
- `Access-Control-Allow-Origin: *` (when CORS enabled)
- `Access-Control-Allow-Methods: GET, POST, OPTIONS`
- `Access-Control-Allow-Headers: Content-Type`

## Testing and Validation

### Automated Test Suite
Use the built-in test suite to validate compliance:

```bash
# Run all validation tests
./dinero-cli.sh test-suite

# Check response headers
./dinero-cli.sh headers
```

### Manual Validation
Individual tests are available for debugging specific requirements:

```bash
# Test custom header
curl -sD - -o /dev/null --user "$(cat /tmp/test-dir2/mainnet/.cookie)" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
  http://127.0.0.1:20998 | tr -d '\r' | grep -i '^x-dinero-rpc-engine: v2'

# Test nextblockhash omission
# (See validation examples above)

# Test clean JSON-RPC
# (See validation examples above)
```

## Versioning

### RPC Engine Version
- **Current:** v2
- **Format:** `X-Dinero-RPC-Engine: v2`
- **Changes:** Increment version when breaking changes are introduced

### Standards Version
- **Current:** 1.0.0
- **Format:** Semantic versioning (MAJOR.MINOR.PATCH)
- **Breaking Changes:** Increment MAJOR version

## Compliance Levels

### Required
- All three core requirements must be implemented
- Non-compliance will cause automated tests to fail
- Required for production deployment

### Recommended
- Follow Bitcoin Core response patterns
- Implement consistent error codes
- Use meaningful error messages

### Optional
- Additional response headers
- Extended error information
- Performance optimizations

## Implementation Notes

### Current Implementation Status
- ✅ **Custom Header**: Implemented in all RPC servers
- ✅ **nextblockhash Omission**: Properly handled in getblock responses
- ✅ **Clean JSON-RPC**: Error field only present when needed

### Files Modified
- `rpc/rpc_http.hpp` - Line 120
- `src/daemon/rpc_server.cpp` - Line 847
- `daemon/rpc_server.cpp` - Line 584
- `src/common/json_utils.cpp` - Line 127

### Testing
- Test suite validates all requirements automatically
- Manual validation commands available for debugging
- CI/CD integration ensures ongoing compliance

## Future Considerations

### Planned Enhancements
- Batch RPC support
- WebSocket RPC endpoints
- Rate limiting and authentication
- Extended error codes and messages

### Backward Compatibility
- New versions will maintain backward compatibility
- Breaking changes will be clearly documented
- Migration guides will be provided for major changes

## References

- [JSON-RPC 2.0 Specification](https://www.jsonrpc.org/specification)
- [Bitcoin Core RPC Documentation](https://developer.bitcoin.org/reference/rpc/)
- [Dinero RPC Test Suite](../dinero-cli.sh#test-suite)

---

**Document Maintainer:** Dinero Development Team  
**Last Updated:** August 20, 2025  
**Next Review:** September 20, 2025

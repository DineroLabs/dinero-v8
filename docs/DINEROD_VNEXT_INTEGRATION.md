# DineroCoin CLI + dinerod vNext Integration Plan

## Overview

The DineroCoin CLI v1.0.0 provides the perfect foundation for integrating with the upcoming dinerod vNext infrastructure improvements. This document outlines the integration strategy for RPC stabilization, health endpoints, and enhanced observability.

## Integration Points

### 1. RPC Schema Evolution

**Current State:**
- CLI uses `din.cli.v1` envelope for all JSON output
- Daemon responses are unversioned and inconsistent

**Target State:**
- Daemon adopts `din.rpc.v1` schema for all RPC responses
- CLI validates and displays schema compatibility
- Dual-schema support during transition

**Implementation:**
```json
// CLI envelope (unchanged)
{
  "schema": "din.cli.v1",
  "command": "getnetworkinfo",
  "ok": true,
  "data": {
    "rpc_schema": "din.rpc.v1",    // New: daemon schema
    "version": "1.0.0",
    "connections": 8,
    "warnings": ""
  }
}
```

### 2. Health Endpoint Integration

**Current State:**
- CLI `--wait-ready` uses RPC calls to check daemon readiness
- No standardized health check endpoint

**Target State:**
- `/healthz` endpoint provides fast health checks
- CLI uses health endpoint for readiness validation
- Structured health data for monitoring

**Implementation:**
```bash
# Enhanced readiness check
dinero-cli --profile prod --wait-ready --timeout 60
# → Uses GET /healthz instead of RPC calls for faster response

# Health data in nodeinfo
dinero-cli --nodeinfo --format json
# → Includes health status and metrics
```

### 3. Metrics Integration

**Current State:**
- No standardized metrics endpoint
- Limited observability for production deployments

**Target State:**
- `/metrics` endpoint in Prometheus format
- CLI can display key metrics in `--nodeinfo`
- Full observability stack ready

**Implementation:**
```bash
# Metrics in CLI output
dinero-cli --profile prod --nodeinfo --verbose
# → Shows key metrics from /metrics endpoint

# Direct metrics access
curl http://node:20998/metrics | grep dinerod_chain_tip_height
```

## CLI Enhancements Required

### 1. RPC Schema Validation

Add validation for daemon RPC schema compatibility:

```cpp
// In RPC response handling
bool validateRpcSchema(const Json::Value& response) {
    auto schema = response.get("rpc_schema", "").asString();
    if (schema.empty()) {
        std::cerr << "Warning: Daemon missing rpc_schema tag" << std::endl;
        return false;
    }
    if (schema != "din.rpc.v1") {
        std::cerr << "Warning: Unsupported RPC schema: " << schema << std::endl;
        return false;
    }
    return true;
}
```

### 2. Health Endpoint Support

Enhance `--wait-ready` to use `/healthz`:

```cpp
// In retry.cpp waitForReady function
bool checkHealthEndpoint(const std::string& baseUrl) {
    try {
        auto healthUrl = baseUrl + "/../healthz";
        auto response = httpGet(healthUrl);
        auto json = Json::parse(response);
        return json.get("ok", false).asBool();
    } catch (...) {
        // Fallback to RPC method
        return checkRpcReadiness(baseUrl);
    }
}
```

### 3. Enhanced Nodeinfo

Include health and metrics data:

```cpp
// In nodeinfo output
void printEnhancedNodeInfo(const NodeInfo& ni, const Options& opt) {
    // Existing nodeinfo...
    
    // Add health status
    if (auto health = getHealthStatus(ni.rpc_url)) {
        std::cout << "Health Status:\n";
        std::cout << "  Tip Height: " << health->tipHeight << "\n";
        std::cout << "  Peers: " << health->peers << "\n";
        std::cout << "  Mempool: " << health->mempoolSize << " tx\n";
    }
    
    // Add key metrics if verbose
    if (opt.verbose) {
        if (auto metrics = getKeyMetrics(ni.rpc_url)) {
            std::cout << "Metrics:\n";
            std::cout << "  Chain Tip: " << metrics->chainTip << "\n";
            std::cout << "  Connected Peers: " << metrics->peers << "\n";
        }
    }
}
```

## Migration Strategy

### Phase 1: Schema Preparation (Week 1)
- Add RPC schema validation to CLI
- Implement graceful fallback for unversioned responses
- Add warning messages for schema mismatches

### Phase 2: Health Integration (Week 2)
- Implement `/healthz` endpoint support in CLI
- Update `--wait-ready` to use health endpoint
- Add health status to `--nodeinfo` output

### Phase 3: Metrics Integration (Week 3)
- Add `/metrics` endpoint support
- Include key metrics in verbose nodeinfo
- Document metrics for monitoring setups

### Phase 4: Full Integration (Week 4)
- Complete RPC method coverage (help, getnetworkinfo, etc.)
- Validate all CLI commands against new schema
- Update documentation and examples

## Backward Compatibility

### During Transition
- CLI supports both versioned and unversioned daemon responses
- Warning messages for deprecated patterns
- Graceful degradation when new endpoints unavailable

### Long-term Support
- CLI v1.x maintains compatibility with `din.rpc.v1`
- Breaking changes require CLI v2.0 + `din.rpc.v2`
- Clear migration path documented

## Testing Strategy

### Integration Tests
```bash
# Test schema validation
dinero-cli --format json getnetworkinfo | jq '.data.rpc_schema'
# Should return: "din.rpc.v1"

# Test health endpoint
dinero-cli --wait-ready --timeout 5
# Should use /healthz for faster response

# Test metrics integration
dinero-cli --nodeinfo --verbose | grep "Chain Tip"
# Should show metrics data
```

### Compatibility Tests
```bash
# Test with old daemon (no schema)
dinero-cli --format json status
# Should work with warning message

# Test with new daemon
dinero-cli --format json help
# Should show all new RPC methods
```

## Production Benefits

### For Operators
- **Faster health checks**: `/healthz` endpoint reduces readiness check time
- **Better monitoring**: Prometheus metrics for comprehensive observability
- **Stable contracts**: Versioned schemas prevent breaking changes

### For Developers
- **Clear interfaces**: RPC registry provides method discovery
- **Consistent errors**: JSON-RPC 2.0 error codes
- **Better debugging**: Structured logging with trace IDs

### For Exchanges
- **Reliable automation**: Stable JSON contracts for trading systems
- **Performance monitoring**: Metrics for capacity planning
- **Health validation**: Fast readiness checks for load balancers

## Implementation Timeline

| Week | Focus | Deliverables |
|------|-------|--------------|
| 1 | RPC Schema | Schema validation, fallback handling |
| 2 | Health Endpoints | `/healthz` integration, enhanced `--wait-ready` |
| 3 | Metrics | `/metrics` support, verbose nodeinfo |
| 4 | Integration | Full RPC coverage, documentation |

## Success Criteria

- [ ] CLI validates `din.rpc.v1` schema in all responses
- [ ] `--wait-ready` uses `/healthz` endpoint when available
- [ ] `--nodeinfo --verbose` shows key metrics
- [ ] All new RPC methods (help, getnetworkinfo, etc.) work with CLI
- [ ] Backward compatibility maintained with warnings
- [ ] Integration tests pass for both old and new daemons

This integration plan ensures the CLI v1.0.0 evolves seamlessly with dinerod vNext while maintaining production stability and backward compatibility.

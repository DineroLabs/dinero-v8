# DineroCoin CLI vNext Integration Usage Guide

## Overview

The DineroCoin CLI v1.0.0 now includes seamless integration with dinerod vNext features, providing enhanced observability, faster health checks, and schema validation for production deployments.

## New Features

### 1. RPC Schema Validation

The CLI automatically validates daemon responses for `din.rpc.v1` schema compatibility:

```bash
# Schema validation happens automatically
dinero-cli getnetworkinfo
# Warning: Daemon response missing 'rpc_schema' field.
# Consider upgrading to dinerod vNext for better compatibility.

# With vNext daemon (no warnings)
dinero-cli getnetworkinfo
# Returns data with validated schema
```

### 2. Health Endpoint Integration

Enhanced `--wait-ready` using `/healthz` endpoint for faster readiness checks:

```bash
# Fast health-based readiness (2-3x faster than RPC)
dinero-cli --wait-ready --timeout 30
# Uses /healthz endpoint if available, falls back to RPC

# Health data in nodeinfo
dinero-cli --nodeinfo
# Shows health status from /healthz endpoint

dinero-cli --nodeinfo --format json
# Includes structured health data in JSON output
```

### 3. Metrics Integration

Access Prometheus metrics via `--nodeinfo --verbose`:

```bash
# Show key metrics in text format
dinero-cli --nodeinfo --verbose
# Displays chain tip, peers, mempool, uptime, RPC calls

# Structured metrics in JSON
dinero-cli --nodeinfo --verbose --format json
# Includes full metrics object for monitoring
```

## Usage Examples

### Basic Health Monitoring

```bash
# Quick health check
dinero-cli --nodeinfo
# Output includes:
#   Health Status:
#     Status:         OK
#     Tip Height:     12345
#     Peers:          8
#     Mempool Size:   42 tx

# JSON format for automation
dinero-cli --nodeinfo --format json | jq '.data.health'
{
  "ok": true,
  "tip_height": 12345,
  "peers": 8,
  "mempool_size": 42,
  "status": "synced"
}
```

### Production Readiness

```bash
# Wait for daemon with health endpoint (fast)
dinero-cli --profile prod --wait-ready --timeout 60
# Uses /healthz for sub-second response times

# Comprehensive status with metrics
dinero-cli --profile prod --nodeinfo --verbose --format json
# Full observability data for monitoring systems
```

### Schema Compatibility

```bash
# Check daemon compatibility
dinero-cli help
# Lists all available RPC methods with schema validation

# Verify specific method schema
dinero-cli getnetworkinfo --format json | jq '.data.rpc_schema'
# Should return: "din.rpc.v1"
```

## Integration with Monitoring

### Prometheus Metrics

The CLI can extract key metrics from the daemon's `/metrics` endpoint:

```bash
# Get metrics for Grafana dashboards
dinero-cli --nodeinfo --verbose --format json | jq '.data.metrics'
{
  "chain_tip": 12345,
  "peers": 8,
  "mempool_tx_count": 42,
  "uptime_seconds": 86400.5,
  "rpc_calls_total": 1337
}
```

### Health Checks for Load Balancers

```bash
# Fast health check for HAProxy/nginx
dinero-cli --wait-ready --timeout 5
echo $?  # 0 = healthy, non-zero = unhealthy

# JSON health data for custom health checks
dinero-cli --nodeinfo --format json | jq -r '.data.health.ok'
# Returns: true/false
```

### Alerting Integration

```bash
#!/bin/bash
# Example monitoring script

HEALTH=$(dinero-cli --nodeinfo --format json | jq '.data.health')
OK=$(echo $HEALTH | jq -r '.ok')
PEERS=$(echo $HEALTH | jq -r '.peers')

if [ "$OK" != "true" ] || [ "$PEERS" -lt 3 ]; then
    echo "ALERT: Node unhealthy - OK:$OK Peers:$PEERS"
    exit 1
fi

echo "Node healthy - Peers:$PEERS"
```

## Backward Compatibility

### Graceful Degradation

The CLI maintains full compatibility with older daemons:

```bash
# Works with both old and new daemons
dinero-cli getbestblockhash
# New daemon: validates schema, shows no warnings
# Old daemon: works normally, shows upgrade suggestion

# Health features gracefully degrade
dinero-cli --wait-ready
# New daemon: uses fast /healthz endpoint
# Old daemon: falls back to RPC calls
```

### Migration Path

1. **Current Setup**: CLI v1.0.0 + old daemon
   - Full functionality with warnings about missing schema
   - `--wait-ready` uses RPC calls (slower but works)
   - `--nodeinfo` shows basic connection info

2. **Partial Upgrade**: CLI v1.0.0 + dinerod vNext
   - Schema validation passes silently
   - Fast health checks via `/healthz`
   - Enhanced `--nodeinfo` with health and metrics

3. **Full Integration**: CLI v1.0.0 + dinerod vNext + monitoring
   - Production-grade observability
   - Prometheus metrics integration
   - Sub-second health checks for load balancers

## Configuration

### Profile Integration

Health and metrics work seamlessly with CLI profiles:

```json
// ~/.dinero/profiles.json
{
  "prod": {
    "rpc_url": "http://prod-node:20998",
    "cookie_path": "/opt/dinero/prod/.cookie"
  },
  "staging": {
    "rpc_url": "http://staging-node:20998",
    "cookie_path": "/opt/dinero/staging/.cookie"
  }
}
```

```bash
# Health check specific environment
dinero-cli --profile prod --nodeinfo --verbose
dinero-cli --profile staging --wait-ready --timeout 30
```

### Environment Variables

```bash
# Override health endpoint timeout
export DINERO_HEALTH_TIMEOUT=10

# Disable health endpoint (force RPC fallback)
export DINERO_DISABLE_HEALTH_ENDPOINT=1

# Enable verbose health logging
export DINERO_HEALTH_VERBOSE=1
```

## Troubleshooting

### Schema Warnings

```bash
# Warning: RPC schema mismatch. Found: 'din.rpc.v0.9', Expected: 'din.rpc.v1'
# Solution: Upgrade daemon to vNext

# Warning: Daemon response missing 'rpc_schema' field.
# Solution: Upgrade daemon or ignore warnings in automation
```

### Health Endpoint Issues

```bash
# Health endpoint unavailable (falls back to RPC)
dinero-cli --wait-ready --verbose
# Shows: "Health endpoint unavailable, using RPC fallback"

# Health endpoint returns error
dinero-cli --nodeinfo
# Shows health error in output for debugging
```

### Performance Optimization

```bash
# Fastest health check (2-second timeout)
dinero-cli --wait-ready --timeout 2

# Skip metrics collection (faster nodeinfo)
dinero-cli --nodeinfo  # without --verbose

# Cache health data for multiple checks
HEALTH=$(dinero-cli --nodeinfo --format json)
echo $HEALTH | jq '.data.health.ok'
echo $HEALTH | jq '.data.health.peers'
```

## API Reference

### Health Data Structure

```json
{
  "health": {
    "ok": true,
    "tip_height": 12345,
    "peers": 8,
    "mempool_size": 42,
    "status": "synced",
    "error": null  // only present if ok=false
  }
}
```

### Metrics Data Structure

```json
{
  "metrics": {
    "chain_tip": 12345,
    "peers": 8,
    "mempool_tx_count": 42,
    "uptime_seconds": 86400.5,
    "rpc_calls_total": 1337
  }
}
```

### Schema Validation

All RPC responses from vNext daemons include:

```json
{
  "rpc_schema": "din.rpc.v1",
  "method": "getnetworkinfo",
  "result": {
    // method-specific data
  }
}
```

## Best Practices

### Production Deployments

1. **Use profiles** for environment separation
2. **Enable health endpoints** for fast load balancer checks
3. **Monitor metrics** via `--nodeinfo --verbose --format json`
4. **Set appropriate timeouts** for `--wait-ready` (5-30 seconds)
5. **Validate schema compatibility** during daemon upgrades

### Automation Scripts

1. **Parse JSON output** for reliable data extraction
2. **Check exit codes** for success/failure detection
3. **Use health endpoints** for frequent monitoring
4. **Cache nodeinfo data** for multiple metric extractions
5. **Handle graceful degradation** for mixed daemon versions

### Monitoring Integration

1. **Prometheus**: Extract metrics via `jq` from `--nodeinfo --verbose`
2. **Grafana**: Use JSON output for dashboard data sources
3. **Alerting**: Monitor health.ok and key metrics thresholds
4. **Load Balancers**: Use `--wait-ready` for health checks
5. **CI/CD**: Validate daemon readiness in deployment pipelines

This integration provides a smooth evolution path from CLI v1.0.0 to full dinerod vNext observability while maintaining backward compatibility and production reliability.

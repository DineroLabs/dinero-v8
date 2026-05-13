# Dinero RPC Rate Limiting RFC

**Status:** Draft  
**Version:** 1.0  
**Last Updated:** August 22, 2025  
**Next Review:** September 22, 2025

## Overview

This RFC defines rate limiting for Dinero RPC endpoints, protecting against accidental overload and DoS attacks while maintaining service stability. The implementation covers both HTTP RPC and WebSocket connections with configurable limits and circuit breaker patterns.

## Goals

- **Service Protection**: Prevent accidental overload and DoS attacks
- **Latency Stability**: Maintain consistent response times under load
- **Resource Management**: Protect CPU, memory, and network resources
- **Flexible Configuration**: Support different limits for different client types
- **Monitoring**: Provide visibility into rate limiting behavior

## Architecture

### Rate Limiting Strategy
- **Token Bucket Algorithm**: Burst-friendly with configurable refill rates
- **Per-Client Limits**: Different limits based on authentication and client type
- **Circuit Breaker**: Global limits when system is stressed
- **Graceful Degradation**: Return 429 responses instead of dropping requests

### Client Classification
1. **Unauthenticated**: Very low limits (or disabled for local-only daemons)
2. **Cookie Authenticated**: Standard limits for local clients
3. **Token Authenticated**: Higher limits for trusted remote clients
4. **Admin**: Highest limits for privileged operations

## Implementation

### Token Bucket Algorithm

```cpp
class TokenBucket {
    double tokens;           // Current tokens available
    double max_tokens;       // Maximum burst capacity
    double refill_rate;      // Tokens per second
    std::chrono::steady_clock::time_point last_refill;
    
public:
    bool consume(double tokens_needed);
    void refill();
};
```

### Rate Limit Configuration

```cpp
struct RateLimitConfig {
    // HTTP RPC limits
    int http_requests_per_second = 50;
    int http_burst_limit = 100;
    
    // WebSocket limits
    int ws_subscriptions_per_second = 5;
    int ws_max_subscriptions = 8;
    int ws_max_connections = 100;
    
    // Global circuit breaker
    int max_cpu_percent = 80;
    int max_event_backlog = 1000;
    double circuit_breaker_multiplier = 0.5;
};
```

## Rate Limiting Rules

### HTTP RPC Endpoints

#### Standard Methods
- **getblockcount, getblockchaininfo**: 100 req/sec, burst 200
- **getblock, getblockhash**: 50 req/sec, burst 100
- **getmininginfo, getnetworkstats**: 30 req/sec, burst 60
- **setgenerate, startmining**: 10 req/sec, burst 20
- **wallet operations**: 20 req/sec, burst 40

#### Batch RPC
- **batch method**: 20 req/sec, burst 40
- **Per-batch limit**: Maximum 100 individual requests per batch

### WebSocket Connections

#### Connection Limits
- **Max concurrent connections**: 100
- **Max subscriptions per connection**: 8
- **Connection timeout**: 5 minutes idle

#### Subscription Limits
- **newHeads**: 5 updates/sec per subscription
- **newBlocks**: 2 updates/sec per subscription
- **mempoolTx**: 10 updates/sec per subscription
- **miningInfo**: 1 update/sec per subscription

### Authentication-Based Limits

#### Unauthenticated (Local Only)
- **HTTP RPC**: 10 req/sec, burst 20
- **WebSocket**: Disabled
- **Note**: Only enabled if `-rpcallowip=127.0.0.1` is set

#### Cookie Authenticated
- **HTTP RPC**: 50 req/sec, burst 100
- **WebSocket**: Standard limits
- **Scope**: Local development and administration

#### Token Authenticated (Future)
- **HTTP RPC**: 100 req/sec, burst 200
- **WebSocket**: Higher limits
- **Scope**: Remote clients, exchanges, pools

#### Admin Privileges
- **HTTP RPC**: 200 req/sec, burst 400
- **WebSocket**: Highest limits
- **Scope**: System administration, debugging

## Circuit Breaker Pattern

### Global Backpressure Detection

```cpp
class CircuitBreaker {
    std::atomic<bool> is_open{false};
    std::atomic<int> failure_count{0};
    std::chrono::steady_clock::time_point last_failure;
    
    // Thresholds
    int failure_threshold = 10;
    int recovery_timeout_seconds = 60;
    
public:
    bool should_allow_request();
    void record_failure();
    void record_success();
};
```

### Trigger Conditions

#### CPU Usage
- **Threshold**: >80% CPU usage for 10 seconds
- **Action**: Reduce all rate limits by 50%
- **Recovery**: When CPU drops below 60% for 30 seconds

#### Event Loop Backlog
- **Threshold**: >1000 pending events
- **Action**: Reduce WebSocket push rates by 75%
- **Recovery**: When backlog drops below 100

#### Memory Pressure
- **Threshold**: >90% memory usage
- **Action**: Reject new WebSocket connections
- **Recovery**: When memory drops below 70%

## Response Handling

### Rate Limit Exceeded (HTTP)

```http
HTTP/1.1 429 Too Many Requests
Content-Type: application/json
Retry-After: 1
X-Dinero-RPC-Engine: v2

{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -429,
    "message": "Rate limit exceeded",
    "data": {
      "retry_after": 1,
      "limit": 50,
      "current": 52,
      "reset_time": 1735689721
    }
  }
}
```

### Rate Limit Exceeded (WebSocket)

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -429,
    "message": "Rate limit exceeded",
    "data": {
      "retry_after": 1,
      "limit": 5,
      "current": 6,
      "subscription": "sub-1"
    }
  }
}
```

### Circuit Breaker Open

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32002,
    "message": "Service temporarily unavailable",
    "data": {
      "reason": "circuit_breaker_open",
      "estimated_recovery": 30
    }
  }
}
```

## Configuration

### Command Line Options

```bash
# Enable rate limiting
-rpcratelimit

# HTTP RPC limits
-rpcrate=50                    # requests per second
-rpcratelimit-burst=100        # burst limit

# WebSocket limits
-rpcws-rate=5                  # subscriptions per second
-rpcws-max-subs=8              # max subscriptions per connection
-rpcws-max-connections=100     # max concurrent connections

# Circuit breaker thresholds
-rpcratelimit-cpu-threshold=80     # CPU percentage
-rpcratelimit-backlog-threshold=1000  # event backlog
-rpcratelimit-recovery-timeout=60  # recovery timeout seconds
```

### Configuration File

```json
{
  "rpcratelimit": {
    "enabled": true,
    "http": {
      "requests_per_second": 50,
      "burst_limit": 100
    },
    "websocket": {
      "subscriptions_per_second": 5,
      "max_subscriptions": 8,
      "max_connections": 100
    },
    "circuit_breaker": {
      "cpu_threshold": 80,
      "backlog_threshold": 1000,
      "recovery_timeout": 60
    }
  }
}
```

## Monitoring and Metrics

### Exposed Metrics

#### Rate Limiting Counters
- `rpc_rate_limit_allowed_total`: Total allowed requests
- `rpc_rate_limit_rejected_total`: Total rejected requests
- `rpc_rate_limit_burst_total`: Total burst requests
- `rpc_rate_limit_circuit_breaker_opens`: Circuit breaker activations

#### Current State
- `rpc_rate_limit_current_tokens`: Current tokens available per client
- `rpc_rate_limit_connections_active`: Active WebSocket connections
- `rpc_rate_limit_subscriptions_active`: Active subscriptions

### RPC Methods for Monitoring

#### getratelimitinfo
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "getratelimitinfo",
  "params": []
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "enabled": true,
    "global": {
      "circuit_breaker_open": false,
      "cpu_usage": 45.2,
      "event_backlog": 23
    },
    "limits": {
      "http_requests_per_second": 50,
      "ws_subscriptions_per_second": 5
    },
    "clients": {
      "127.0.0.1": {
        "tokens_remaining": 42,
        "last_request": 1735689720,
        "total_requests": 1250
      }
    }
  }
}
```

## Testing Strategy

### Unit Tests
1. **Token Bucket**: Verify token consumption and refill
2. **Rate Limiter**: Test various rate limit scenarios
3. **Circuit Breaker**: Test failure detection and recovery

### Integration Tests
1. **HTTP RPC**: Exceed limits, verify 429 responses
2. **WebSocket**: Test subscription rate limits
3. **Circuit Breaker**: Simulate high load, verify activation

### Load Tests
1. **Concurrent Clients**: Test with 100+ simultaneous connections
2. **Rate Limit Recovery**: Verify limits reset after cooldown
3. **Circuit Breaker**: Test under sustained high load

## Security Considerations

### DoS Protection
- **IP-based Limits**: Prevent single IP from overwhelming service
- **Connection Limits**: Prevent connection exhaustion attacks
- **Resource Monitoring**: Detect and respond to resource pressure

### Authentication Bypass
- **Rate Limit Enforcement**: Ensure limits apply to all clients
- **IP Spoofing**: Use connection source IP, not headers
- **Token Validation**: Verify authentication before applying limits

### Resource Exhaustion
- **Memory Limits**: Prevent excessive memory usage
- **CPU Monitoring**: Detect and respond to high CPU usage
- **Connection Cleanup**: Automatically close idle connections

## Performance Impact

### Overhead
- **Token Bucket**: <1μs per request
- **Circuit Breaker**: <1μs per check
- **Monitoring**: <5μs per metric update

### Memory Usage
- **Per-Client State**: ~100 bytes per client
- **Connection Registry**: ~200 bytes per connection
- **Total Overhead**: <1MB for 1000 clients

### CPU Impact
- **Normal Operation**: <0.1% CPU overhead
- **High Load**: <1% CPU overhead
- **Circuit Breaker**: <0.5% CPU overhead

## Future Enhancements

### Phase 2: Advanced Features
- **Per-Method Limits**: Different limits for different RPC methods
- **Dynamic Adjustment**: Auto-tune limits based on system performance
- **Client Quotas**: Per-client rate limit quotas

### Phase 3: Distributed Rate Limiting
- **Redis Backend**: Shared rate limiting across multiple daemons
- **Consistent Hashing**: Distribute limits across cluster
- **Global Circuit Breaker**: Cluster-wide health monitoring

### Phase 4: Machine Learning
- **Anomaly Detection**: Identify unusual traffic patterns
- **Predictive Scaling**: Anticipate load and adjust limits
- **Behavioral Analysis**: Learn normal client behavior patterns

## References

- [Token Bucket Algorithm](https://en.wikipedia.org/wiki/Token_bucket)
- [Circuit Breaker Pattern](https://martinfowler.com/bliki/CircuitBreaker.html)
- [Rate Limiting Best Practices](https://cloud.google.com/architecture/rate-limiting-strategies-techniques)
- [HTTP 429 Status Code](https://developer.mozilla.org/en-US/docs/Web/HTTP/Status/429)

---

**Contributors:** Dinero Development Team  
**Reviewers:** TBD  
**Implementation:** TBD

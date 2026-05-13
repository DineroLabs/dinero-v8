# Dinero RPC WebSocket Support RFC

**Status:** Draft  
**Version:** 1.0  
**Last Updated:** August 22, 2025  
**Next Review:** September 22, 2025

## Overview

This RFC defines WebSocket RPC support for Dinero, enabling real-time subscriptions to blockchain events while maintaining JSON-RPC 2.0 semantics. The implementation includes basic rate limiting per connection to ensure service stability.

## Goals

- **Real-time Push Notifications**: New blocks, mempool transactions, tip changes, mining status
- **JSON-RPC 2.0 Compatibility**: Maintain existing RPC semantics over WebSocket
- **Subscription Management**: Subscribe/unsubscribe to specific event channels
- **Rate Limiting**: Prevent DoS and maintain service stability
- **Authentication**: Support both cookie and future token-based auth

## Architecture

### Endpoint
- **Path**: `/rpc.ws` (same port as HTTP RPC)
- **Protocol**: WebSocket upgrade from HTTP
- **Authentication**: Cookie-based (same as HTTP RPC)

### Connection Flow
1. Client connects to `/rpc.ws` with valid authentication
2. Server upgrades HTTP connection to WebSocket
3. Client subscribes to desired channels
4. Server pushes notifications for subscribed events
5. Connection maintained with heartbeat (ping/pong)

## API Specification

### WebSocket Upgrade
```
GET /rpc.ws HTTP/1.1
Host: 127.0.0.1:20998
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <base64-encoded-key>
Sec-WebSocket-Version: 13
Authorization: Basic <cookie-auth>
```

**Response Headers:**
```
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: <calculated-key>
X-Dinero-RPC-Engine: v2
```

### Subscription Methods

#### Subscribe
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "subscribe",
  "params": ["newHeads"]
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "subscription": "sub-1",
    "status": "subscribed"
  }
}
```

#### Unsubscribe
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "unsubscribe",
  "params": ["sub-1"]
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "status": "unsubscribed"
  }
}
```

### Server Push Notifications

#### New Block Headers
```json
{
  "jsonrpc": "2.0",
  "method": "dinero_subscription",
  "params": {
    "subscription": "sub-1",
    "result": {
      "height": 2,
      "hash": "0000...",
      "time": 1735689720,
      "difficulty": 1.0
    }
  }
}
```

#### Mempool Transaction
```json
{
  "jsonrpc": "2.0",
  "method": "dinero_subscription",
  "params": {
    "subscription": "sub-2",
    "result": {
      "txid": "abcd...",
      "fee": 0.0001,
      "size": 250
    }
  }
}
```

#### Mining Status Update
```json
{
  "jsonrpc": "2.0",
  "method": "dinero_subscription",
  "params": {
    "subscription": "sub-3",
    "result": {
      "generating": true,
      "threads": 4,
      "hashrate": 1250.5
    }
  }
}
```

## Available Channels

### `newHeads`
- **Description**: New block headers when best tip changes
- **Payload**: Block header information (omits `nextblockhash` at tip)
- **Frequency**: On every new block

### `newBlocks`
- **Description**: Full block information
- **Payload**: Complete block data (optional body gated by flag)
- **Frequency**: On every new block

### `mempoolTx`
- **Description**: New transactions entering mempool
- **Payload**: Transaction ID and basic metadata
- **Frequency**: On every new transaction

### `miningInfo`
- **Description**: Mining status updates
- **Payload**: Current mining information
- **Frequency**: Every 1 second while mining, or on status change

## Rate Limiting

### Per-Connection Limits
- **HTTP RPC**: 50 requests/second, burst 100
- **WebSocket Subscriptions**: 5 pushes/second per subscription
- **Connection Limits**: Maximum 8 subscriptions per connection

### Circuit Breaker
- **Global Backpressure**: If event loop backlog grows, temporarily lower limits
- **CPU Threshold**: If CPU usage > 80%, reduce rate limits by 50%

### Rate Limit Response
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -429,
    "message": "Rate limit exceeded",
    "data": {
      "retry_after": 1,
      "limit": 50,
      "current": 52
    }
  }
}
```

## Connection Management

### Heartbeat
- **Ping Interval**: 30 seconds
- **Pong Timeout**: 5 seconds
- **Idle Timeout**: 2 minutes (4 missed pings)

### Backpressure Handling
- **Outbound Queue**: Bounded per connection (max 100 messages)
- **Overflow Strategy**: Drop oldest messages when queue full
- **Lag Detection**: Switch to "lagging" mode for slow clients

### Connection Limits
- **Max Connections**: 100 concurrent WebSocket connections
- **Max Subscriptions**: 8 per connection
- **Connection Timeout**: 5 minutes idle

## Error Handling

### WebSocket Errors
```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32000,
    "message": "WebSocket error: connection limit exceeded"
  }
}
```

### Subscription Errors
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32001,
    "message": "Invalid subscription channel: unknownChannel"
  }
}
```

## Security Considerations

### Authentication
- **Cookie Required**: Same authentication as HTTP RPC
- **Connection Validation**: Verify auth on WebSocket upgrade
- **Subscription Scoping**: Users can only subscribe to their authorized channels

### Rate Limiting
- **Per-IP Limits**: Prevent DoS attacks
- **Per-Auth Limits**: Higher limits for authenticated users
- **Global Circuit Breaker**: Protect system resources

### Input Validation
- **JSON Schema**: Validate all incoming messages
- **Subscription Limits**: Prevent excessive subscriptions
- **Message Size**: Limit individual message size to 1MB

## Implementation Notes

### Server Components
1. **WebSocket Upgrader**: Handle HTTP → WebSocket upgrade
2. **Connection Registry**: Track active WebSocket connections
3. **Subscription Manager**: Manage subscriptions per connection
4. **Event Dispatcher**: Route blockchain events to subscribers
5. **Rate Limiter**: Enforce per-connection limits

### Integration Points
- **Blockchain Events**: Hook into new block/transaction events
- **Mining Events**: Hook into mining status changes
- **RPC Server**: Extend existing RPC infrastructure

### Configuration
```bash
# Enable WebSocket support
-rpcws

# WebSocket endpoint path (default: /rpc.ws)
-rpcwspath=/ws

# Rate limiting
-rpcrate=50
-rpcratelimit-burst=100

# Connection limits
-rpcws-max-connections=100
-rpcws-max-subscriptions=8
```

## Testing Strategy

### Integration Tests
1. **Connection Tests**: WebSocket upgrade, authentication
2. **Subscription Tests**: Subscribe, receive events, unsubscribe
3. **Rate Limiting Tests**: Exceed limits, verify 429 responses
4. **Reconnection Tests**: Disconnect/reconnect, verify no replay

### Performance Tests
1. **Connection Scaling**: Test with 100+ concurrent connections
2. **Event Throughput**: Measure push notification latency
3. **Rate Limit Recovery**: Verify limits reset after cooldown

### Security Tests
1. **Authentication**: Verify unauthorized connections rejected
2. **Rate Limit Bypass**: Attempt to bypass rate limiting
3. **Input Validation**: Test malformed JSON and oversized messages

## Future Enhancements

### Phase 2: Advanced Rate Limiting
- Per-auth principal limits
- Scoped rate limits (read vs write)
- Dynamic rate adjustment based on system load

### Phase 3: Token Authentication
- HMAC/JWT token support
- Scoped permissions per token
- Remote client authentication

### Phase 4: Advanced Features
- Event replay for missed notifications
- Filtered subscriptions (e.g., specific addresses)
- Batch notifications for high-frequency events

## References

- [JSON-RPC 2.0 Specification](https://www.jsonrpc.org/specification)
- [WebSocket Protocol RFC 6455](https://tools.ietf.org/html/rfc6455)
- [Bitcoin Core RPC Documentation](https://developer.bitcoin.org/reference/rpc/)
- [Rate Limiting Best Practices](https://cloud.google.com/architecture/rate-limiting-strategies-techniques)

---

**Contributors:** Dinero Development Team  
**Reviewers:** TBD  
**Implementation:** TBD

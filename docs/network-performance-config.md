# Dinero Network Performance Configuration

## Overview

This document describes the performance configuration system for Dinero's network components, including header sync, address management, compact block relay, and peer scoring.

## Configuration Structure

### Header Sync Configuration

```cpp
struct HeaderSync {
    uint32_t max_headers_per_request = 2000;     // Max headers per batch
    uint32_t max_blocks_per_request = 16;       // Max blocks per batch
    std::chrono::seconds sync_timeout{30};      // Overall sync timeout
    std::chrono::seconds peer_selection_interval{5}; // Peer rotation interval
    uint32_t max_concurrent_requests = 3;       // Max parallel requests
    std::chrono::seconds request_timeout{10};  // Individual request timeout
    uint32_t max_retries = 3;                  // Max retry attempts
    std::chrono::seconds retry_delay{5};       // Delay between retries
};
```

### Address Manager Configuration

```cpp
struct AddressManager {
    size_t max_addresses = 10000;              // Total address capacity
    size_t max_good_addresses = 1000;         // Good address capacity
    std::chrono::seconds address_timeout{24 * 60 * 60}; // Address expiry
    std::chrono::seconds ban_duration{60 * 60}; // Default ban duration
    int32_t max_score = 100;                   // Maximum peer score
    int32_t min_score = -100;                 // Minimum peer score
    uint32_t max_attempts_per_address = 10;   // Max attempts per address
    std::chrono::seconds cleanup_interval{300}; // Cleanup interval
};
```

### Compact Block Relay Configuration

```cpp
struct CompactBlockRelay {
    size_t max_prefilled_txns = 10;           // Max pre-filled transactions
    size_t max_short_ids = 1000;              // Max short IDs per block
    std::chrono::seconds tx_pool_timeout{300}; // Transaction pool timeout
    uint32_t max_tx_pool_size = 10000;        // Max transaction pool size
    std::chrono::seconds reconstruction_timeout{30}; // Reconstruction timeout
    uint32_t max_reconstruction_attempts = 3;  // Max reconstruction attempts
};
```

### Peer Scoring Configuration

```cpp
struct PeerScoring {
    int32_t max_score = 1000;                 // Maximum peer score
    int32_t min_score = -1000;                 // Minimum peer score
    int32_t ban_threshold = -100;              // Ban threshold
    std::chrono::seconds default_ban_duration{3600}; // Default ban duration
    std::chrono::seconds cleanup_interval{300}; // Cleanup interval
    uint32_t max_events_per_peer = 1000;      // Max events per peer
    std::chrono::seconds event_timeout{24 * 60 * 60}; // Event expiry
};
```

## Performance Metrics

### Key Metrics to Monitor

1. **Sync Performance**
   - Headers per second
   - Blocks per second
   - Sync completion time
   - Peer utilization

2. **Bandwidth Usage**
   - Download rate
   - Upload rate
   - Compact block savings
   - Fallback frequency

3. **Peer Management**
   - Active connections
   - Peer scores
   - Ban frequency
   - Eviction rate

4. **Memory Usage**
   - Mempool size
   - Block cache size
   - Peer cache size
   - Address database size

## Performance Tuning

### For High-Performance Nodes

```cpp
// Increase batch sizes for faster sync
max_headers_per_request = 5000;
max_blocks_per_request = 32;
max_concurrent_requests = 5;

// Increase address capacity
max_addresses = 50000;
max_good_addresses = 5000;

// Optimize compact blocks
max_prefilled_txns = 20;
max_short_ids = 2000;
```

### For Resource-Constrained Nodes

```cpp
// Reduce batch sizes to save memory
max_headers_per_request = 1000;
max_blocks_per_request = 8;
max_concurrent_requests = 2;

// Reduce address capacity
max_addresses = 5000;
max_good_addresses = 500;

// Limit compact block usage
max_prefilled_txns = 5;
max_short_ids = 500;
```

### For Test Networks

```cpp
// Fast sync for testing
max_headers_per_request = 200;
max_blocks_per_request = 4;
sync_timeout = 10s;
request_timeout = 5s;

// Quick bans for testing
ban_threshold = -50;
default_ban_duration = 60s;
```

## Security Considerations

### DoS Protection

1. **Rate Limiting**
   - Limit requests per peer
   - Implement connection throttling
   - Use circuit breakers

2. **Resource Limits**
   - Cap memory usage
   - Limit concurrent operations
   - Implement timeouts

3. **Peer Scoring**
   - Penalize misbehavior
   - Ban persistent offenders
   - Implement decay mechanisms

### Attack Mitigation

1. **Header Spam**
   - Validate header chains
   - Limit header requests
   - Implement peer rotation

2. **Block Spam**
   - Validate block headers
   - Limit block requests
   - Implement fallback mechanisms

3. **Connection Flooding**
   - Limit connections per IP
   - Implement connection timeouts
   - Use peer scoring

## Monitoring and Alerting

### Key Alerts

1. **Sync Issues**
   - Sync timeout exceeded
   - Peer rotation failures
   - Header validation errors

2. **Performance Degradation**
   - High memory usage
   - Low bandwidth utilization
   - Excessive fallbacks

3. **Security Events**
   - High ban frequency
   - Suspicious peer behavior
   - Resource exhaustion

### Metrics Collection

```cpp
struct PerformanceMetrics {
    uint64_t bytes_downloaded;
    uint64_t bytes_uploaded;
    uint32_t connections_active;
    uint32_t connections_total;
    uint32_t blocks_synced;
    uint32_t headers_synced;
    uint32_t transactions_relayed;
    uint32_t peers_banned;
    std::chrono::milliseconds avg_sync_time;
    std::chrono::milliseconds avg_connection_time;
};
```

## Configuration Files

### Default Configuration

The system uses sensible defaults for most configurations. These can be overridden via:

1. **Command Line Arguments**
   - `--max-peers=125`
   - `--sync-timeout=30`
   - `--ban-duration=3600`

2. **Configuration Files**
   - `dinero.conf` for persistent settings
   - Environment variables for runtime overrides

3. **RPC Commands**
   - `net.setconfig` for dynamic configuration
   - `net.getconfig` for current settings

## Best Practices

### Production Deployment

1. **Start Conservative**
   - Use default settings initially
   - Monitor performance metrics
   - Adjust based on observed behavior

2. **Monitor Resources**
   - Track memory usage
   - Monitor bandwidth consumption
   - Watch for performance degradation

3. **Security First**
   - Enable all security features
   - Monitor for attacks
   - Implement proper logging

### Development and Testing

1. **Use Test Configurations**
   - Faster timeouts for testing
   - Reduced resource limits
   - Enhanced logging

2. **Simulate Conditions**
   - Test with network delays
   - Simulate peer failures
   - Test attack scenarios

3. **Performance Testing**
   - Load testing
   - Stress testing
   - Benchmarking

## Troubleshooting

### Common Issues

1. **Sync Failures**
   - Check peer connectivity
   - Verify header validation
   - Review timeout settings

2. **Performance Issues**
   - Monitor resource usage
   - Check configuration settings
   - Review peer quality

3. **Security Issues**
   - Check ban lists
   - Review peer scores
   - Monitor for attacks

### Debug Commands

```bash
# Check sync status
dinero-cli sync.getstatus

# View peer information
dinero-cli net.getpeerinfo

# Check address manager
dinero-cli net.getaddrmanstats

# View ban list
dinero-cli net.getbanlist

# Get performance metrics
dinero-cli net.getperformancemetrics
```

## Future Enhancements

### Planned Features

1. **Dynamic Configuration**
   - Runtime configuration changes
   - A/B testing support
   - Performance profiling

2. **Advanced Metrics**
   - Detailed performance breakdowns
   - Historical trend analysis
   - Predictive analytics

3. **Machine Learning**
   - Adaptive peer selection
   - Anomaly detection
   - Performance optimization

### Research Areas

1. **Network Topology**
   - Optimal peer selection
   - Network partitioning
   - Load balancing

2. **Protocol Optimization**
   - Message compression
   - Batch processing
   - Parallel operations

3. **Security Enhancements**
   - Advanced DoS protection
   - Privacy improvements
   - Attack detection

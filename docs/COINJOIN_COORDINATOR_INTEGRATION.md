# CoinJoin Real Coordinator Integration Documentation

## Overview

This document describes the complete CoinJoin coordinator integration system for Dinero, including both generic JSON coordinators and JoinMarket bridge integration.

## Architecture

### Adapter Pattern

The system uses an adapter pattern to support multiple coordinator types:

```
CoinJoin Client → ICoinJoinAdapter → Coordinator
                     ↓
            ┌─────────────────┐
            │                 │
    CoinJoinAdapterGeneric  CoinJoinAdapterJM
            │                 │
    Generic JSON         JoinMarket Bridge
    Coordinator          Microservice
```

### Components

1. **ICoinJoinAdapter Interface**: Defines the contract for all coordinators
2. **CoinJoinAdapterGeneric**: Implements generic JSON coordinator protocol
3. **CoinJoinAdapterJM**: Implements JoinMarket bridge protocol
4. **CoinJoinFactory**: Creates appropriate adapter based on configuration
5. **JM Bridge Microservice**: Python FastAPI service that bridges to JoinMarket

## Configuration

### Daemon Configuration

Add to your `dinero.conf` or configuration file:

```ini
[coinjoin]
type = "generic"                    # "generic" | "jm"
base_url = "http://127.0.0.1:8080" # Coordinator URL
require_proxy = false               # Require SOCKS5 proxy
proxy_host = "127.0.0.1"          # SOCKS5 proxy host
proxy_port = 9050                  # SOCKS5 proxy port
timeout_seconds = 30               # Request timeout
max_retries = 3                    # Max retry attempts
```

### Coordinator Types

#### Generic Coordinator (`type = "generic"`)

A simple JSON-based coordinator that implements the standard CoinJoin protocol:

**Endpoints:**
- `POST /register` - Register new round
- `POST /inputs` - Submit inputs
- `GET /status` - Get round status
- `GET /psbt` - Get PSBT
- `POST /submit` - Submit signed PSBT
- `POST /cancel` - Cancel round

**Example Registration:**
```json
POST /register
{
  "amount": 100000,
  "feerate": 5,
  "min_peers": 3,
  "policy": "anon"
}

Response:
{
  "round_id": "round_12345"
}
```

#### JoinMarket Bridge (`type = "jm"`)

A bridge service that translates Dinero's protocol to JoinMarket's taker workflow:

**Features:**
- SQLite persistence for rounds and inputs
- JoinMarket taker integration
- Tor support via reverse proxy
- Health monitoring
- Round management

**Setup:**
1. Install JoinMarket
2. Run JM bridge: `python3 tools/jm_bridge.py`
3. Configure daemon: `type = "jm"`

## Usage

### RPC Methods

The existing CoinJoin RPC methods work with both coordinator types:

```bash
# Start a CoinJoin round
dinero-cli coinjoinjoin --coordinator "http://coordinator:8080" --amount 100000 --feerate 5 --min_peers 3

# Check round status
dinero-cli coinjoinstatus --round_id "round_12345"

# Cancel a round
dinero-cli coinjoincancel --round_id "round_12345"
```

### Programmatic Usage

```cpp
#include "privacy/coinjoin_factory.h"

// Create adapter
auto adapter = din::make_cj_adapter("generic", "http://127.0.0.1:8080");

// Register round
din::CJJoinParams params;
params.base_url = "http://127.0.0.1:8080";
params.amount = 100000;
params.feerate_sat_vb = 5;
params.min_peers = 3;
params.policy = "anon";

std::string round_id = adapter->register_round(params);

// Submit inputs
std::vector<din::CJInputLite> inputs;
inputs.push_back({"txid123", 0, 50000});
adapter->submit_inputs(round_id, inputs, "equal_spk_hex", "change_spk_hex", 5);

// Check status
auto status = adapter->status(round_id);
if (status.phase == "psbt") {
    std::string psbt = adapter->fetch_psbt(round_id);
    // Sign PSBT...
    adapter->submit_signed(round_id, signed_psbt);
}
```

## Deployment

### Generic Coordinator Deployment

1. **Simple Setup** (Development):
   ```bash
   # Start coordinator
   python3 -m http.server 8080
   
   # Configure daemon
   echo '[coinjoin]
   type = "generic"
   base_url = "http://127.0.0.1:8080"' >> dinero.conf
   ```

2. **Production Setup** (Recommended):
   ```bash
   # Use nginx + uvicorn
   pip install uvicorn fastapi
   uvicorn coordinator:app --host 0.0.0.0 --port 8080
   
   # Add authentication, rate limiting, persistence
   ```

### JoinMarket Bridge Deployment

1. **Install Dependencies**:
   ```bash
   pip install fastapi uvicorn sqlite3
   ```

2. **Configure JoinMarket**:
   ```bash
   # Set up JoinMarket taker
   jm-taker --config jm-taker.conf
   ```

3. **Start Bridge**:
   ```bash
   python3 tools/jm_bridge.py
   ```

4. **Configure Daemon**:
   ```ini
   [coinjoin]
   type = "jm"
   base_url = "http://127.0.0.1:8080"
   ```

### Tor Integration

For privacy-enhanced deployment:

1. **Install Tor**:
   ```bash
   brew install tor  # macOS
   sudo apt install tor  # Ubuntu
   ```

2. **Configure Tor**:
   ```bash
   # /etc/tor/torrc
   HiddenServiceDir /var/lib/tor/coinjoin/
   HiddenServicePort 80 127.0.0.1:8080
   ```

3. **Start Services**:
   ```bash
   # Start Tor
   sudo systemctl start tor
   
   # Start coordinator
   python3 tools/jm_bridge.py
   
   # Get onion address
   sudo cat /var/lib/tor/coinjoin/hostname
   ```

4. **Configure Daemon**:
   ```ini
   [coinjoin]
   type = "jm"
   base_url = "http://your-onion-address.onion"
   require_proxy = true
   proxy_host = "127.0.0.1"
   proxy_port = 9050
   ```

## Testing

### Unit Tests

```bash
# Build tests
cmake --build build --target test_coinjoin

# Run tests
./build/test_coinjoin
```

### Integration Tests

```bash
# Start coordinator
python3 tools/jm_bridge.py &

# Run integration tests
./tests/test_coinjoin_integration.sh
```

### Manual Testing

1. **Start Coordinator**:
   ```bash
   python3 tools/jm_bridge.py
   ```

2. **Test Registration**:
   ```bash
   curl -X POST http://127.0.0.1:8080/register \
     -H "Content-Type: application/json" \
     -d '{"amount": 100000, "feerate": 5, "min_peers": 3}'
   ```

3. **Test CoinJoin**:
   ```bash
   dinero-cli coinjoinjoin --coordinator "http://127.0.0.1:8080" --amount 100000 --feerate 5 --min_peers 3
   ```

## Security Considerations

### Coordinator Security

1. **Authentication**: Add API keys or certificates
2. **Rate Limiting**: Prevent abuse and DoS attacks
3. **Input Validation**: Validate all inputs and parameters
4. **Logging**: Log all operations (without sensitive data)
5. **Monitoring**: Monitor for suspicious activity

### Privacy Considerations

1. **Tor Support**: Use Tor for coordinator communication
2. **No Key Storage**: Coordinators never store private keys
3. **PSBT Security**: Use secure PSBT handling
4. **Network Privacy**: Use SOCKS5 proxies when available

### Operational Security

1. **Backup**: Regular backups of coordinator state
2. **Updates**: Keep coordinator software updated
3. **Monitoring**: Monitor coordinator health and performance
4. **Incident Response**: Plan for coordinator failures

## Troubleshooting

### Common Issues

1. **Connection Refused**:
   - Check if coordinator is running
   - Verify URL and port
   - Check firewall settings

2. **Authentication Failed**:
   - Verify API keys
   - Check certificate validity
   - Confirm authentication method

3. **Round Timeout**:
   - Check coordinator logs
   - Verify network connectivity
   - Increase timeout settings

4. **PSBT Issues**:
   - Verify PSBT format
   - Check signature validity
   - Confirm input/output matching

### Debug Commands

```bash
# Check coordinator health
curl http://coordinator:8080/health

# List active rounds
curl http://coordinator:8080/rounds

# Check daemon logs
tail -f dinero.log | grep coinjoin

# Test network connectivity
telnet coordinator 8080
```

### Log Analysis

```bash
# Filter CoinJoin logs
grep "coinjoin" dinero.log

# Check coordinator logs
tail -f coordinator.log

# Monitor network traffic
tcpdump -i any port 8080
```

## Performance Optimization

### Coordinator Optimization

1. **Database**: Use PostgreSQL instead of SQLite for production
2. **Caching**: Cache frequently accessed data
3. **Load Balancing**: Use multiple coordinator instances
4. **CDN**: Use CDN for static content

### Client Optimization

1. **Connection Pooling**: Reuse HTTP connections
2. **Async Operations**: Use async/await for non-blocking operations
3. **Batch Operations**: Batch multiple operations
4. **Caching**: Cache coordinator responses

## Future Enhancements

### Planned Features

1. **Multi-Coordinator Support**: Support multiple coordinators simultaneously
2. **Coordinator Discovery**: Automatic coordinator discovery
3. **Federation**: Coordinator federation for better privacy
4. **Mobile Support**: Mobile-optimized coordinator clients

### Research Areas

1. **Zero-Knowledge Proofs**: ZK proofs for coordinator operations
2. **Decentralized Coordinators**: Fully decentralized coordination
3. **Cross-Chain Support**: Multi-chain CoinJoin support
4. **Advanced Privacy**: Enhanced privacy features

## Conclusion

The CoinJoin coordinator integration provides a flexible, secure, and privacy-focused solution for Dinero's CoinJoin functionality. The adapter pattern allows for easy integration with different coordinator types, while the JoinMarket bridge provides access to a proven ecosystem.

The system is designed for both development and production use, with comprehensive testing, documentation, and security considerations. Future enhancements will continue to improve privacy, performance, and usability.

For questions or support, please refer to the Dinero documentation or contact the development team.

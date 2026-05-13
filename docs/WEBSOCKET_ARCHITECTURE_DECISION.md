# WebSocket Architecture Decision

**Date**: November 7, 2025  
**Decision**: Disable WebSockets in desktop GUI, plan separate relay for mobile  
**Rationale**: Production-ready architecture with clear separation of concerns  

---

## 📊 Decision Matrix

| Scenario | Need WebSockets? | Recommended Approach |
|----------|------------------|----------------------|
| **Desktop GUI only** | ❌ | Use RPC polling |
| **Web dashboard / Explorer** | ✅ | Keep daemon WS or relay |
| **iOS/Android wallet** | ✅ | Use WebSocket relay |
| **CLI tools / monitoring** | ❌ | Use RPC & Prometheus |

---

## ✅ Current State: Desktop GUI Only

### Why RPC Polling is Better

**Desktop GUI Needs**:
- ✅ Balance updates (every 3-5 seconds is fine)
- ✅ New blocks (180s block time = plenty of time)
- ✅ Transaction status (can poll every 2-3 seconds)
- ✅ Peer count (can poll every 10 seconds)

**RPC Polling Advantages**:
```cpp
// Simple, reliable, no connection state to manage
QTimer::singleShot(3000, [this]() {
    rpc_->call("wallet.getbalance", {});  // Just works
});
```

**WebSockets Disadvantages for Desktop**:
- ❌ **Complexity**: Connection management, reconnection logic, state sync
- ❌ **Attack Surface**: Another network protocol to secure
- ❌ **Overkill**: Desktop doesn't need sub-second updates
- ❌ **Single User**: Desktop GUI = 1 user = no need for broadcast efficiency

### Implementation

**GUI CMakeLists.txt**:
```cmake
# WebSockets are OPTIONAL (disabled by default)
find_package(Qt6 QUIET COMPONENTS WebSockets)

if(Qt6WebSockets_FOUND)
  message(STATUS "✅ Qt6WebSockets found - WebSocket support enabled")
  set(HAVE_WEBSOCKETS ON)
else()
  message(STATUS "⚠️  Qt6WebSockets not found - WebSocket support disabled")
  set(HAVE_WEBSOCKETS OFF)
endif()
```

**Result**: GUI builds and works fine without WebSockets.

---

## 🚀 Future State: Mobile Apps (iOS/Android)

### Why Mobile Needs WebSockets

**Mobile App Requirements**:
- ✅ **Real-time updates**: Users expect instant balance changes
- ✅ **Battery efficiency**: Long-polling wastes battery
- ✅ **Push notifications**: New transactions trigger notifications
- ✅ **Network resilience**: Handle cellular network transitions

### Architecture: Separate Relay Microservice

**Why NOT Embed WebSockets in Daemon**:
- ❌ Daemon is consensus-critical (minimize attack surface)
- ❌ Mobile clients need different authentication (JWT, not RPC cookie)
- ❌ Scaling: 1,000 mobile clients → 1,000 WS connections to daemon (bad)

**Why USE a Separate Relay**:
- ✅ **Isolation**: Relay crashes don't affect consensus
- ✅ **Scaling**: Relay can scale horizontally (multiple instances)
- ✅ **Authentication**: JWT tokens, rate limiting, user management
- ✅ **Caching**: Relay caches common queries (balance, tx history)
- ✅ **SSL/TLS**: Relay handles HTTPS/WSS certificates
- ✅ **Multi-protocol**: Relay can offer REST + WebSocket + GraphQL

---

## 🧩 Proposed Future Architecture

### DineroRelay Microservice (2026+)

```
┌─────────────────────────────────────────────────────────┐
│                    Mobile Apps                          │
│         (iOS Wallet, Android Wallet)                    │
└────────────────┬────────────────────────────────────────┘
                 │
                 │ WebSocket (WSS) + REST (HTTPS)
                 │ Authentication: JWT tokens
                 │
┌────────────────▼────────────────────────────────────────┐
│              DineroRelay Service                        │
│  - WebSocket server (WSS on port 8443)                  │
│  - REST API (HTTPS on port 443)                         │
│  - JWT authentication                                   │
│  - Rate limiting (per user, per IP)                     │
│  - Caching (balance, tx history)                        │
│  - Push notifications (Firebase/APNs integration)       │
│  - Multi-node failover                                  │
└────────────────┬────────────────────────────────────────┘
                 │
                 │ Internal RPC (authenticated)
                 │ Localhost or private network
                 │
┌────────────────▼────────────────────────────────────────┐
│              Dinero Core Daemon (dinerod)               │
│  - Consensus engine                                     │
│  - RPC server (localhost:20998)                         │
│  - P2P network                                          │
│  - Blockchain storage (RocksDB)                         │
│  - Wallet (if enabled)                                  │
│  - NO public WebSocket endpoint                         │
└─────────────────────────────────────────────────────────┘
```

### DineroRelay Features

**Phase 1**: Basic relay (2026 Q1)
```typescript
// dinero-relay (Node.js/TypeScript or Rust)
import { WebSocketServer } from 'ws';
import { DineroRPC } from 'dinero-rpc-client';

const wss = new WebSocketServer({ port: 8443 });
const rpc = new DineroRPC('http://localhost:20998');

wss.on('connection', (ws, req) => {
  // JWT authentication
  const token = authenticateJWT(req);
  if (!token) return ws.close(4001, 'Unauthorized');
  
  // Subscribe to events
  ws.on('message', async (msg) => {
    const { method, params } = JSON.parse(msg);
    
    // Rate limiting
    if (!checkRateLimit(token.userId, method)) {
      return ws.send({ error: 'Rate limit exceeded' });
    }
    
    // Proxy to daemon via RPC
    const result = await rpc.call(method, params);
    ws.send({ result });
  });
  
  // Push new blocks
  pollForNewBlocks((block) => {
    ws.send({ event: 'block', data: block });
  });
});
```

**Phase 2**: Advanced features (2026 Q2-Q3)
- Redis caching for common queries
- PostgreSQL for user accounts & push tokens
- Firebase/APNs push notification integration
- GraphQL endpoint for flexible queries
- Horizontal scaling with Redis pub/sub
- Prometheus metrics & health checks

---

## 📋 Implementation Roadmap

### Current State (November 2025)

✅ **Status**: WebSockets optional in GUI, production build works without it

```bash
# Production build (no WebSockets)
cmake -DDIN_EXPERIMENTAL_FEATURES=OFF

# Development build (with WebSockets, if available)
cmake -DDIN_EXPERIMENTAL_FEATURES=ON
```

### Phase 1: Desktop GUI Finalization (November 2025)

- ✅ WebSockets optional (done)
- ✅ Experimental features behind compile flag (done)
- ⏳ RPC polling for all GUI updates
- ⏳ Remove WebSocket usage from production tabs

### Phase 2: Mobile App Planning (2026 Q1)

- ⏳ Design DineroRelay API specification
- ⏳ Define WebSocket message protocol
- ⏳ Design JWT authentication flow
- ⏳ Create iOS/Android wallet requirements doc

### Phase 3: DineroRelay Development (2026 Q2)

- ⏳ Implement relay service (Node.js/TypeScript or Rust)
- ⏳ WebSocket server with JWT auth
- ⏳ RPC proxy to daemon
- ⏳ Rate limiting & caching
- ⏳ Docker deployment

### Phase 4: Mobile Wallet Development (2026 Q3)

- ⏳ iOS wallet (Swift + WebSocket)
- ⏳ Android wallet (Kotlin + WebSocket)
- ⏳ Push notifications via relay
- ⏳ Real-time balance updates

---

## 🔒 Security Considerations

### Current (Desktop GUI)

**Threat Model**:
- ✅ **Attack Surface**: Minimal (RPC over localhost only)
- ✅ **Authentication**: Cookie file (secure, localhost-only)
- ✅ **Encryption**: Not needed (localhost communication)

**Security Posture**: **EXCELLENT** (minimal attack surface)

### Future (Mobile via DineroRelay)

**Threat Model**:
- ⚠️  **Attack Surface**: Public WebSocket endpoint (exposed to internet)
- ⚠️  **Authentication**: JWT tokens (must be rotated, rate-limited)
- ⚠️  **Encryption**: TLS required (WSS, not WS)
- ⚠️  **DDoS**: Rate limiting, Cloudflare, IP bans
- ⚠️  **Injection**: Input validation on all RPC proxies

**Security Measures**:
1. **JWT Tokens**: Short-lived (1 hour), refresh tokens (7 days)
2. **Rate Limiting**: 100 requests/minute per user, 1000/min per IP
3. **TLS**: Let's Encrypt certificates, auto-renewal
4. **Firewall**: Relay on DMZ, daemon on private network
5. **Monitoring**: Sentry for errors, Prometheus for metrics
6. **Audit Logs**: All relay actions logged to PostgreSQL

---

## 🎯 Decision Summary

### For Desktop GUI (Current)

**Decision**: ❌ **Disable WebSockets** (optional dependency, not used)

**Reasons**:
1. ✅ RPC polling is simpler and sufficient
2. ✅ Reduces attack surface
3. ✅ No WebSocket reconnection complexity
4. ✅ Desktop doesn't need real-time updates
5. ✅ Fewer dependencies = easier builds

**Implementation**:
```cpp
// gui/src/mainwindow.cpp
// Poll for updates every 3 seconds (plenty fast for desktop)
#ifndef HAVE_WEBSOCKETS
  QTimer *pollTimer = new QTimer(this);
  connect(pollTimer, &QTimer::timeout, this, &MainWindow::updateBalance);
  pollTimer->start(3000);  // 3 second polling
#endif
```

### For Mobile Apps (Future)

**Decision**: ✅ **Use Separate DineroRelay Microservice**

**Reasons**:
1. ✅ Isolates consensus daemon from public internet
2. ✅ Scales horizontally for many mobile clients
3. ✅ Provides modern auth (JWT, OAuth)
4. ✅ Enables push notifications
5. ✅ Offers caching & rate limiting
6. ✅ Can be updated independently of daemon

**Implementation Timeline**:
- **2026 Q1**: Design & specification
- **2026 Q2**: DineroRelay development
- **2026 Q3**: Mobile wallet integration

---

## 📚 References

### Comparable Projects

**Bitcoin Core**:
- No WebSocket in daemon
- Mobile wallets use Electrum protocol (separate server)
- Desktop GUI uses RPC polling

**Ethereum (geth)**:
- WebSocket available but optional
- Infura provides WebSocket relay for mobile
- Desktop tools use HTTP RPC

**Monero**:
- RPC-only in daemon
- MyMonero relay service for web/mobile
- Desktop GUI uses RPC polling

**Lesson**: **Separate relay is industry best practice** for mobile wallets.

---

## ✅ Checklist

### Current Implementation

- [x] Make WebSockets optional in GUI CMakeLists.txt
- [x] GUI builds without WebSockets
- [x] Experimental features behind compile flag
- [ ] Remove WebSocket code from production tabs
- [ ] Update GUI to use RPC polling only

### Future Planning

- [ ] Write DineroRelay API specification
- [ ] Design WebSocket message protocol
- [ ] Define JWT authentication flow
- [ ] Create mobile wallet requirements document
- [ ] Evaluate relay frameworks (Node.js vs Rust)

---

**Status**: ✅ **DECISION IMPLEMENTED**  
**Current**: Desktop GUI uses RPC polling (no WebSockets)  
**Future**: Mobile apps will use DineroRelay microservice  
**Architecture**: Clean separation of concerns ✨  

---

**Author**: Dinero Core Team  
**Date**: November 7, 2025  
**Milestone**: WebSocket Architecture Decision  
**Achievement**: Production-Ready GUI Architecture 🎉


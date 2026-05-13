# Dinero Global Node Registry - Architecture

## System Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                    DINERO GLOBAL NODE REGISTRY                      │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐ │
│  │                    Registry Server (Python)                   │ │
│  │                                                               │ │
│  │  ┌─────────────┐      ┌──────────────┐     ┌──────────────┐ │ │
│  │  │  HTTP API   │◄────►│ State Manager│◄───►│ Stats Engine │ │ │
│  │  │ (Port 8080) │      │ (Thread-Safe)│     │  (Metrics)   │ │ │
│  │  └─────────────┘      └──────────────┘     └──────────────┘ │ │
│  │         ▲                      ▲                    ▲        │ │
│  │         │                      │                    │        │ │
│  │         │              ┌───────┴────────┐           │        │ │
│  │         │              │ Refresh Thread │           │        │ │
│  │         │              │  (Background)  │           │        │ │
│  │         │              └────────┬───────┘           │        │ │
│  │         │                       │                   │        │ │
│  └─────────┼───────────────────────┼───────────────────┼────────┘ │
│            │                       │                   │          │
└────────────┼───────────────────────┼───────────────────┼──────────┘
             │                       │                   │
             │                       │                   │
    ┌────────┴────────┐     ┌────────▼────────┐  ┌──────▼──────┐
    │   REST Clients  │     │  Node Queries   │  │   History   │
    │  (GUI/Web/CLI)  │     │  (HTTP Polls)   │  │   Storage   │
    └─────────────────┘     └─────────────────┘  └─────────────┘
```

## Data Flow

### 1. Node Health Monitoring (Background)

```
┌─────────────────────────────────────────────────────────────────┐
│                    Every 60 seconds                              │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
         ┌────────────────────────────────────────┐
         │   Refresh Thread Wakes Up              │
         └────────────────────────────────────────┘
                              │
                              ▼
         ┌────────────────────────────────────────┐
         │   Get List of Nodes to Monitor         │
         │   (Manual + Self-Registered)           │
         └────────────────────────────────────────┘
                              │
                              ▼
         ┌────────────────────────────────────────┐
         │   For Each Node:                       │
         │   1. Start timer                       │
         │   2. HTTP GET serverinfo.json          │
         │   3. Measure latency                   │
         │   4. Parse JSON response               │
         │   5. Update statistics                 │
         └────────────────────────────────────────┘
                              │
                              ▼
         ┌────────────────────────────────────────┐
         │   Aggregate Results                    │
         │   - Total nodes alive                  │
         │   - Average latency                    │
         │   - Success/failure counts             │
         └────────────────────────────────────────┘
                              │
                              ▼
         ┌────────────────────────────────────────┐
         │   Update Global State (Thread-Safe)    │
         │   - node_data                          │
         │   - node_stats                         │
         │   - node_history                       │
         └────────────────────────────────────────┘
                              │
                              ▼
         ┌────────────────────────────────────────┐
         │   Sleep for REFRESH_INTERVAL           │
         └────────────────────────────────────────┘
                              │
                              └─────► Loop
```

### 2. Client Request Flow

```
┌─────────────┐
│   Client    │  GET /nodes.json
│ (Browser)   ├────────────────────┐
└─────────────┘                    │
                                   ▼
                    ┌──────────────────────────┐
                    │   HTTP Request Handler   │
                    │   (Main Thread)          │
                    └──────────────────────────┘
                                   │
                                   ▼
                    ┌──────────────────────────┐
                    │   Acquire Read Lock      │
                    └──────────────────────────┘
                                   │
                                   ▼
                    ┌──────────────────────────┐
                    │   Read node_data         │
                    │   (Atomic Copy)          │
                    └──────────────────────────┘
                                   │
                                   ▼
                    ┌──────────────────────────┐
                    │   Release Lock           │
                    └──────────────────────────┘
                                   │
                                   ▼
                    ┌──────────────────────────┐
                    │   Serialize to JSON      │
                    └──────────────────────────┘
                                   │
                                   ▼
                    ┌──────────────────────────┐
                    │   Send HTTP 200 Response │
                    └──────────────────────────┘
                                   │
                                   ▼
                              ┌─────────┐
                              │ Client  │
                              └─────────┘
```

### 3. Self-Registration Flow (Extended Version)

```
┌──────────────┐
│ Dinero Node  │  POST /api/register
│ (New Node)   │  { "serverinfo_url": "http://..." }
└──────────────┘           │
                           ▼
            ┌──────────────────────────────┐
            │  Registry Server              │
            │  (POST Handler)               │
            └──────────────────────────────┘
                           │
                           ▼
            ┌──────────────────────────────┐
            │  Validate Request             │
            │  - JSON parseable?            │
            │  - URL format valid?          │
            │  - HMAC signature (optional)  │
            └──────────────────────────────┘
                           │
                   ┌───────┴───────┐
                   │               │
              ✅ Valid        ❌ Invalid
                   │               │
                   ▼               ▼
     ┌──────────────────┐  ┌──────────────────┐
     │ Check Capacity   │  │ Return HTTP 400  │
     │ (Max 100 nodes)  │  │ or 403           │
     └──────────────────┘  └──────────────────┘
                   │
           ┌───────┴───────┐
           │               │
      ✅ Space        ❌ Full
           │               │
           ▼               ▼
  ┌─────────────────┐  ┌──────────────────┐
  │ Acquire Lock    │  │ Return HTTP 429  │
  └─────────────────┘  └──────────────────┘
           │
           ▼
  ┌─────────────────┐
  │ Add to          │
  │ registered_nodes│
  └─────────────────┘
           │
           ▼
  ┌─────────────────┐
  │ Release Lock    │
  └─────────────────┘
           │
           ▼
  ┌─────────────────┐
  │ Return HTTP 201 │
  │ { "status": ... }│
  └─────────────────┘
           │
           ▼
  ┌─────────────────┐
  │ Node Added!     │
  │ Will be polled  │
  │ on next refresh │
  └─────────────────┘
```

## Component Architecture

### Registry Server Components

```
dinero_registry.py / dinero_registry_extended.py
│
├── Configuration Module
│   ├── DEFAULT_NODES[]          # Initial node list
│   ├── REFRESH_INTERVAL         # 60 seconds
│   ├── REQUEST_TIMEOUT          # 5 seconds
│   └── MAX_REGISTERED_NODES     # 100 (extended)
│
├── State Management (Thread-Safe)
│   ├── node_data{}              # Current aggregated state
│   ├── node_stats{}             # Per-node reliability metrics
│   ├── node_history[]           # Last 100 snapshots
│   ├── registered_nodes{}       # Self-registered nodes (extended)
│   └── lock                     # Threading.Lock for sync
│
├── Background Thread
│   ├── refresh_registry()       # Main polling loop
│   ├── measure_latency()        # HTTP timing
│   └── update_node_stats()      # Metrics calculation
│
├── HTTP Server
│   ├── RegistryHandler
│   │   ├── do_GET()            # Handle GET requests
│   │   │   ├── /nodes.json
│   │   │   ├── /api/status
│   │   │   ├── /api/stats
│   │   │   ├── /api/history
│   │   │   └── /
│   │   ├── do_POST()           # Handle POST requests (extended)
│   │   │   └── /api/register
│   │   └── do_OPTIONS()        # CORS preflight
│   └── run_server()            # Main entry point
│
└── Utility Functions
    ├── extract_node_id()        # Parse URL to ID
    ├── validate_url()           # URL format check
    ├── validate_hmac()          # Signature verification
    └── _generate_status_page()  # HTML dashboard
```

## Data Structures

### node_data (Primary State)

```python
{
    "timestamp": "2025-11-03T12:00:00Z",
    "total_nodes": 3,
    "total_configured": 3,        # Manual nodes
    "total_registered": 0,        # Self-registered (extended)
    "nodes": [
        {
            "name": "Virginia",
            "ip": "173.249.195.59",
            "rpc_port": 21999,
            "ws_port": 21000,
            "p2p_port": 20999,
            "network": "mainnet",
            "uptime": 86400,
            "connections": 8,
            "features": ["contracts", "bridge"],
            "source_url": "http://173.249.195.59:21999/serverinfo.json",
            "latency_ms": 42.5,
            "uptime_percentage": 99.8,
            "avg_latency_ms": 45.2,
            "registered": false       # Extended only
        }
    ]
}
```

### node_stats (Reliability Metrics)

```python
{
    "173.249.195.59:21999": {
        "total_queries": 1440,
        "successful_queries": 1438,
        "failed_queries": 2,
        "avg_latency_ms": 45.2,
        "last_seen": "2025-11-03T12:00:00Z",
        "uptime_percentage": 99.86,
        "first_seen": "2025-11-02T00:00:00Z",
        "registered": false
    }
}
```

### node_history (Historical Tracking)

```python
[
    {
        "timestamp": "2025-11-03T11:00:00Z",
        "total_nodes": 2
    },
    {
        "timestamp": "2025-11-03T11:01:00Z",
        "total_nodes": 3
    }
    # ... up to 100 entries
]
```

## Deployment Architectures

### 1. Simple Deployment (Development)

```
┌─────────────────────────┐
│  Your Machine           │
│                         │
│  ┌──────────────────┐   │
│  │ dinero_registry  │   │
│  │   :8080          │   │
│  └──────────────────┘   │
│                         │
└─────────────────────────┘
          ▲
          │
    HTTP Requests
```

### 2. Production Deployment (Single Server)

```
┌─────────────────────────────────────────┐
│  VPS / Cloud Server                     │
│                                         │
│  ┌───────────────────────────────────┐  │
│  │  Nginx (:443, :80)                │  │
│  │  - SSL/TLS                        │  │
│  │  - Rate Limiting                  │  │
│  │  - Caching                        │  │
│  └───────────┬───────────────────────┘  │
│              │                          │
│              ▼                          │
│  ┌───────────────────────────────────┐  │
│  │  Registry Server (:8080)          │  │
│  │  - systemd service                │  │
│  │  - Auto-restart                   │  │
│  └───────────────────────────────────┘  │
│                                         │
└─────────────────────────────────────────┘
```

### 3. Docker Deployment

```
┌─────────────────────────────────────────┐
│  Host Machine                           │
│                                         │
│  ┌───────────────────────────────────┐  │
│  │  Docker Container                 │  │
│  │  ┌─────────────────────────────┐  │  │
│  │  │  dinero-registry            │  │  │
│  │  │  Python 3.11 + requests     │  │  │
│  │  │  Port: 8080                 │  │  │
│  │  └─────────────────────────────┘  │  │
│  └───────────────────────────────────┘  │
│              │                          │
│              ▼                          │
│     Host Port :8080                     │
│                                         │
└─────────────────────────────────────────┘
```

### 4. High Availability (Future)

```
                    ┌──────────────┐
                    │  Load        │
                    │  Balancer    │
                    └───────┬──────┘
                            │
              ┌─────────────┼─────────────┐
              │             │             │
              ▼             ▼             ▼
    ┌────────────┐  ┌────────────┐  ┌────────────┐
    │ Registry 1 │  │ Registry 2 │  │ Registry 3 │
    │ US-East    │  │ US-West    │  │ EU-Central │
    └────────────┘  └────────────┘  └────────────┘
              │             │             │
              └─────────────┼─────────────┘
                            ▼
                  ┌──────────────────┐
                  │  Shared Database │
                  │  (PostgreSQL)    │
                  └──────────────────┘
```

## Security Architecture

### Defense Layers

```
┌─────────────────────────────────────────────────────────┐
│  Layer 1: Network (Nginx)                               │
│  - Rate limiting (60 req/min API, 10 req/min register) │
│  - HTTPS/TLS 1.2+                                       │
│  - DDoS protection                                      │
└─────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────┐
│  Layer 2: Application (Python)                          │
│  - Input validation (URL format, JSON schema)          │
│  - HMAC signature verification (optional)              │
│  - Max node limit (prevent spam)                       │
└─────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────┐
│  Layer 3: System (OS)                                   │
│  - Non-root user                                        │
│  - Resource limits (MemoryMax, LimitNOFILE)            │
│  - Read-only filesystems                               │
└─────────────────────────────────────────────────────────┘
```

## Performance Characteristics

### Time Complexity

- **GET /nodes.json**: O(1) - Direct dict access + JSON serialization
- **GET /api/status**: O(1) - Simple dict reads
- **POST /api/register**: O(1) - Set insertion (hash table)
- **Node Health Check**: O(N) - Linear in number of nodes

### Space Complexity

- **node_data**: O(N) - N = number of nodes
- **node_stats**: O(N) - Per-node metrics
- **node_history**: O(H) - H = MAX_HISTORY (100)
- **Total Memory**: ~1 KB per node + overhead (~50-100 MB Python)

### Concurrency Model

```
┌────────────────────────────────────────┐
│  Main Thread (HTTP Server)             │
│  - Handles all client requests         │
│  - Reads from shared state (locked)    │
└────────────────────────────────────────┘
                  │
                  │ Shared State
                  │ (Protected by Lock)
                  │
┌────────────────────────────────────────┐
│  Background Thread (Refresh)           │
│  - Polls nodes every 60s               │
│  - Writes to shared state (locked)     │
└────────────────────────────────────────┘
```

## Integration Points

### 1. GUI Wallet Integration

```
Dinero-Qt                  Registry              Nodes
    │                         │                    │
    ├──GET /nodes.json──────>│                    │
    │                         │                    │
    │<───[node list]──────────┤                    │
    │                         │                    │
    │  Parse & Sort by        │                    │
    │  latency_ms             │                    │
    │                         │                    │
    ├──WebSocket Connect─────────────────────────>│
    │                         │                    │
    │<──────────────────Block/Tx Data─────────────┤
```

### 2. Daemon Auto-Registration

```
dinerod Startup          Registry
    │                       │
    │  Wait for RPC         │
    ├───────────────────────┤
    │                       │
    │  POST /api/register───>
    │  Every 10 min         │
    │                       │
    │<──201 Created─────────┤
    │                       │
    │  Continue running     │
```

## Monitoring & Observability

### Logs

```bash
# Registry logs (stdout)
[OK] 173.249.195.59:21999 - 42.5ms - Virginia
[WARN] 127.0.0.1:21999 - timeout after 5s
[Registry] Updated: 2/3 nodes alive
[✓] Registered: 127.0.0.1:21999 - LocalNode
```

### Metrics (Future Prometheus Export)

```
# HELP dinero_nodes_total Total configured nodes
# TYPE dinero_nodes_total gauge
dinero_nodes_total 3

# HELP dinero_nodes_alive Number of responsive nodes
# TYPE dinero_nodes_alive gauge
dinero_nodes_alive 2

# HELP dinero_node_latency_ms Node latency in milliseconds
# TYPE dinero_node_latency_ms gauge
dinero_node_latency_ms{node="173.249.195.59:21999"} 42.5
```

---

**Architecture Design Principles:**
- ✅ **Simplicity** - Easy to understand and modify
- ✅ **Reliability** - Graceful error handling, auto-recovery
- ✅ **Performance** - Low latency, minimal resource usage
- ✅ **Security** - Multiple defense layers, input validation
- ✅ **Scalability** - Linear scaling up to 100s of nodes
- ✅ **Observability** - Comprehensive logging and metrics

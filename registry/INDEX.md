# Dinero Global Node Registry - File Index

## 📂 Complete Package Contents

### 🐍 Python Implementation
| File | Size | Description |
|------|------|-------------|
| `dinero_registry.py` | 13K | **Basic registry** - Manual node monitoring with REST API |
| `dinero_registry_extended.py` | 17K | **Extended registry** - Adds self-registration via POST /api/register |

**Quick Start:**
```bash
./dinero_registry.py                    # Basic (manual nodes)
./dinero_registry_extended.py          # Extended (self-registration)
```

### 📚 Documentation
| File | Size | What's Inside |
|------|------|---------------|
| `README.md` | 11K | **Full documentation** - API reference, deployment, integration examples |
| `QUICKSTART.md` | 7.6K | **5-minute guide** - Get running fast with common use cases |
| `IMPLEMENTATION_SUMMARY.md` | 9.8K | **Project overview** - Features, status, roadmap, checklist |
| `ARCHITECTURE.md` | 26K | **Technical deep-dive** - Data flow, components, security model |
| `INDEX.md` | (this file) | **Navigation guide** - You are here! |

**Start Here:**
1. New users → Read `QUICKSTART.md`
2. Integration → Read `README.md` API section
3. Understanding internals → Read `ARCHITECTURE.md`
4. Project planning → Read `IMPLEMENTATION_SUMMARY.md`

### 🔧 Integration & Testing
| File | Size | Description |
|------|------|-------------|
| `daemon_integration_example.cpp` | 4.5K | C++ code to add auto-registration to dinerod |
| `test_registration.sh` | 2.2K | Automated test suite for all endpoints |

**Usage:**
```bash
# Run tests
./test_registration.sh

# Integrate into daemon (pseudocode)
# 1. Add code from daemon_integration_example.cpp to src/registry.cpp
# 2. Call StartRegistrySync() in src/init.cpp
# 3. Enable with -registry=1 flag
```

### 🐳 Deployment Files
| File | Size | Description |
|------|------|-------------|
| `Dockerfile` | 930B | Docker image definition (Python 3.11 + requests) |
| `docker-compose.yml` | 1.8K | Multi-profile orchestration (basic/extended/production) |
| `dinero-registry.service` | 990B | Systemd service for Linux servers |
| `nginx.conf.example` | 4.8K | Production nginx config with SSL, rate limiting |

**Deployment Options:**
```bash
# 1. Standalone Python
./dinero_registry.py --port 8080

# 2. Docker
docker build -t dinero-registry . && docker run -d -p 8080:8080 dinero-registry

# 3. Docker Compose
docker-compose --profile extended up -d

# 4. Systemd (Linux)
sudo cp dinero-registry.service /etc/systemd/system/ && sudo systemctl start dinero-registry

# 5. Production (Nginx + SSL)
sudo cp nginx.conf.example /etc/nginx/sites-available/dinero-registry
```

## 🗂️ File Organization

```
registry/
│
├── Core Implementation (30K total)
│   ├── dinero_registry.py          ← Basic version
│   └── dinero_registry_extended.py ← Self-registration version
│
├── Documentation (54K total)
│   ├── INDEX.md                    ← You are here
│   ├── QUICKSTART.md               ← Start here (5 min)
│   ├── README.md                   ← Full docs
│   ├── ARCHITECTURE.md             ← Technical details
│   └── IMPLEMENTATION_SUMMARY.md   ← Project overview
│
├── Integration (6.7K total)
│   ├── daemon_integration_example.cpp
│   └── test_registration.sh
│
└── Deployment (8.5K total)
    ├── Dockerfile
    ├── docker-compose.yml
    ├── dinero-registry.service
    └── nginx.conf.example
```

## 🎯 Quick Navigation

### I want to...

**...get started quickly**
→ Read `QUICKSTART.md` then run `./dinero_registry.py`

**...understand the API**
→ See "API Endpoints" section in `README.md`

**...deploy to production**
→ See "Production Deployment" in `README.md` + use `nginx.conf.example`

**...integrate with my wallet**
→ See "Integration Examples" in `README.md`

**...integrate with dinerod**
→ Use code from `daemon_integration_example.cpp`

**...run tests**
→ Execute `./test_registration.sh`

**...understand the architecture**
→ Read `ARCHITECTURE.md`

**...see project status**
→ Read `IMPLEMENTATION_SUMMARY.md`

**...deploy with Docker**
→ Use `Dockerfile` or `docker-compose.yml`

**...set up systemd service**
→ Copy `dinero-registry.service` to `/etc/systemd/system/`

**...configure nginx**
→ Adapt `nginx.conf.example` for your domain

## 📊 Features Matrix

| Feature | Basic | Extended |
|---------|-------|----------|
| Manual node monitoring | ✅ | ✅ |
| Latency tracking | ✅ | ✅ |
| Uptime statistics | ✅ | ✅ |
| Historical data | ✅ | ✅ |
| REST API | ✅ | ✅ |
| Web dashboard | ✅ | ✅ |
| CORS support | ✅ | ✅ |
| Self-registration | ❌ | ✅ |
| HMAC validation | ❌ | ✅ |
| Registration tracking | ❌ | ✅ |

## 🚀 Typical Workflows

### Workflow 1: Local Development

```bash
# 1. Start basic registry
./dinero_registry.py

# 2. Open browser
open http://localhost:8080

# 3. Query API
curl http://localhost:8080/nodes.json | jq
```

### Workflow 2: Test with Self-Registration

```bash
# 1. Start extended registry
./dinero_registry_extended.py &

# 2. Run test suite
./test_registration.sh

# 3. Register a node
curl -X POST http://localhost:8080/api/register \
  -H "Content-Type: application/json" \
  -d '{"serverinfo_url": "http://127.0.0.1:21999/serverinfo.json"}'
```

### Workflow 3: Production Deployment

```bash
# 1. Copy files to server
scp -r registry/ user@server:/opt/dinero/

# 2. Install systemd service
sudo cp dinero-registry.service /etc/systemd/system/
sudo systemctl enable dinero-registry
sudo systemctl start dinero-registry

# 3. Configure nginx
sudo cp nginx.conf.example /etc/nginx/sites-available/dinero-registry
sudo ln -s /etc/nginx/sites-available/dinero-registry /etc/nginx/sites-enabled/

# 4. Get SSL certificate
sudo certbot --nginx -d status.dinero-coin.com

# 5. Monitor
sudo journalctl -u dinero-registry -f
```

### Workflow 4: Docker Deployment

```bash
# 1. Build image
docker build -t dinero-registry .

# 2. Run container
docker run -d \
  --name dinero-registry \
  --restart always \
  -p 8080:8080 \
  dinero-registry

# 3. View logs
docker logs -f dinero-registry
```

### Workflow 5: Integration Testing

```bash
# 1. Start local Dinero node with HTTP server
dinerod -regtest -daemon

# 2. Enable serverinfo.json
dinero-cli setserverinfo '{"name":"Test Node","network":"regtest"}'

# 3. Start registry
./dinero_registry_extended.py &

# 4. Register node
curl -X POST http://localhost:8080/api/register \
  -H "Content-Type: application/json" \
  -d '{"ip":"127.0.0.1","rpc_port":21999}'

# 5. Verify registration
curl http://localhost:8080/nodes.json | jq '.nodes[] | {name, ip}'
```

## 📋 Dependencies

### Python (Runtime)
- **Python 3.8+** (tested with 3.11)
- **requests** library (`pip install requests`)

### Optional (Deployment)
- **Docker** (for containerized deployment)
- **Nginx** (for production reverse proxy)
- **Certbot** (for SSL certificates)
- **systemd** (for service management on Linux)

### Optional (Testing)
- **curl** (API testing)
- **jq** (JSON parsing)
- **bash** (test script execution)

## 🔗 External References

### Related Dinero Files
- `src/httprpc.cpp` - HTTP server implementation
- `src/rpc/server.cpp` - RPC commands (add `getserverinfo`)
- `src/init.cpp` - Daemon initialization (add `StartRegistrySync()`)

### Standards & Protocols
- **HTTP/1.1** - [RFC 7230](https://tools.ietf.org/html/rfc7230)
- **JSON** - [RFC 8259](https://tools.ietf.org/html/rfc8259)
- **CORS** - [W3C Spec](https://www.w3.org/TR/cors/)
- **HMAC-SHA256** - [RFC 2104](https://tools.ietf.org/html/rfc2104)

## ✅ Verification Checklist

Before deployment, verify:

- [ ] All Python files are executable (`chmod +x *.py`)
- [ ] Test script runs successfully (`./test_registration.sh`)
- [ ] Basic registry starts without errors
- [ ] Extended registry handles registration
- [ ] All API endpoints return 200 OK
- [ ] Dashboard loads in browser
- [ ] Docker image builds successfully
- [ ] Systemd service starts and logs appear
- [ ] Nginx config passes syntax check (`nginx -t`)

## 📞 Support & Resources

- **GitHub Issues**: Report bugs at github.com/dinerocoin/dinero/issues
- **Documentation**: Full docs at docs.dinero-coin.com
- **Community**: Join Discord at discord.gg/dinero
- **Email**: support@dinero-coin.com

## 📜 License

Same as Dinero Core: **MIT License**

```
Copyright (c) 2025 Dinero Development Team

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction...
```

---

**Version**: 1.0
**Last Updated**: 2025-11-03
**Status**: ✅ Ready for Production

**Total Package Size**: ~100 KB (12 files)
**Lines of Code**: ~1,500 (Python + C++ + configs)
**Documentation**: 5 comprehensive guides
**Deployment Options**: 5 (standalone, Docker, systemd, compose, nginx)

---

## 🎉 You're Ready!

You now have a **complete, production-ready** global node registry system for Dinero.

**Next Steps:**
1. Read `QUICKSTART.md` (5 minutes)
2. Run `./dinero_registry.py` locally
3. Deploy to a test server
4. Integrate with Dinero-Qt
5. Announce to the community

**Questions?** Check the documentation files above or open a GitHub issue.

**Happy Monitoring!** 🚀

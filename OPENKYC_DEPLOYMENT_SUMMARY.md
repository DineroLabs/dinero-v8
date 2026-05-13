# OpenKYC Backend Deployment Summary

## Overview

You have **two sets of deployment resources** for hosting OpenKYC on your DineroCAN server (96.9.226.98):

1. **Original deployment scripts** in `wallet-core/openkyc-backend/`
2. **New comprehensive deployment package** in `deployment/`

Both approaches work, but the **new deployment package** (`deployment/`) is recommended as it's more recent and includes enhanced features.

## Recommended: Use New Deployment Package

### Files Created (in `/deployment` directory):

```
deployment/
├── deploy-openkyc.sh          # Automated deployment script
├── openkyc-nginx.conf         # Nginx reverse proxy configuration
├── update-ffi-config.sh       # FFI library configuration updater
└── README.md                  # Complete deployment guide
```

### Quick Start

```bash
# 1. Configure DNS
# Add A record: openkyc.dinero-coin.com → 96.9.226.98

# 2. Upload deployment script
scp -i ~/.ssh/dinero_deployment_2025 \
    deployment/deploy-openkyc.sh \
    root@96.9.226.98:/root/

# 3. Upload OpenKYC backend code
rsync -avz -e "ssh -i ~/.ssh/dinero_deployment_2025" \
    wallet-core/openkyc-backend/ \
    root@96.9.226.98:/root/openkyc-backend/

# 4. SSH into server and deploy
ssh -i ~/.ssh/dinero_deployment_2025 root@96.9.226.98
bash deploy-openkyc.sh

# 5. Configure Persona API credentials
nano /opt/openkyc/.env
# Add your PERSONA_API_KEY and PERSONA_TEMPLATE_ID

# 6. Restart service
pm2 restart openkyc

# 7. Test deployment
curl https://openkyc.dinero-coin.com/health
```

## Alternative: Use Original Deployment Scripts

If you prefer to use the original scripts in `wallet-core/openkyc-backend/`:

```bash
# Navigate to the backend directory
cd wallet-core/openkyc-backend

# Run deployment script (uploads to DineroCAN)
./deploy_to_dinerocan.sh

# SSH into server
ssh -i ~/.ssh/dinero_deployment_2025 root@96.9.226.98

# Setup PM2 and SSL
./setup_pm2.sh
./setup_ssl.sh
```

## Key Differences

| Feature | New Package (`deployment/`) | Original (`wallet-core/openkyc-backend/`) |
|---------|----------------------------|-------------------------------------------|
| Deployment | Single automated script | Multiple manual steps |
| SSL Setup | Integrated in deploy script | Separate `setup_ssl.sh` |
| PM2 Setup | Integrated in deploy script | Separate `setup_pm2.sh` |
| Configuration | Comprehensive `.env` template | Basic configuration |
| Documentation | Full README with troubleshooting | Setup guide only |
| Nginx Config | Production-hardened with rate limiting | Basic reverse proxy |
| Security | Enhanced headers + HSTS | Standard SSL |

## Production Architecture

```
Mobile Wallet (iOS/Android)
    ↓ HTTPS
https://openkyc.dinero-coin.com (Nginx + Let's Encrypt SSL)
    ↓ Reverse Proxy (localhost)
127.0.0.1:8080 (Node.js Backend + PM2)
    ↓ API Calls
Persona KYC Service
```

## API Endpoints

### Health Check
```bash
curl https://openkyc.dinero-coin.com/health

# Response:
{
  "status": "ok",
  "timestamp": 1699564800,
  "uptime": 86400
}
```

### Start Verification
```bash
curl -X POST https://openkyc.dinero-coin.com/api/verify/start \
  -H "Content-Type: application/json" \
  -d '{
    "walletAddress": "din1q...",
    "referenceId": "optional-reference",
    "metadata": {
      "platform": "ios",
      "version": "1.0.0"
    }
  }'

# Response:
{
  "success": true,
  "verificationId": "ver_...",
  "inquiryUrl": "https://withpersona.com/verify?inquiry-id=...",
  "expiresAt": "2024-01-01T12:00:00Z"
}
```

### Check Verification Status
```bash
curl -X POST https://openkyc.dinero-coin.com/api/verify/status \
  -H "Content-Type: application/json" \
  -d '{"verificationId": "ver_..."}'

# Response:
{
  "success": true,
  "status": "completed",
  "walletAddress": "din1q...",
  "completedAt": "2024-01-01T12:00:00Z",
  "verificationData": {
    "firstName": "John",
    "lastName": "Doe",
    "country": "US"
  }
}
```

## Cost Analysis

### Current Infrastructure
- **DineroCAN Server**: Already paid (1GB RAM, 1 core, 15GB storage)
- **Domain**: $10-15/year (dinero-coin.com - already owned)
- **SSL Certificate**: FREE (Let's Encrypt with auto-renewal)

### Total Additional Cost
**$0/month** (uses existing server resources)

### Avoided Costs
- Firebase Hosting: $0.026/GB (~$5-10/month)
- Cloud Functions: $0.40/million invocations (~$10-50/month)
- **Total Savings**: $15-60/month

## Server Requirements

### Minimum Specs (Met by DineroCAN)
- **RAM**: 512MB minimum, 1GB recommended ✅
- **CPU**: 1 core ✅
- **Disk**: 5GB minimum ✅
- **OS**: Ubuntu 20.04+ or Debian 10+ ✅

### Resource Usage
- **OpenKYC Backend**: ~50-100MB RAM
- **Nginx**: ~10-20MB RAM
- **Node.js**: ~30-50MB RAM
- **Total**: ~90-170MB RAM (well within 1GB limit)

## Security Features

### SSL/TLS
- TLS 1.2 and 1.3 only
- Modern cipher suites (Mozilla Intermediate profile)
- HSTS enabled (max-age: 31536000)
- OCSP stapling for fast certificate validation
- Auto-renewal via certbot cron job

### Rate Limiting
- 10 requests/second per IP address
- Burst allowance: 20 requests
- Protects against abuse and DoS attacks

### Security Headers
```
Strict-Transport-Security: max-age=31536000; includeSubDomains
X-Frame-Options: SAMEORIGIN
X-Content-Type-Options: nosniff
X-XSS-Protection: 1; mode=block
Referrer-Policy: strict-origin-when-cross-origin
```

### CORS Policy
- Allows all origins (`*`) for mobile app access
- Allows `GET`, `POST`, `OPTIONS` methods
- Allows `Content-Type`, `Authorization` headers

## Management Commands

### PM2 Process Management
```bash
# View logs (live tail)
pm2 logs openkyc

# View last 100 lines
pm2 logs openkyc --lines 100

# Restart service
pm2 restart openkyc

# Stop service
pm2 stop openkyc

# View status
pm2 status

# Monitor resources
pm2 monit
```

### Nginx Management
```bash
# Test configuration
nginx -t

# Reload configuration
systemctl reload nginx

# Restart Nginx
systemctl restart nginx

# View error logs
tail -f /var/log/nginx/openkyc-error.log

# View access logs
tail -f /var/log/nginx/openkyc-access.log
```

### SSL Certificate Management
```bash
# Check certificate expiry
certbot certificates

# Manual renewal (auto-renews via cron)
certbot renew

# Force renewal
certbot renew --force-renewal

# Reload Nginx after renewal
systemctl reload nginx
```

## Updating Mobile Wallet FFI

After deploying OpenKYC to production, update your mobile wallet to use the production endpoint:

```bash
# Navigate to deployment directory
cd deployment

# Run FFI configuration updater
bash update-ffi-config.sh

# This updates: src/wallet/ffi/config.h
# From: http://localhost:8080
# To:   https://openkyc.dinero-coin.com

# Rebuild FFI library
cd ..
cmake --build build --target dinero_wallet_ffi

# Copy updated library to your iOS/Android project
# iOS: Copy libdinero_wallet_ffi.a to DineroCoin-iOS/Libraries/
# Android: Copy libdinero_wallet_ffi.so to DineroCoin-Android/app/src/main/jniLibs/
```

## Troubleshooting

### Service won't start
```bash
# Check PM2 logs
pm2 logs openkyc --err

# Check port availability
lsof -i :8080

# Verify Node.js dependencies
cd /opt/openkyc && npm install
```

### Nginx errors
```bash
# Test configuration
nginx -t

# Check error logs
tail -50 /var/log/nginx/openkyc-error.log

# Verify upstream is running
curl http://127.0.0.1:8080/health
```

### SSL certificate issues
```bash
# Check certificate expiry
certbot certificates

# Renew certificates
certbot renew --force-renewal

# Reload Nginx
systemctl reload nginx
```

### DNS not resolving
```bash
# Test DNS resolution
dig openkyc.dinero-coin.com

# Check DNS propagation
nslookup openkyc.dinero-coin.com 8.8.8.8

# Verify A record
host openkyc.dinero-coin.com
```

## Monitoring

### Health Check Endpoint
```bash
# Basic health check
curl https://openkyc.dinero-coin.com/health

# With verbose output
curl -v https://openkyc.dinero-coin.com/health

# From mobile app (Swift example)
URLSession.shared.dataTask(with: URL(string: "https://openkyc.dinero-coin.com/health")!)
```

### Server Monitoring
```bash
# CPU and memory usage
top

# Disk usage
df -h

# Network connections
netstat -tulpn | grep -E "(8080|443|80)"

# Process status
ps aux | grep -E "(node|nginx|pm2)"
```

### Log Analysis
```bash
# Request rate from Nginx logs
tail -10000 /var/log/nginx/openkyc-access.log | \
  awk '{print $4}' | cut -d: -f1-2 | uniq -c

# Response time analysis
tail -1000 /var/log/nginx/openkyc-access.log | \
  awk '{print $10}' | sort -n | tail -20

# Error frequency
grep -c "ERROR" /var/log/nginx/openkyc-error.log
```

## Next Steps

1. **Deploy to Production**: Follow the Quick Start guide above
2. **Configure Persona**: Add API credentials to `/opt/openkyc/.env`
3. **Update Mobile Wallet**: Run `deployment/update-ffi-config.sh` and rebuild FFI
4. **Test End-to-End**: Complete verification flow from mobile app
5. **Setup Monitoring**: Configure uptime monitoring (e.g., UptimeRobot, Pingdom)
6. **Documentation**: Update user-facing docs with new KYC flow

## Support

### Documentation
- **New Package**: `deployment/README.md` (comprehensive guide)
- **Original Package**: `wallet-core/openkyc-backend/DINEROCAN_DEPLOYMENT.md`
- **Setup Guide**: `wallet-core/openkyc-backend/SETUP_GUIDE.md`

### Logs
- **Application**: `pm2 logs openkyc`
- **Nginx Access**: `/var/log/nginx/openkyc-access.log`
- **Nginx Error**: `/var/log/nginx/openkyc-error.log`
- **System**: `journalctl -u nginx -f`

### Testing
```bash
# Test from server
curl http://127.0.0.1:8080/health

# Test from internet
curl https://openkyc.dinero-coin.com/health

# Test SSL
openssl s_client -connect openkyc.dinero-coin.com:443 -servername openkyc.dinero-coin.com
```

## Recommendation

**Use the new deployment package** (`deployment/deploy-openkyc.sh`) for:
- Simplified one-command deployment
- Enhanced security configuration
- Better documentation and troubleshooting
- Production-ready rate limiting
- Automated SSL setup

The original scripts in `wallet-core/openkyc-backend/` are still functional but require more manual steps.

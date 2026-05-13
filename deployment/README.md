# OpenKYC Production Deployment Guide

This guide covers deploying the OpenKYC backend to your DineroCAN server (96.9.226.98 / dinero-coin.com).

## Prerequisites

- DineroCAN server access (SSH key: `~/.ssh/dinero_deployment_2025`)
- Domain DNS configured: `openkyc.dinero-coin.com` → `96.9.226.98`
- Persona API credentials (sign up at https://withpersona.com)

## Architecture

```
Mobile Wallet (iOS/Android)
    ↓ HTTPS
openkyc.dinero-coin.com (Nginx + Let's Encrypt)
    ↓ Reverse Proxy
127.0.0.1:8080 (Node.js + PM2)
    ↓ API Calls
Persona KYC Service
```

## Deployment Steps

### 1. Configure DNS

Add an A record for your subdomain:

```
Type: A
Name: openkyc
Value: 96.9.226.98
TTL: 3600
```

Verify DNS propagation:
```bash
dig openkyc.dinero-coin.com
```

### 2. Upload deployment files to server

From your local machine:

```bash
# Copy deployment scripts to server
scp -i ~/.ssh/dinero_deployment_2025 \
    deployment/deploy-openkyc.sh \
    root@96.9.226.98:/root/

# Upload OpenKYC backend code
rsync -avz -e "ssh -i ~/.ssh/dinero_deployment_2025" \
    wallet-core/openkyc-backend/ \
    root@96.9.226.98:/root/openkyc-backend/
```

### 3. Run deployment script

SSH into the server and run the automated deployment:

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@96.9.226.98

cd /root
chmod +x deploy-openkyc.sh
bash deploy-openkyc.sh
```

The script will:
1. Install Node.js, Nginx, Certbot
2. Install PM2 process manager
3. Copy backend files to `/opt/openkyc`
4. Configure Nginx reverse proxy
5. Obtain Let's Encrypt SSL certificate
6. Start OpenKYC with PM2

### 4. Configure Persona API credentials

Edit the `.env` file on the server:

```bash
nano /opt/openkyc/.env
```

Add your Persona credentials:

```env
PORT=8080
NODE_ENV=production
LOG_LEVEL=info

# Persona API credentials
PERSONA_API_KEY=your_actual_api_key_here
PERSONA_TEMPLATE_ID=your_template_id_here
```

Restart the service:

```bash
pm2 restart openkyc
```

### 5. Test the deployment

```bash
# Health check
curl https://openkyc.dinero-coin.com/health

# Verification endpoint
curl -X POST https://openkyc.dinero-coin.com/api/verify/start \
  -H "Content-Type: application/json" \
  -d '{"walletAddress": "din1qtest123456789"}'
```

Expected response:
```json
{
  "success": true,
  "verificationId": "ver_...",
  "inquiryUrl": "https://withpersona.com/verify?inquiry-id=..."
}
```

### 6. Update mobile wallet configuration

On your local machine, update the FFI configuration:

```bash
cd deployment
bash update-ffi-config.sh
```

This updates `src/wallet/ffi/config.h` to use the production URL.

Rebuild the FFI library:

```bash
cd ..
cmake --build build --target dinero_wallet_ffi
```

Copy the updated library to your mobile wallet project.

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

# Start service
pm2 start openkyc

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

### SSL Certificate Renewal

Let's Encrypt certificates auto-renew via cron. To manually renew:

```bash
certbot renew
systemctl reload nginx
```

## Security Features

### Rate Limiting
- 10 requests/second per IP address
- Burst allowance: 20 requests
- Protects against abuse and DoS attacks

### HTTPS/TLS
- TLS 1.2 and 1.3 only
- Modern cipher suites (Mozilla Intermediate profile)
- HSTS enabled (strict transport security)
- OCSP stapling for fast certificate validation

### Security Headers
- `X-Frame-Options: SAMEORIGIN` (prevents clickjacking)
- `X-Content-Type-Options: nosniff` (prevents MIME sniffing)
- `X-XSS-Protection: 1; mode=block` (XSS protection)
- `Referrer-Policy: strict-origin-when-cross-origin`

### CORS Policy
- Allows all origins (`*`) for API endpoints
- Allows `GET`, `POST`, `OPTIONS` methods
- Allows `Content-Type`, `Authorization` headers

## Monitoring

### Check service status

```bash
# PM2 status
pm2 status

# Process info
pm2 info openkyc

# CPU and memory usage
pm2 monit
```

### View logs

```bash
# Application logs
pm2 logs openkyc

# Nginx access logs
tail -f /var/log/nginx/openkyc-access.log

# Nginx error logs
tail -f /var/log/nginx/openkyc-error.log

# System logs
journalctl -u nginx -f
```

### Performance metrics

```bash
# Request rate from Nginx logs
tail -10000 /var/log/nginx/openkyc-access.log | awk '{print $4}' | cut -d: -f1-2 | uniq -c

# Response time analysis
tail -1000 /var/log/nginx/openkyc-access.log | awk '{print $10}' | sort -n | tail -20
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
```

## Cost Analysis

### Current Costs
- **VPS Hosting**: Already paid (DineroCAN server)
- **Domain**: $10-15/year (already owned)
- **SSL Certificate**: FREE (Let's Encrypt)
- **Total Additional Cost**: $0/month

### Avoided Costs
- Firebase Hosting: $0.026/GB (~$5-10/month)
- Cloud Functions: $0.40/million invocations (~$10-50/month)
- **Total Savings**: $15-60/month

## API Endpoints

### Health Check
```
GET https://openkyc.dinero-coin.com/health
```

Response:
```json
{
  "status": "ok",
  "timestamp": 1699564800,
  "uptime": 86400
}
```

### Start Verification
```
POST https://openkyc.dinero-coin.com/api/verify/start
Content-Type: application/json

{
  "walletAddress": "din1q...",
  "referenceId": "optional-reference",
  "metadata": {
    "platform": "ios",
    "version": "1.0.0"
  }
}
```

Response:
```json
{
  "success": true,
  "verificationId": "ver_...",
  "inquiryUrl": "https://withpersona.com/verify?inquiry-id=...",
  "expiresAt": "2024-01-01T12:00:00Z"
}
```

### Check Verification Status
```
POST https://openkyc.dinero-coin.com/api/verify/status
Content-Type: application/json

{
  "verificationId": "ver_..."
}
```

Response:
```json
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

## Next Steps

1. **Mobile Wallet Integration**: Update your iOS/Android app to use `https://openkyc.dinero-coin.com`
2. **Testing**: Run end-to-end verification flow from mobile app
3. **Monitoring**: Set up uptime monitoring (e.g., UptimeRobot, Pingdom)
4. **Backup**: Configure automatic backups of verification data
5. **Documentation**: Update user-facing docs with new KYC flow

## Support

For issues or questions:
- Check logs: `pm2 logs openkyc`
- Review Nginx logs: `/var/log/nginx/openkyc-*.log`
- Test locally: `curl https://openkyc.dinero-coin.com/health`

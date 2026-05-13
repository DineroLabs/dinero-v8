#!/bin/bash
# OpenKYC Backend Deployment Script
# Run this on your DineroCAN server (96.9.226.98)
# Usage: bash deploy-openkyc.sh

set -e  # Exit on error

echo "=========================================="
echo "OpenKYC Backend Deployment"
echo "Server: dinero-coin.com (96.9.226.98)"
echo "=========================================="
echo ""

# Configuration
DOMAIN="openkyc.dinero-coin.com"
APP_DIR="/opt/openkyc"
BACKEND_PORT=8080
EMAIL="admin@dinero-coin.com"  # Change this to your email

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Step 1: Update system packages
echo -e "${YELLOW}[1/8]${NC} Updating system packages..."
apt-get update -qq
apt-get upgrade -y -qq

# Step 2: Install dependencies
echo -e "${YELLOW}[2/8]${NC} Installing dependencies (Node.js, Nginx, Certbot)..."
if ! command -v node &> /dev/null; then
    curl -fsSL https://deb.nodesource.com/setup_18.x | bash -
    apt-get install -y nodejs
fi

apt-get install -y nginx certbot python3-certbot-nginx

# Step 3: Install PM2 globally
echo -e "${YELLOW}[3/8]${NC} Installing PM2 process manager..."
npm install -g pm2

# Step 4: Create application directory
echo -e "${YELLOW}[4/8]${NC} Setting up application directory..."
mkdir -p $APP_DIR
mkdir -p /var/www/certbot

# Step 5: Copy backend files
echo -e "${YELLOW}[5/8]${NC} Copying OpenKYC backend files..."
if [ -d "./openkyc-backend" ]; then
    cp -r ./openkyc-backend/* $APP_DIR/
else
    echo -e "${RED}Error: openkyc-backend directory not found${NC}"
    echo "Please run this script from the directory containing openkyc-backend/"
    exit 1
fi

# Install Node.js dependencies
cd $APP_DIR
npm install --production

# Create .env file if it doesn't exist
if [ ! -f "$APP_DIR/.env" ]; then
    echo -e "${YELLOW}Creating .env configuration...${NC}"
    cat > $APP_DIR/.env << EOF
PORT=$BACKEND_PORT
NODE_ENV=production
LOG_LEVEL=info

# Persona API credentials (add your keys here)
# PERSONA_API_KEY=your_persona_api_key
# PERSONA_TEMPLATE_ID=your_template_id
EOF
    echo -e "${GREEN}✓${NC} Created .env file at $APP_DIR/.env"
    echo -e "${YELLOW}⚠ Remember to add your Persona API credentials to $APP_DIR/.env${NC}"
fi

# Step 6: Configure Nginx
echo -e "${YELLOW}[6/8]${NC} Configuring Nginx reverse proxy..."

# Copy Nginx config
cat > /etc/nginx/sites-available/$DOMAIN << 'NGINX_CONFIG'
limit_req_zone $binary_remote_addr zone=openkyc_limit:10m rate=10r/s;

upstream openkyc_backend {
    server 127.0.0.1:8080;
    keepalive 64;
}

server {
    listen 80;
    listen [::]:80;
    server_name openkyc.dinero-coin.com;

    location /.well-known/acme-challenge/ {
        root /var/www/certbot;
    }

    location / {
        return 301 https://$server_name$request_uri;
    }
}

server {
    listen 443 ssl http2;
    listen [::]:443 ssl http2;
    server_name openkyc.dinero-coin.com;

    ssl_certificate /etc/letsencrypt/live/openkyc.dinero-coin.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/openkyc.dinero-coin.com/privkey.pem;
    ssl_trusted_certificate /etc/letsencrypt/live/openkyc.dinero-coin.com/chain.pem;

    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_ciphers 'ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384';
    ssl_prefer_server_ciphers off;
    ssl_session_cache shared:SSL:10m;
    ssl_session_timeout 10m;

    add_header Strict-Transport-Security "max-age=31536000; includeSubDomains" always;
    add_header X-Frame-Options "SAMEORIGIN" always;
    add_header X-Content-Type-Options "nosniff" always;
    add_header Access-Control-Allow-Origin "*" always;
    add_header Access-Control-Allow-Methods "GET, POST, OPTIONS" always;
    add_header Access-Control-Allow-Headers "Content-Type, Authorization" always;

    if ($request_method = 'OPTIONS') {
        return 204;
    }

    access_log /var/log/nginx/openkyc-access.log;
    error_log /var/log/nginx/openkyc-error.log warn;

    location /api/ {
        limit_req zone=openkyc_limit burst=20 nodelay;
        proxy_pass http://openkyc_backend;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Connection "";
        proxy_connect_timeout 30s;
        proxy_send_timeout 30s;
        proxy_read_timeout 30s;
        proxy_buffering off;
    }

    location /health {
        proxy_pass http://openkyc_backend;
        access_log off;
    }

    location = / {
        return 404;
    }
}
NGINX_CONFIG

# Enable site
ln -sf /etc/nginx/sites-available/$DOMAIN /etc/nginx/sites-enabled/

# Test Nginx config
nginx -t

# Step 7: Obtain SSL certificate
echo -e "${YELLOW}[7/8]${NC} Obtaining Let's Encrypt SSL certificate..."
if [ ! -d "/etc/letsencrypt/live/$DOMAIN" ]; then
    certbot certonly --nginx -d $DOMAIN --non-interactive --agree-tos -m $EMAIL
    echo -e "${GREEN}✓${NC} SSL certificate obtained"
else
    echo -e "${GREEN}✓${NC} SSL certificate already exists"
fi

# Reload Nginx
systemctl reload nginx

# Step 8: Start OpenKYC with PM2
echo -e "${YELLOW}[8/8]${NC} Starting OpenKYC backend with PM2..."
cd $APP_DIR

# Stop existing process if running
pm2 delete openkyc 2>/dev/null || true

# Start with PM2
pm2 start index.js --name openkyc --env production

# Save PM2 configuration
pm2 save

# Setup PM2 startup script
pm2 startup systemd -u root --hp /root

echo ""
echo -e "${GREEN}=========================================="
echo "OpenKYC Deployment Complete!"
echo "==========================================${NC}"
echo ""
echo "Service URLs:"
echo "  • HTTPS: https://$DOMAIN/api/verify/start"
echo "  • Health: https://$DOMAIN/health"
echo ""
echo "Management Commands:"
echo "  • View logs:    pm2 logs openkyc"
echo "  • Restart:      pm2 restart openkyc"
echo "  • Stop:         pm2 stop openkyc"
echo "  • Status:       pm2 status"
echo ""
echo -e "${YELLOW}Next Steps:${NC}"
echo "1. Add your Persona API credentials to: $APP_DIR/.env"
echo "2. Restart the service: pm2 restart openkyc"
echo "3. Test the endpoint: curl https://$DOMAIN/health"
echo ""
echo -e "${GREEN}✓ OpenKYC is now accessible at: https://$DOMAIN${NC}"

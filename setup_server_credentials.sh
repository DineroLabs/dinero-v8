#!/bin/bash
# Setup Server Credentials for Deployment
# Run this once to configure your server access

echo "================================"
echo "  Dinero Server Credentials"
echo "================================"
echo ""

# Prompt for credentials
read -p "Server 1 (e.g., user@192.168.1.10): " SERVER1
read -p "Server 2 (e.g., user@192.168.1.11): " SERVER2
read -p "SSH Key Path (e.g., ~/.ssh/id_rsa): " SSH_KEY

# Expand tilde
SSH_KEY="${SSH_KEY/#\~/$HOME}"

# Verify SSH key exists
if [ ! -f "$SSH_KEY" ]; then
    echo ""
    echo "ERROR: SSH key not found: $SSH_KEY"
    exit 1
fi

# Create config file
CONFIG_FILE="$HOME/.ssh/dinero_servers.conf"
cat > "$CONFIG_FILE" <<EOF
# Dinero Server Credentials
# Created: $(date)

SERVER1="$SERVER1"
SERVER2="$SERVER2"
SSH_KEY="$SSH_KEY"
EOF

chmod 600 "$CONFIG_FILE"

echo ""
echo "✅ Credentials saved to: $CONFIG_FILE"
echo ""
echo "Test connection:"
echo "  ssh -i $SSH_KEY $SERVER1 'echo Connected to Server 1'"
echo "  ssh -i $SSH_KEY $SERVER2 'echo Connected to Server 2'"
echo ""
echo "Ready to deploy:"
echo "  ./DEPLOY_TO_LINUX_SERVERS.sh"


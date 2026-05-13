# DineroCoin Storage Security Practices

## Overview

This document outlines security best practices for DineroCoin storage backends in production environments, covering at-rest encryption, TLS configuration, access controls, and operational security procedures.

## At-Rest Encryption

### Database Encryption

#### RocksDB Encryption
```bash
# Enable RocksDB encryption at rest
export DINERO_ROCKSDB_ENCRYPTION=true
export DINERO_ENCRYPTION_KEY_FILE=/secure/path/encryption.key

# Key rotation configuration
export DINERO_KEY_ROTATION_INTERVAL=2592000  # 30 days in seconds
export DINERO_OLD_KEY_RETENTION=7776000      # 90 days in seconds
```

#### LevelDB Encryption
```bash
# LevelDB with filesystem-level encryption
export DINERO_LEVELDB_ENCRYPTED_PATH=/encrypted/storage
export DINERO_FILESYSTEM_ENCRYPTION=luks2
```

### Key Management

#### Key Generation
```bash
# Generate encryption keys
openssl rand -hex 32 > /secure/path/encryption.key
chmod 600 /secure/path/encryption.key
chown dinero:dinero /secure/path/encryption.key

# Hardware Security Module (HSM) integration
export DINERO_HSM_ENABLED=true
export DINERO_HSM_SLOT=0
export DINERO_HSM_PIN_FILE=/secure/hsm.pin
```

#### Key Rotation
```bash
# Automated key rotation script
#!/bin/bash
CURRENT_KEY="/secure/path/encryption.key"
NEW_KEY="/secure/path/encryption.key.new"
BACKUP_KEY="/secure/path/encryption.key.backup"

# Generate new key
openssl rand -hex 32 > "$NEW_KEY"

# Backup current key
cp "$CURRENT_KEY" "$BACKUP_KEY"

# Rotate key (requires application restart)
systemctl stop dinerod
mv "$CURRENT_KEY" "$CURRENT_KEY.old"
mv "$NEW_KEY" "$CURRENT_KEY"
systemctl start dinerod
```

## TLS Configuration

### Node-to-Node Communication

#### TLS Certificate Setup
```bash
# Generate CA certificate
openssl genrsa -out ca-key.pem 4096
openssl req -new -x509 -days 365 -key ca-key.pem -sha256 -out ca.pem

# Generate node certificate
openssl genrsa -out node-key.pem 4096
openssl req -subj "/CN=dinero-node" -sha256 -new -key node-key.pem -out node.csr
openssl x509 -req -days 365 -sha256 -in node.csr -CA ca.pem -CAkey ca-key.pem -out node-cert.pem
```

#### Configuration
```json
{
  "tls": {
    "enabled": true,
    "cert_file": "/etc/dinero/tls/node-cert.pem",
    "key_file": "/etc/dinero/tls/node-key.pem",
    "ca_file": "/etc/dinero/tls/ca.pem",
    "min_version": "1.3",
    "cipher_suites": [
      "TLS_AES_256_GCM_SHA384",
      "TLS_CHACHA20_POLY1305_SHA256",
      "TLS_AES_128_GCM_SHA256"
    ],
    "verify_peer": true,
    "verify_hostname": true
  }
}
```

### RPC API Security

#### Client Authentication
```json
{
  "rpc": {
    "tls_enabled": true,
    "cert_file": "/etc/dinero/rpc/server-cert.pem",
    "key_file": "/etc/dinero/rpc/server-key.pem",
    "client_ca_file": "/etc/dinero/rpc/client-ca.pem",
    "require_client_cert": true,
    "allowed_clients": [
      "dinero-cli",
      "monitoring-system",
      "backup-service"
    ]
  }
}
```

## Access Controls

### File System Permissions

#### Storage Directory Security
```bash
# Create dedicated user and group
useradd -r -s /bin/false dinero
groupadd dinero

# Set storage directory permissions
chown -R dinero:dinero /var/lib/dinero
chmod 750 /var/lib/dinero
chmod 640 /var/lib/dinero/*.db

# SELinux context (if enabled)
semanage fcontext -a -t dinero_data_t "/var/lib/dinero(/.*)?"
restorecon -R /var/lib/dinero
```

#### Configuration File Security
```bash
# Secure configuration files
chmod 600 /etc/dinero/dinero.conf
chown dinero:dinero /etc/dinero/dinero.conf

# Secure key files
chmod 400 /secure/path/encryption.key
chown dinero:dinero /secure/path/encryption.key
```

### Network Security

#### Firewall Configuration
```bash
# iptables rules
iptables -A INPUT -p tcp --dport 8332 -s 10.0.0.0/8 -j ACCEPT  # RPC
iptables -A INPUT -p tcp --dport 8333 -j ACCEPT                # P2P
iptables -A INPUT -p tcp --dport 8334 -s 10.0.0.0/8 -j ACCEPT  # Monitoring

# UFW rules
ufw allow from 10.0.0.0/8 to any port 8332
ufw allow 8333
ufw allow from 10.0.0.0/8 to any port 8334
```

## Authentication and Authorization

### RPC Authentication

#### Cookie-based Authentication
```bash
# Generate secure cookie
openssl rand -hex 32 > /var/lib/dinero/.cookie
chmod 600 /var/lib/dinero/.cookie
chown dinero:dinero /var/lib/dinero/.cookie
```

#### User-based Authentication
```json
{
  "rpc_users": [
    {
      "username": "admin",
      "password_hash": "$2b$12$...",
      "permissions": ["read", "write", "admin"]
    },
    {
      "username": "monitor",
      "password_hash": "$2b$12$...",
      "permissions": ["read"]
    }
  ]
}
```

### API Rate Limiting
```json
{
  "rate_limiting": {
    "enabled": true,
    "requests_per_minute": 60,
    "burst_size": 10,
    "whitelist": [
      "127.0.0.1",
      "10.0.0.0/8"
    ]
  }
}
```

## Monitoring and Auditing

### Security Event Logging
```json
{
  "security_logging": {
    "enabled": true,
    "log_file": "/var/log/dinero/security.log",
    "log_level": "INFO",
    "events": [
      "authentication_failure",
      "authorization_failure",
      "tls_handshake_failure",
      "rate_limit_exceeded",
      "suspicious_activity"
    ]
  }
}
```

### Intrusion Detection
```bash
# Install and configure fail2ban
apt-get install fail2ban

# /etc/fail2ban/jail.local
[dinero-rpc]
enabled = true
port = 8332
filter = dinero-rpc
logpath = /var/log/dinero/security.log
maxretry = 3
bantime = 3600
```

## Backup Security

### Encrypted Backups
```bash
# GPG encryption for backups
gpg --gen-key  # Generate backup encryption key

# Backup with encryption
dinero-cli backupstorage /tmp/backup.tar
gpg --encrypt --recipient backup@dinero.com /tmp/backup.tar
rm /tmp/backup.tar
```

### Secure Backup Storage
```bash
# S3 with encryption
aws s3 cp backup.tar.gpg s3://dinero-backups/ \
  --server-side-encryption AES256 \
  --storage-class STANDARD_IA

# Azure with encryption
az storage blob upload \
  --file backup.tar.gpg \
  --container-name dinero-backups \
  --encryption-scope backup-scope
```

## Incident Response

### Security Incident Procedures

1. **Detection**: Monitor security logs and alerts
2. **Containment**: Isolate affected systems
3. **Investigation**: Analyze logs and forensic data
4. **Recovery**: Restore from secure backups
5. **Lessons Learned**: Update security procedures

### Emergency Procedures
```bash
# Emergency shutdown
systemctl stop dinerod
iptables -A INPUT -j DROP  # Block all incoming traffic

# Secure backup creation
dinero-cli emergencybackup /secure/emergency/
```

## Compliance and Standards

### Security Standards
- **NIST Cybersecurity Framework**: Implement identify, protect, detect, respond, recover
- **ISO 27001**: Information security management system
- **SOC 2 Type II**: Security, availability, processing integrity

### Audit Requirements
- Regular security assessments
- Penetration testing
- Vulnerability scanning
- Compliance reporting

## Security Checklist

### Pre-Production
- [ ] Enable at-rest encryption
- [ ] Configure TLS for all communications
- [ ] Set up proper file permissions
- [ ] Configure firewall rules
- [ ] Enable security logging
- [ ] Set up monitoring and alerting
- [ ] Create incident response plan

### Ongoing Operations
- [ ] Regular key rotation
- [ ] Security patch management
- [ ] Log review and analysis
- [ ] Backup testing and validation
- [ ] Security training for operators
- [ ] Regular security assessments

## References

- [NIST Cybersecurity Framework](https://www.nist.gov/cyberframework)
- [OWASP Security Guidelines](https://owasp.org/)
- [CIS Controls](https://www.cisecurity.org/controls/)
- [RocksDB Encryption Documentation](https://github.com/facebook/rocksdb/wiki/Encryption-at-Rest)

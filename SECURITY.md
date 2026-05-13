# Security Policy

## 🔒 Security Overview

Dinero takes security seriously. This document outlines our security practices, how to report vulnerabilities, and our response procedures.

## ⚠️ ALPHA RELEASE NOTICE - v3.0.0-alpha1

**This is a pre-release version and has NOT been externally audited.**

### Alpha Status Warning

- ❌ **NOT for production use**
- ❌ **NOT for mainnet deployment**
- ❌ **NO external security audit completed**
- ✅ **Testnet use only**
- ✅ **Feedback welcome**

### Security Limitations

**v3.0.0-alpha1 is released for:**
- Developer testing
- Testnet deployment
- Early feedback collection
- Pre-audit evaluation

**v3.0.0-alpha1 is NOT suitable for:**
- Production mainnet use
- Exchange integration
- Large fund storage
- Mission-critical applications

### Known Security Gaps

1. **No External Audit** - Code has not been reviewed by independent security researchers
2. **Lightning Incomplete** - Signature verification and payment routing not complete
   - **Architectural Note:** Lightning is implemented as a decoupled subsystem and is not part of the consensus-critical dinerod core. Incomplete Lightning features cannot affect block validation, chain safety, or on-chain wallet funds.
3. **Performance Not Optimized** - May have denial-of-service vulnerabilities under load
4. **Alpha Quality** - May contain undiscovered bugs or vulnerabilities

### Reporting Security Issues in Alpha

**Even for alpha releases, please report security issues privately.**

**Email:** security@dinero-coin.com

Include:
- Affected version (v3.0.0-alpha1)
- Description of the vulnerability
- Steps to reproduce
- Potential impact assessment

**Expected Response Time:**
- Initial acknowledgment: 48 hours
- Preliminary assessment: 1 week
- Fix timeline: Depends on severity (alpha releases prioritize critical issues only)

### Security Roadmap

- **v3.0.0-beta1** - External security audit scheduled
- **v3.0.0-rc1** - Audit complete, critical issues fixed
- **v3.0.0 (final)** - Production-ready, full security hardening

### For Stable Production Use

**Use v2.2.6 for production deployments.**

v2.2.6 is the latest stable release with:
- ✅ Extensive testing
- ✅ Production hardening
- ✅ Known security profile
- ✅ Mainnet ready

---

## ⚠️ Consensus Notice

**Versions prior to v1.0.4 contain a block header endianness bug that may prevent chain progression beyond block 1 during mining.**

All miners and node operators should upgrade to **v1.0.4** or later.

**Impact**: Nodes running versions prior to v1.0.4 may experience:
- Stale-prevhash block rejections
- Inability to mine beyond block 1
- High CPU usage (600%+) due to worker thread spin

**Fixed in**: v1.0.4 (2026-01-01)

## 🔧 CPU Stability Fixes

**All known CPU spinning issues have been eliminated as of v1.0.7.**

### Timeline of CPU Fixes

#### v1.0.4 (2026-01-01) - Critical CPU Fixes
Fixed two major CPU spinning issues:

1. **Validation Worker Livelock**
   - **Impact**: CPU usage spiked to 625% (8-core busy spin)
   - **Cause**: Worker threads spinning when validation queue was empty
   - **Fix**: Added proper queue blocking with condition variables
   - **Result**: CPU usage dropped to 0.2% when idle
   - **Location**: `src/consensus/validation_queue.cpp`

2. **Mining Worker Spin**
   - **Impact**: CPU usage at 146% even when mining disabled
   - **Cause**: Mining threads continuously polling for work without backoff
   - **Fix**: Added 100ms sleep when no mining work available
   - **Result**: CPU usage dropped to 4-11% when idle
   - **Location**: `src/daemon/services/mining_service.cpp`

#### v1.0.7 (2026-01-01) - Stratum Server Hardening
Fixed Stratum server tight loop on socket errors:

1. **Stratum Server Tight Loop**
   - **Impact**: Potential 625% CPU spike when `accept()` fails repeatedly
   - **Scenario**: File descriptor exhaustion, port conflicts, or socket errors
   - **Cause**: No backoff delay after `accept()` failure, causing immediate retry
   - **Fix**: Added 100ms sleep on `accept()` failure in all Stratum implementations
   - **Result**: CPU usage stays below 1% even under socket error conditions
   - **Locations**:
     - `src/stratum_bridge/stratum_server.cpp`
     - `src/stratum_bridge/stratum_server_unified.cpp`
     - `src/stratum_bridge/stratum_server_complete.cpp`

### Recommended Actions

- **Immediate**: Upgrade to v1.0.7 or later for complete CPU stability
- **Stratum Operators**: v1.0.7 is especially critical for pool operators running Stratum servers
- **Node Operators**: v1.0.4+ provides stable operation for full nodes and solo miners

### Verification

All long-running daemon loops now implement proper backoff strategies:
- ✅ Network listener: 100ms poll interval
- ✅ Peer maintenance: 30s maintenance interval
- ✅ Message processing: 10ms processing interval
- ✅ WebSocket server: `select()` with timeout (proper blocking I/O)
- ✅ Mining workers: 100ms sleep when no work + adaptive backoff
- ✅ Stratum servers: 100ms backoff on socket errors

**CPU usage when idle**: <1% (typical: 0.2-0.5%)

## 🔨 Build & ABI Stability

### v1.0.8.23 (2026-01-02) - C++17 ABI Compatibility Fix

Fixed C++ ABI incompatibility between DineroCoin (C++17) and bundled JsonCpp (C++11):

- **Impact**: Linker errors involving `std::string_view` symbols during consensus-critical test builds
- **Cause**: JsonCpp hardcoded C++11 standard, causing ABI mismatch with DineroCoin's C++17 code
- **Fix**: Forced JsonCpp targets to compile with C++17 using parent CMake overrides (no submodule modifications)
- **Additional**: Replaced C++20-only `starts_with()`/`ends_with()` with C++17-compatible logic
- **Verification**: Consensus-critical regression test passes on Ubuntu and macOS
- **Scope**: No consensus, runtime, or database behavior changes
- **Result**: ABI consistency across all JsonCpp usage, stable cross-platform builds

**Recommended**: Use v1.0.8.23 or later for stable builds from source.

## 🛡️ Security Features

### Production Security Hardening
- **TLS-Only Communication**: All RPC and WebSocket traffic encrypted with TLS 1.2+
- **Rate Limiting**: Built-in rate limiting (10 req/sec RPC, 5 req/sec WebSocket)
- **Input Validation**: Comprehensive validation of all inputs with sanitization
- **Constant-Time Operations**: Timing-safe authentication to prevent timing attacks
- **Non-Root Execution**: Daemon runs as unprivileged user with capability dropping
- **Read-Only Filesystem**: Container deployments use read-only root filesystem
- **Network Policies**: Kubernetes network policies restrict traffic flow

### System Hardening
- **Systemd Security**: 15+ security flags including `PrivateTmp`, `ProtectSystem=strict`
- **Capability Dropping**: Minimal required capabilities with `NoNewPrivileges`
- **Resource Limits**: Memory, CPU, and file descriptor limits enforced
- **Seccomp Profiles**: Syscall filtering to reduce attack surface
- **File Permissions**: Strict file permissions (cookie 0600, configs 0644)

### Data Protection
- **Database Hardening**: SQLite with `SQLITE_DBCONFIG_DEFENSIVE`, `FULLMUTEX`, integrity checks
- **WAL Recovery**: Startup `CHECKPOINT_FULL` for crash recovery, shutdown `CHECKPOINT_TRUNCATE`
- **Transaction Safety**: `BEGIN IMMEDIATE` prevents writer starvation, durability profiles (FAST|NORMAL|SAFE)
- **Secure Backups**: Encrypted backups with integrity verification
- **Key Management**: Secure handling of cryptographic keys and cookies
- **Audit Logging**: Complete audit trail of security-relevant events

## 🚨 Supported Versions

We provide security updates for the following versions:

| Version | Supported          | End of Support |
| ------- | ------------------ | -------------- |
| 1.0.x   | ✅ Full Support    | TBD            |
| 0.9.x   | ⚠️ Security Only   | 2025-07-XX     |
| < 0.9   | ❌ Not Supported   | Ended          |

## 🐛 Reporting a Vulnerability

### Responsible Disclosure

We follow a 90-day coordinated disclosure policy. Please report security vulnerabilities responsibly.

### How to Report

**🔐 For security vulnerabilities, please DO NOT open a public GitHub issue.**

Instead, please report security issues through one of these channels:

#### 1. GitHub Security Advisories (Preferred)
- Go to the [Security tab](https://github.com/dinero/dinero/security) in our repository
- Click "Report a vulnerability"
- Fill out the security advisory form

#### 2. Email
- Send details to: **security@dinero.example.com**
- Use PGP encryption if possible (key below)
- Include "SECURITY" in the subject line

#### 3. Encrypted Communication
```
-----BEGIN PGP PUBLIC KEY BLOCK-----
[PGP Key would be here in production]
-----END PGP PUBLIC KEY BLOCK-----
```

### What to Include

Please include the following information in your report:

- **Description**: Clear description of the vulnerability
- **Impact**: Potential impact and attack scenarios
- **Reproduction**: Step-by-step instructions to reproduce
- **Environment**: Version, OS, configuration details
- **Proof of Concept**: Code or commands demonstrating the issue
- **Suggested Fix**: If you have ideas for remediation

### Example Report Template

```
Subject: SECURITY - [Brief Description]

Vulnerability Details:
- Component: [e.g., RPC server, WebSocket handler]
- Type: [e.g., Buffer overflow, Authentication bypass]
- Severity: [Critical/High/Medium/Low]

Description:
[Detailed description of the vulnerability]

Impact:
[What an attacker could achieve]

Reproduction Steps:
1. [Step 1]
2. [Step 2]
3. [Step 3]

Environment:
- Dinero Version: [version]
- OS: [operating system]
- Configuration: [relevant config]

Proof of Concept:
[Code, commands, or screenshots]

Suggested Mitigation:
[Your ideas for fixing the issue]
```

## 📋 Response Process

### Timeline

| Phase | Timeline | Description |
|-------|----------|-------------|
| **Acknowledgment** | 24 hours | We confirm receipt of your report |
| **Initial Assessment** | 72 hours | We assess severity and impact |
| **Investigation** | 1-2 weeks | We investigate and develop fixes |
| **Fix Development** | 2-4 weeks | We develop and test the fix |
| **Disclosure** | 90 days max | Coordinated public disclosure |

### Severity Classification

We use the following severity levels:

#### 🔴 Critical (CVSS 9.0-10.0)
- Remote code execution
- Complete system compromise
- Cryptocurrency theft/loss
- **Response**: Immediate (24-48 hours)

#### 🟠 High (CVSS 7.0-8.9)
- Privilege escalation
- Authentication bypass
- Data corruption/loss
- **Response**: 1 week

#### 🟡 Medium (CVSS 4.0-6.9)
- Information disclosure
- Denial of service
- Limited privilege escalation
- **Response**: 2-4 weeks

#### 🟢 Low (CVSS 0.1-3.9)
- Minor information leaks
- Configuration issues
- **Response**: Next release cycle

### Our Commitments

- **Acknowledgment**: We will acknowledge your report within 24 hours
- **Communication**: We will keep you updated on our progress
- **Credit**: We will credit you in our security advisory (unless you prefer anonymity)
- **Coordination**: We will coordinate disclosure timing with you
- **No Legal Action**: We will not pursue legal action for good-faith security research

## 🏆 Security Researchers

We appreciate the security research community and welcome responsible disclosure of vulnerabilities.

### Hall of Fame

We maintain a list of security researchers who have helped improve Dinero's security:

- [Your name could be here!]

### Bug Bounty

While we don't currently offer a formal bug bounty program, we recognize valuable security contributions through:

- Public acknowledgment in release notes
- CVE credit where applicable
- Dinero project swag (when available)
- Potential future bounty program participation

## 🔍 Security Audits

### Internal Security Measures

- **Code Review**: All code changes require security-focused review
- **Static Analysis**: Automated security scanning in CI/CD pipeline
- **Dynamic Testing**: Fuzzing and chaos testing for robustness
- **Dependency Scanning**: Regular vulnerability scanning of dependencies
- **Penetration Testing**: Regular internal security assessments

### External Audits

We welcome and encourage external security audits:

- **Academic Research**: We support academic security research
- **Professional Audits**: We engage professional security firms
- **Community Review**: We encourage community security review

### Audit Reports

Completed security audit reports will be published here:

- [Future audit reports will be listed here]

## 📚 Security Best Practices

### For Operators

#### Deployment Security
- **Use TLS**: Always deploy with TLS termination (nginx configuration provided)
- **Network Isolation**: Use firewalls and network policies to restrict access
- **Regular Updates**: Keep Dinero and system packages updated
- **Monitoring**: Deploy comprehensive monitoring and alerting
- **Backups**: Implement secure, tested backup procedures

#### Configuration Security
- **Strong Authentication**: Use strong, unique passwords/keys
- **Minimal Permissions**: Run with minimal required permissions
- **Secure Storage**: Protect configuration files and keys
- **Log Monitoring**: Monitor logs for security events
- **Resource Limits**: Configure appropriate resource limits

#### Operational Security
- **Access Control**: Limit administrative access
- **Audit Logging**: Enable comprehensive audit logging
- **Incident Response**: Have incident response procedures ready
- **Regular Testing**: Test backup and recovery procedures
- **Security Training**: Ensure operators understand security procedures

### For Developers

#### Secure Development
- **Input Validation**: Validate all inputs thoroughly
- **Output Encoding**: Properly encode all outputs
- **Error Handling**: Handle errors securely without information leakage
- **Cryptography**: Use established cryptographic libraries
- **Dependencies**: Keep dependencies updated and scan for vulnerabilities

#### Code Review
- **Security Focus**: Review code with security in mind
- **Threat Modeling**: Consider potential attack vectors
- **Testing**: Include security testing in development process
- **Documentation**: Document security-relevant decisions

## 🚨 Security Incidents

### Current Status
- **No known critical vulnerabilities**
- **Last security update**: [Date of last security release]
- **Security contact**: security@dinero.example.com

### Past Incidents
- [Future security incidents will be documented here]

## 📞 Contact Information

- **Security Email**: security@dinero.example.com
- **General Contact**: contact@dinero.example.com
- **GitHub Security**: [Repository Security Tab](https://github.com/dinero/dinero/security)

## 🔄 Policy Updates

This security policy is reviewed and updated regularly. Last updated: [Current Date]

Changes to this policy will be announced through:
- GitHub repository notifications
- Security mailing list (if established)
- Release notes

---

**Thank you for helping keep Dinero secure! 🛡️**

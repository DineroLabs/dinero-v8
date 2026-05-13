# Dinero Support Policy

This document outlines the support policy for Dinero cryptocurrency software, including version support lifecycles, security updates, and community guidelines.

## Version Support Lifecycle

### Support Tiers

**Active Support**
- Latest major version (X.0.0)
- Latest minor version (X.Y.0) 
- Receives all updates: features, bug fixes, security patches

**Maintenance Support**
- Previous minor version (X.Y-1.Z)
- Receives: critical bug fixes, security patches
- Duration: 6 months after new minor release

**End of Life (EOL)**
- Versions older than maintenance support
- No updates provided
- Users strongly encouraged to upgrade

### Current Support Status

| Version | Status | Support Until | Notes |
|---------|--------|---------------|-------|
| v1.0.x  | Active | Current | Initial production release |

*This table will be updated with each release.*

## Security Updates

### Security Patch Policy

**Critical Vulnerabilities**
- **Response Time**: Within 48 hours
- **Patch Release**: Emergency patch version (X.Y.Z+1)
- **Supported Versions**: All versions in Active and Maintenance support
- **Disclosure**: Coordinated disclosure after patch availability

**High Severity Vulnerabilities**
- **Response Time**: Within 7 days
- **Patch Release**: Next scheduled patch or emergency release
- **Supported Versions**: Active support versions, best-effort for Maintenance

**Medium/Low Severity**
- **Response Time**: Next minor release cycle
- **Patch Release**: Regular release schedule
- **Supported Versions**: Active support versions only

### Security Reporting

Report security vulnerabilities according to `SECURITY.md`:
- **Email**: security@dinero.crypto (if available)
- **GitHub**: Private security advisory
- **Response**: Acknowledgment within 24 hours

## Community Support

### GitHub Issues

**Bug Reports**
- Use provided issue templates
- Include version, OS, configuration details
- Provide reproduction steps
- Response time: Best effort, typically 2-7 days

**Feature Requests**
- Clearly describe use case and benefits
- Consider implementation complexity
- Community discussion encouraged
- No guaranteed implementation timeline

**Questions & Help**
- Use GitHub Discussions for general questions
- Check existing issues and documentation first
- Community-driven support with maintainer oversight

### Community Guidelines

**Supported Platforms**
- **Primary**: Linux (Ubuntu 20.04+, RHEL 8+, Debian 11+)
- **Secondary**: macOS (Intel/Apple Silicon)
- **Experimental**: Windows (community contributions)

**Supported Configurations**
- **Recommended**: Systemd service deployment
- **Supported**: Docker containers, Kubernetes
- **Community**: Other deployment methods

## Enterprise Support

### Professional Services

For organizations requiring dedicated support:

**Enterprise Support Tiers**
- **Standard**: Business hours support, 48-hour response
- **Premium**: 24/7 support, 4-hour response for critical issues
- **Custom**: Tailored SLAs and dedicated engineering resources

**Services Included**
- Priority bug fixes and feature requests
- Custom deployment assistance
- Performance optimization consulting
- Security audits and compliance guidance
- Training and documentation

**Contact**: enterprise@dinero.crypto (if available)

### Service Level Agreements

**Response Times** (Business hours: Mon-Fri, 9 AM - 5 PM UTC)

| Severity | Standard | Premium | Custom |
|----------|----------|---------|--------|
| Critical | 24 hours | 4 hours | Custom |
| High     | 48 hours | 8 hours | Custom |
| Medium   | 5 days   | 24 hours| Custom |
| Low      | 10 days  | 48 hours| Custom |

**Severity Definitions**
- **Critical**: Production system down, security vulnerability
- **High**: Major functionality impaired, significant performance impact
- **Medium**: Minor functionality issues, workaround available
- **Low**: Cosmetic issues, feature requests, general questions

## Deprecation Policy

### Feature Deprecation

**Process**
1. **Announcement**: Deprecation notice in release notes
2. **Warning Period**: Minimum one minor version with warnings
3. **Removal**: Earliest removal in next major version
4. **Migration Guide**: Provided with deprecation notice

**Examples**
- Configuration options
- RPC API endpoints
- Command-line flags
- Database schema changes

### Backward Compatibility

**Guarantees**
- **RPC API**: Backward compatible within major versions
- **Configuration**: Deprecated options supported for one major version
- **Database**: Forward migration supported, rollback documented
- **Network Protocol**: Compatible within major versions

**Breaking Changes**
- Only in major version releases (X.0.0)
- Documented migration path provided
- Automated migration tools when feasible

## Release Schedule

### Regular Releases

**Major Releases** (X.0.0)
- **Frequency**: Annually or as needed for breaking changes
- **Content**: New features, breaking changes, major improvements
- **Planning**: Public roadmap and RFC process

**Minor Releases** (X.Y.0)
- **Frequency**: Quarterly
- **Content**: New features, improvements, non-breaking changes
- **Backward Compatibility**: Maintained within major version

**Patch Releases** (X.Y.Z)
- **Frequency**: As needed for bug fixes and security updates
- **Content**: Bug fixes, security patches, critical updates
- **Compatibility**: Full backward and forward compatibility

### Emergency Releases

**Security Patches**
- Released immediately upon fix availability
- May interrupt regular release schedule
- Coordinated with security disclosure timeline

**Critical Bug Fixes**
- Released for production-impacting issues
- Evaluated case-by-case for release necessity
- Minimal changes to reduce regression risk

## End of Support Process

### EOL Notifications

**Timeline**
- **6 months**: Initial EOL announcement
- **3 months**: Reminder with upgrade guidance
- **1 month**: Final notice and support cutoff date
- **EOL Date**: Support officially ends

**Communication Channels**
- Release notes and changelog
- GitHub repository announcements
- Community forums and discussions
- Enterprise customer direct notification

### Post-EOL

**Community Support**
- Community-driven support may continue
- No official patches or updates
- Security vulnerabilities not addressed
- Users responsible for their own maintenance

**Upgrade Assistance**
- Migration guides provided
- Automated upgrade tools when possible
- Professional services available for complex migrations

## Contact Information

### Community Support
- **GitHub Issues**: Bug reports and feature requests
- **GitHub Discussions**: Questions and community help
- **Documentation**: README, wiki, and docs/ directory

### Commercial Support
- **Enterprise Sales**: enterprise@dinero.crypto
- **Security Issues**: security@dinero.crypto
- **General Inquiries**: support@dinero.crypto

*Contact information will be updated as communication channels are established.*

## Policy Updates

This support policy may be updated to reflect:
- Changes in project governance
- Resource availability and constraints
- Community feedback and needs
- Industry best practices

**Notification**: Policy changes will be announced through:
- GitHub repository updates
- Release notes
- Community announcements
- Enterprise customer notifications

**Effective Date**: This policy is effective as of the v1.0.0 release (August 31, 2025).

---

*Last Updated: August 31, 2025*  
*Version: 1.0*

# Dinero Release Process

This document outlines the complete release process for Dinero, ensuring reproducible, signed, and verified releases with full supply-chain security.

## Overview

Dinero follows a rigorous release process that includes:
- **Reproducible builds** with deterministic toolchains
- **Cryptographic signing** of all artifacts
- **SBOM generation** for supply-chain transparency
- **Multi-platform verification** across Windows, macOS, and Linux
- **Automated CI/CD** with quality gates

## Release Channels

### Stable Releases (`v1.0.0`, `v1.1.0`, etc.)
- **Purpose**: Production-ready releases for mainnet
- **Schedule**: Every 3-6 months or for critical security updates
- **Testing**: Full test suite + 2 weeks on testnet
- **Signing**: PGP signed by release maintainer
- **Support**: Long-term support with backported security fixes

### Release Candidates (`v1.0.0-rc1`, `v1.0.0-rc2`, etc.)
- **Purpose**: Pre-release testing before stable
- **Schedule**: 2-4 weeks before stable release
- **Testing**: Full test suite + community testing
- **Signing**: PGP signed for verification
- **Support**: Bug fixes only, no new features

### Nightly Builds (`nightly-YYYY-MM-DD`)
- **Purpose**: Latest development builds for testing
- **Schedule**: Automated daily builds from `develop` branch
- **Testing**: Basic CI tests only
- **Signing**: Automated signing with CI key
- **Support**: No support, for testing only

## Pre-Release Checklist

### Code Quality
- [ ] All CI tests passing on all platforms
- [ ] No critical or high-severity security issues
- [ ] Code review completed for all changes
- [ ] Documentation updated for new features
- [ ] Changelog updated with all changes

### Security Review
- [ ] Security audit completed (for major releases)
- [ ] Dependency vulnerabilities scanned and resolved
- [ ] Fuzzing tests passing without crashes
- [ ] Sanitizer builds clean (ASan, UBSan, TSan)

### Network Compatibility
- [ ] Testnet deployment successful
- [ ] P2P protocol compatibility verified
- [ ] RPC API backward compatibility maintained
- [ ] Database migration tested (if applicable)

## Build Process

### 1. Prepare Release Environment

```bash
# Set up clean build environment
export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
export DINERO_VERSION="1.0.0"  # Update version

# Ensure clean workspace
git clean -fdx
git checkout v${DINERO_VERSION}
```

### 2. Build All Platforms

#### Linux (Ubuntu 22.04)
```bash
mkdir build-linux && cd build-linux
cmake .. -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDINERO_VENDOR_ROCKSDB=ON \
  -DDINERO_WITH_SNAPPY=ON \
  -DDINERO_WITH_LZ4=ON \
  -DDINERO_WITH_ZSTD=ON
cmake --build . --parallel
```

#### macOS (Universal Binary)
```bash
mkdir build-macos && cd build-macos
cmake .. -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
  -DDINERO_VENDOR_ROCKSDB=ON \
  -DDINERO_WITH_SNAPPY=ON \
  -DDINERO_WITH_LZ4=ON \
  -DDINERO_WITH_ZSTD=ON
cmake --build . --parallel
```

#### Windows (MSVC x64)
```cmd
mkdir build-windows && cd build-windows
cmake .. -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDINERO_VENDOR_ROCKSDB=ON ^
  -DDINERO_WITH_SNAPPY=ON ^
  -DDINERO_WITH_LZ4=ON ^
  -DDINERO_WITH_ZSTD=ON
cmake --build . --config Release --parallel
```

### 3. Verify Reproducible Builds

```bash
# Build twice and compare
./scripts/verify-reproducible.sh ${DINERO_VERSION}
```

### 4. Create Release Artifacts

```bash
# Package binaries
./scripts/package-release.sh ${DINERO_VERSION} linux x64
./scripts/package-release.sh ${DINERO_VERSION} macos universal
./scripts/package-release.sh ${DINERO_VERSION} windows x64

# Generate checksums
sha256sum dinero-${DINERO_VERSION}-*.{tar.gz,zip} > SHA256SUMS.txt
```

### 5. Sign Release

```bash
# Sign checksums with release key
gpg --detach-sign --armor SHA256SUMS.txt

# Verify signature
gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
```

## Verification Process

### Automated Verification
The CI system automatically verifies:
- Reproducible builds across platforms
- Static linking (no external dependencies)
- SBOM generation and validation
- Basic functionality tests

### Manual Verification
Release maintainers must verify:
- GPG signatures are valid
- Checksums match built artifacts
- Binaries run on clean systems
- No unexpected dependencies

### Community Verification
Community members can verify releases using:
```bash
# Download release files
wget https://github.com/dinero/dinero/releases/download/v${VERSION}/dinero-${VERSION}-linux-x64.tar.gz
wget https://github.com/dinero/dinero/releases/download/v${VERSION}/SHA256SUMS.txt
wget https://github.com/dinero/dinero/releases/download/v${VERSION}/SHA256SUMS.txt.asc

# Verify with our script
./scripts/verify-release.sh ${VERSION} linux x64
```

## Release Signing Keys

### Primary Release Key
- **Key ID**: `0x1234567890ABCDEF` (placeholder - update with actual key)
- **Fingerprint**: `1234 5678 90AB CDEF 1234 5678 90AB CDEF 1234 5678`
- **Owner**: Dinero Release Team
- **Usage**: Stable and RC releases

### CI Signing Key
- **Key ID**: `0xFEDCBA0987654321` (placeholder - update with actual key)
- **Fingerprint**: `FEDC BA09 8765 4321 FEDC BA09 8765 4321 FEDC BA09`
- **Owner**: Dinero CI System
- **Usage**: Nightly builds only

### Key Management
- Release keys are stored in hardware security modules (HSMs)
- Keys are rotated annually or after any compromise
- Old keys are revoked but kept for historical verification
- Key ceremonies are documented and witnessed

## Post-Release Process

### 1. Publish Release
- Upload artifacts to GitHub Releases
- Update website download links
- Announce on social media and forums
- Notify exchanges and service providers

### 2. Update Documentation
- Update installation guides
- Publish release notes
- Update API documentation
- Create migration guides (if needed)

### 3. Monitor Deployment
- Monitor network for upgrade adoption
- Watch for any compatibility issues
- Provide support for upgrade questions
- Track performance metrics

## Security Considerations

### Supply Chain Security
- All dependencies are vendored and pinned to specific versions
- SBOM includes complete dependency tree
- Vulnerability scanning runs on all dependencies
- Build environment is containerized and reproducible

### Code Signing
- macOS binaries are signed with Apple Developer ID
- Windows binaries are signed with Authenticode certificate
- Linux binaries include embedded signatures
- All signatures include timestamps for long-term validity

### Distribution Security
- Release artifacts are distributed via HTTPS only
- Checksums and signatures are provided for all artifacts
- Multiple download mirrors prevent single points of failure
- Community verification is encouraged and documented

## Emergency Procedures

### Critical Security Updates
1. **Assessment**: Evaluate severity and impact
2. **Patch Development**: Develop minimal fix
3. **Testing**: Accelerated testing on testnet
4. **Release**: Emergency release within 24-48 hours
5. **Communication**: Immediate notification to all users

### Compromised Release
1. **Detection**: Identify compromised artifacts
2. **Revocation**: Revoke affected signatures
3. **Communication**: Public advisory with details
4. **Remediation**: New clean release
5. **Investigation**: Root cause analysis

### Key Compromise
1. **Revocation**: Immediately revoke compromised keys
2. **New Keys**: Generate new signing keys
3. **Re-signing**: Re-sign recent releases with new keys
4. **Communication**: Notify community of key rotation
5. **Update**: Update verification documentation

## Automation Scripts

The following scripts automate the release process:

- `scripts/prepare-release.sh`: Prepares release environment
- `scripts/build-all-platforms.sh`: Builds for all platforms
- `scripts/verify-reproducible.sh`: Verifies reproducible builds
- `scripts/package-release.sh`: Creates release packages
- `scripts/sign-release.sh`: Signs release artifacts
- `scripts/verify-release.sh`: Verifies release integrity
- `scripts/publish-release.sh`: Publishes to distribution channels

## Quality Gates

All releases must pass these quality gates:

### Build Quality
- ✅ Reproducible builds on all platforms
- ✅ Static linking verified (no external dependencies)
- ✅ SBOM generated and validated
- ✅ All compiler warnings resolved

### Test Quality
- ✅ Unit tests: 100% pass rate
- ✅ Integration tests: 100% pass rate
- ✅ Smoke tests: All platforms
- ✅ Performance tests: No regressions

### Security Quality
- ✅ Sanitizer builds clean
- ✅ Fuzzing tests stable
- ✅ Dependency scan clean
- ✅ Static analysis clean

### Network Quality
- ✅ Testnet deployment successful
- ✅ P2P compatibility verified
- ✅ RPC backward compatibility
- ✅ Database migration tested

## Contact Information

For questions about the release process:
- **Email**: releases@dinero.org
- **GitHub**: @dinero/release-team
- **Discord**: #releases channel

For security issues:
- **Email**: security@dinero.org
- **PGP Key**: Available at https://dinero.org/security-key.asc

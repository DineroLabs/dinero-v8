# Security Policy

The canonical Dinero security policy lives at:

**<https://dinerolabs.org/security/>**

That page is the source of truth for reporting procedure, scope, response
SLAs, the current signing chain, and known signing gaps. This file is a
short pointer for GitHub's Security tab.

## How to report

- **Sensitive issues:** email <security@dinerolabs.org>
- **GitHub-native private reporting:** [Report a vulnerability](https://github.com/DineroLabs/dinero-v8/security/advisories/new)
- **Non-sensitive bugs:** open a regular [GitHub issue](https://github.com/DineroLabs/dinero-v8/issues) instead

Dinero Labs does not currently publish a PGP key for encrypted email
disclosure. If your report requires encryption, say so in your first
message and we will coordinate a key exchange.

## Response timeline

| Phase | Commitment |
|---|---|
| Acknowledge receipt | Within 7 days |
| Triage critical issues | Within 14 days |
| Fix critical issues | No fixed deadline; reporter kept informed |
| Coordinated public disclosure | Typically 60–90 days after fix lands |

## Supported versions

| Version | Supported |
|---|---|
| v8.x | ✅ Active |
| v7.x | ❌ Superseded by the v7→v8 chain restart |
| v5.x and earlier | ❌ Historical |

Only v8 receives active security maintenance. Reports against earlier
versions will be evaluated for relevance to v8, but back-porting fixes
to v7 or earlier is not planned.

## Scope summary

In scope: consensus bugs, P2P denial-of-service or partition, wallet
key exposure, RPC authentication bypass, install-pipeline tampering
(including <https://dinerolabs.org/install.sh>), macOS notarization
chain compromise, cryptographic implementation flaws.

Out of scope: lost-password support, raw connection flooding, bugs in
third-party services (exchanges, third-party explorers, wrapped-DIN
bridges), pre-v8 versions.

Full scope, what's signed today, known gaps, and verification
instructions: <https://dinerolabs.org/security/>

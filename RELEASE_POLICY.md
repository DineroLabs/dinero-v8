# Release Policy

## Branch Roles
- `main`: Integration branch (moves freely, CI + hermetic builds required)
- `stable`: Release branch (protected, tag-only)

## Rules
- Commits may be merged into `main` without tags
- No force-pushes to `stable`
- Every commit on `stable` MUST be tagged
- Tags represent releases (vX.Y.Z)

## Release Flow
1. Merge fixes/features into `main`
2. When ready to release:
   - Merge `main` → `stable`
   - Create annotated tag
   - Push `stable` + tags

## What is a Release?
- Anything worth shipping to users
- Build-only fixes are OK but should be bundled when possible

## Critical Release (immediate)
- Consensus bugs
- Privacy vulnerabilities
- Security issues

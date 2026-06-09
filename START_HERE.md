# Start Here

Most users should not start from the source tree.

## I Want To Download Dinero

Use the current release candidate:

- Downloads: https://github.com/DineroLabs/dinero-v8/releases
- Website: https://dinerolabs.org

The release page contains the signed desktop wallet, operator packages, and
checksums. Verify release assets before running them.

## I Want To Run A Node

Use the current v8 release notes and installer from:

- https://dinerolabs.org
- https://github.com/DineroLabs/dinero-v8/releases

Operators should treat this repository as source code. Build from source only
when you intentionally want a development checkout.

## I Am Looking At The Source

This repository is a v8 monorepo. The important source areas are:

- `src/` - daemon, consensus, RPC, wallet, P2P, and NodeCore source
- `qt/` - desktop wallet source
- `cmake/`, `depends/`, `packaging/`, `debian/` - build and packaging support
- `tests/`, `fuzz/` - regression, integration, and fuzz tests
- `docs/` - design notes, release notes, archived material, and operator docs

Older public-snapshot artifacts and historical audit/tracking material live
under `docs/archive/public-snapshot/` so the repository root stays readable.

## Current Public Surfaces

- Canonical source: https://github.com/DineroLabs/dinero-v8
- Current downloads: https://github.com/DineroLabs/dinero-v8/releases
- Website: https://dinerolabs.org
- Explorer: https://explorer.realmoneyforfreepeople.org/
- Wiki source: [docs/wiki/](docs/wiki/)

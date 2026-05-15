# Dinero Depends

This directory is the start of Dinero's Bitcoin-style reproducible dependency
builds.

The first pinned dependencies here are used for optional P2P port mapping:

- `miniupnpc` for UPnP IGD port mapping
- `libnatpmp` for NAT-PMP port mapping

Build them with:

```sh
make -C depends
```

That installs static libraries and headers into `depends/<host>/` and writes
`depends/current-prefix.env`. CMake also auto-detects the `depends/current`
prefix when the symlink can be created.

Release builders can force the prefix explicitly:

```sh
export DINERO_DEPENDS_PREFIX="$(make -C depends --no-print-directory print-prefix)"
cmake -S . -B build-release -DDINERO_ENABLE_PORTMAPPING=ON
```

Developer builds may still discover host packages as a fallback, but release
builds should consume these libraries from `depends/` instead of Homebrew, apt,
Chocolatey, or other host package managers.

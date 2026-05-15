# Dinero Depends

This directory is the start of Dinero's Bitcoin-style reproducible dependency
builds.

The first pinned dependencies here are used for optional P2P port mapping:

- `miniupnpc` for UPnP IGD port mapping
- `libnatpmp` for NAT-PMP port mapping

Build them with CMake:

```sh
cmake -S depends -B depends/build -G Ninja
cmake --build depends/build
```

For local development, omit `-G Ninja` if Ninja is not installed. Release
builders should install and use Ninja, especially for native MSVC Windows.

That installs static libraries and headers into `depends/<host>/` and writes
`depends/current-prefix.env`. CMake also auto-detects the `depends/current`
prefix when the symlink can be created.

Release builders can force the prefix explicitly:

```sh
export DINERO_DEPENDS_PREFIX="$(sed -n 's/^DINERO_DEPENDS_PREFIX=//p' depends/current-prefix.env)"
cmake -S . -B build-release -DDINERO_ENABLE_PORTMAPPING=ON
```

Developer builds may still discover host packages as a fallback, but release
builds should consume these libraries from `depends/` instead of Homebrew, apt,
Chocolatey, or other host package managers.

Platform-specific dependency builds should use the toolchain files under
`depends/toolchains/`, with native MSVC Windows using Ninja from a Visual Studio
Developer shell.

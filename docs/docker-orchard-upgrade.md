# Docker and the scheduled Orchard profile

A completely empty Docker datadir now receives a private `dinero.conf` with
`lightning.oracles.enable=0`, matching the packaged fresh-node example. This
avoids enabling the incompatible legacy Lightning IPC clients on a new node.
The entrypoint does not create this default when any data, a configuration,
a custom config argument, or an explicit Lightning argument is present.
Unreadable volumes are refused rather than treated as empty.

Existing volumes keep their configuration and wallet/chain data. Before upgrading,
operators must check whether they use legacy Lightning IPC consumers. A scheduled
Orchard node refuses those consumers because they have no typed adapter. When no
such consumers are configured, the operator can explicitly set
`lightning.oracles.enable=0` in their existing `dinero.conf`, or supply
`--lightning.oracles.enable=0` to the container. Keep a protected configuration and
wallet backup. If legacy consumers are in use, qualify their migration or adapter
before continuing; the container must not silently turn them off.

This changes no snapshot selection or chain/wallet reset policy. Both the primary
snapshot and the older exact-base fallback remain armed across interrupted first
sync and ordinary restarts. User arguments are not printed to container logs.

`tests/integration/test_docker_entrypoint_config.py` runs the real shell entrypoint
and checks fresh initialization, restart, existing data/configuration, explicit
arguments, unreadable directory refusal and argument redaction. Native daemon
startup and actual snapshot import remain separate runtime checks.

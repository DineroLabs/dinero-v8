# Fleet Watcher

External fleet observability. Polls each node's loopback RPC on a cycle, records
every observation, and pages only for genuinely dangerous conditions.

Design: `docs/superpowers/specs/2026-08-06-fleet-watcher-design.md`

## Install

```bash
install -d -m 0750 -o fleet-watcher -g fleet-watcher /opt/fleet-watcher /var/lib/fleet-watcher
install -m 0644 tools/fleet-watcher/*.py /opt/fleet-watcher/
install -d -m 0700 /etc/fleet-watcher
install -m 0600 tools/fleet-watcher/deploy/config.example.json /etc/fleet-watcher/config.json
```

Write the two credential files, both `0600` and root-owned:

```
/etc/fleet-watcher/pushover.env     PUSHOVER_TOKEN=... / PUSHOVER_USER=...
/etc/fleet-watcher/heartbeat.env    HEARTBEAT_URL=...
```

Neither belongs in Git. Then:

```bash
cp tools/fleet-watcher/deploy/fleet-watcher.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now fleet-watcher
```

## Node-side access

Each polled node needs an unprivileged account restricted to a forced command:

```
command="/usr/local/bin/fleet-watcher-rpc",no-port-forwarding,no-agent-forwarding,no-X11-forwarding,no-pty ssh-ed25519 AAAA...
```

The wrapper accepts only the read RPCs this tool uses and `invocation-id`. It
must never provide a shell.

**The quoting contract is load-bearing.** The watcher invokes
`rpc <shlex.quoted-json>`, so the wrapper MUST word-split `$SSH_ORIGINAL_COMMAND`
through the shell and read the payload as `$2`:

```sh
#!/bin/sh
# /usr/local/bin/fleet-watcher-rpc — forced command, no shell for the caller.
set -eu
set -- $SSH_ORIGINAL_COMMAND          # deliberate word-splitting: the payload
                                      # arrives shell-quoted from the watcher
case "${1:-}" in
  rpc)
    printf '%s' "$2" | curl -sS --max-time 10 -X POST \
        -H 'Content-Type: application/json' --data-binary @- \
        "http://127.0.0.1:${DINERO_RPC_PORT:?}/"
    ;;
  invocation-id)
    systemctl show "${DINERO_UNIT:-dinerod}" -p InvocationID --value
    ;;
  *)
    echo "refused: ${1:-<empty>}" >&2; exit 64
    ;;
esac
```

A wrapper that instead does `${SSH_ORIGINAL_COMMAND#rpc }` receives the single
quotes literally and every RPC fails to parse — so this must be verified once
at install time, not assumed. Verify with:

```bash
ssh -o BatchMode=yes watcher@<node> "rpc '{\"jsonrpc\":\"2.0\",\"id\":\"t\",\"method\":\"getdaemonstatus\",\"params\":[]}'"
```

Expected: a JSON reply containing `"height"`. Anything else means the quoting
contract is broken and the watcher will report every node unreachable.

## Tests

```bash
cd tools/fleet-watcher && python3 -m unittest discover -s tests -v
```

## Verifying the dead-man switch

**Do this at install time and after any change to the heartbeat path.** An
untested dead-man is indistinguishable from a dead one, and it is the failure
mode that hides all the others.

```bash
systemctl stop fleet-watcher
# wait ~6 minutes
```

Confirm an alert arrives from the dead-man service. Then:

```bash
systemctl start fleet-watcher
```

Confirm the check returns to healthy. If no alert arrived, the heartbeat is not
protecting you and must be fixed before relying on this tool.

## Maintenance windows

Set `WATCHER_MAINTENANCE=1` in the service environment to suppress **delivery**
of `node_behind`, `majority_unreachable` and `telemetry_degraded`.

It can never suppress `safe_mode`, `tip_divergence` or `observer_divergence`.
Incidents are still recorded normally either way.

## Reading the data

```bash
sqlite3 /var/lib/fleet-watcher/watcher.db \
  "SELECT opened_at, rule, nodes, closed_at FROM incidents ORDER BY opened_at DESC LIMIT 20;"
```

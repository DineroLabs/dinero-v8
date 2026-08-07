# Fleet Watcher

External fleet observability. Polls each node's loopback RPC on a cycle, records
every observation, and pages only for genuinely dangerous conditions.

Design: `docs/superpowers/specs/2026-08-06-fleet-watcher-design.md`

## Install

**1. Create the service account.** Its home must be `/var/lib/fleet-watcher`,
not under `/home` — the unit sets `ProtectHome=true`, which blanks `/home`, and
SSH needs a readable home for its identity and `known_hosts`.

```bash
useradd --system --home-dir /var/lib/fleet-watcher --create-home \
        --shell /usr/sbin/nologin fleet-watcher
```

**2. Install the code and directories.**

```bash
install -d -m 0750 -o fleet-watcher -g fleet-watcher /opt/fleet-watcher /var/lib/fleet-watcher
install -m 0644 tools/fleet-watcher/*.py /opt/fleet-watcher/
install -d -m 0700 /etc/fleet-watcher
install -m 0600 tools/fleet-watcher/deploy/config.example.json /etc/fleet-watcher/config.json
```

**3. EDIT the config.** The example ships placeholder targets. Starting the
service against them polls hosts that do not exist and pages
`majority_unreachable` on a perfectly healthy fleet.

```bash
$EDITOR /etc/fleet-watcher/config.json      # real names, roles, transports, targets
python3 -c "import sys; sys.path.insert(0,'/opt/fleet-watcher'); \
            import config; print(config.load_config('/etc/fleet-watcher/config.json'))"
```

The second command is the validator: it rejects duplicate names, unknown
transports, a missing role and a config with no voting nodes. Do not proceed
until it prints a `Config(...)`.

**4. Provision SSH.** As the service account, generate a key, install its public
half in each node's `authorized_keys` behind the forced command, and populate
`known_hosts` — `StrictHostKeyChecking=yes` means an unknown host is a failed
poll, not a prompt.

```bash
sudo -u fleet-watcher ssh-keygen -t ed25519 -N '' -f /var/lib/fleet-watcher/.ssh/id_ed25519
sudo -u fleet-watcher ssh-keyscan -H <node> >> /var/lib/fleet-watcher/.ssh/known_hosts
```

**5. Write the two credential files**, both `0600` and root-owned. These are
systemd credentials, not environment files:

```
/etc/fleet-watcher/pushover.creds    PUSHOVER_TOKEN=... and PUSHOVER_USER=...
/etc/fleet-watcher/heartbeat.creds   HEARTBEAT_URL=...
```

Neither belongs in Git.

**6. Start it.**

```bash
cp tools/fleet-watcher/deploy/fleet-watcher.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now fleet-watcher
journalctl -u fleet-watcher -f          # confirm cycles are running
```

## Node-side access

Each polled node needs an unprivileged account restricted to a forced command:

```
command="/usr/local/bin/fleet-watcher-rpc",no-port-forwarding,no-agent-forwarding,no-X11-forwarding,no-pty ssh-ed25519 AAAA...
```

The wrapper accepts only the read RPCs this tool uses and `invocation-id`. It
must never provide a shell.

**The payload arrives on stdin, so there is nothing to quote.** An earlier
design passed it as an argument, which forced the wrapper to recover it from
`$SSH_ORIGINAL_COMMAND` — and every way of doing that is wrong in a different
way. `set -- $SSH_ORIGINAL_COMMAND` word-splits without quote removal, so the
payload arrives truncated with literal quotes; `${SSH_ORIGINAL_COMMAND#rpc }`
keeps the quotes; `eval` fixes both by executing text the caller influenced.
All three present identically: every node unreachable, which reads as a
fleet-wide outage rather than a one-line shell mistake.

```sh
#!/bin/sh
# /usr/local/bin/fleet-watcher-rpc — forced command; the caller never gets a shell.
set -eu
case "${SSH_ORIGINAL_COMMAND:-}" in
  rpc)
    # JSON-RPC body on stdin, straight through to the node's loopback RPC.
    curl -sS --max-time 10 -X POST \
        -H 'Content-Type: application/json' --data-binary @- \
        "http://127.0.0.1:${DINERO_RPC_PORT:?}/"
    ;;
  invocation-id)
    systemctl show "${DINERO_UNIT:-dinerod}" -p InvocationID --value
    ;;
  *)
    echo "refused: ${SSH_ORIGINAL_COMMAND:-<empty>}" >&2
    exit 64
    ;;
esac
```

`SSH_ORIGINAL_COMMAND` is now compared whole against two fixed strings. There is
no parsing to get wrong.

Verify the contract at install time, from the watcher host, as the watcher's own
account — this exercises the key, the host-key policy and the wrapper together:

```bash
printf '%s' '{"jsonrpc": "2.0", "id": "t", "method": "getdaemonstatus", "params": []}' \
  | sudo -u fleet-watcher ssh -o BatchMode=yes -o StrictHostKeyChecking=yes \
        watcher@<node> rpc
```

Expected: a JSON reply containing `"height"`. Note the spaces in the payload —
they match what `json.dumps` emits, so this exercises the real traffic shape
rather than a compact hand-typed one.

## Tests

```bash
cd tools/fleet-watcher && python3 -m unittest discover -s tests -v
```

## Verifying the dead-man switch

**Do this at install time and after any change to the heartbeat path.** An
untested dead-man is indistinguishable from a dead one, and it is the failure
mode that hides all the others.

First, set the grace period at the dead-man provider — the watcher pings every
60s, so a period of about 5 minutes catches a real stop without firing on a slow
cycle. **The provider's default may be an hour**; if you leave it there, the
test below will appear to fail when it is only slow.

Then confirm delivery works while everything is healthy:

```bash
sudo -u fleet-watcher HEARTBEAT_URL="$(sudo sed -n 's/^HEARTBEAT_URL=//p' \
    /etc/fleet-watcher/heartbeat.creds)" \
    curl -sS -o /dev/null -w '%{http_code}\n' "$HEARTBEAT_URL"
```

Expected: `200`. Now the actual test:

```bash
systemctl stop fleet-watcher
# wait for the grace period you configured, plus a minute
```

**Success looks like:** an alert arrives on the channel you configured at the
dead-man provider — which must be a DIFFERENT path from Pushover, so a single
provider outage cannot silence both. Then:

```bash
systemctl start fleet-watcher
```

**Success looks like:** the provider's check returns to healthy within two
minutes.

**If no alert arrived**, work through these in order before relying on the tool:

1. `journalctl -u fleet-watcher | grep -i heartbeat` — is the watcher pinging?
2. Is the grace period still at the provider default?
3. Is the notification channel on the provider actually configured and verified?
4. Is `HEARTBEAT_URL` in the credential file the check URL, not the dashboard URL?

A heartbeat that is not verified is not protecting you.

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

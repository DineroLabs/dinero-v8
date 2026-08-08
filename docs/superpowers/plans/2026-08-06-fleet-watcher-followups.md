# Fleet Watcher — follow-ups

Triaged during the whole-branch review of `design/fleet-watcher`. Everything a
reviewer raised that was deliberately not fixed before merge, with the reasoning
kept so nobody re-derives it.

## Hard gates before first deploy

These are not code changes; they are things that must be *done* on the host, and
the branch cannot prove either of them from a macOS checkout.

- **Run `systemd-analyze verify` on the Linux host.** The unit has never been
  semantically validated. A structural parse was substituted during development.
- **Verify the node-side `fleet-watcher-rpc` wrapper.** It is deployed per node
  and lives outside this repo, so its install-time check is the only real proof
  of the stdin contract. A wrapper that mishandles the payload presents as every
  node unreachable — a fleet-wide outage caused by a one-line shell mistake.
- **Test the dead-man switch by stopping the service and confirming an alert
  arrives.** Documented in the README. An untested dead-man is indistinguishable
  from a dead one, and it is the failure that hides all the others.

## Worth fixing

- **No attempt cap or dead-letter for a permanently rejected notification.**
  A bad Pushover token means the outbox row is rejected forever, retries at the
  1800s cap indefinitely, and holds `has_overdue_critical` true — so the
  heartbeat is suppressed and the dead-man pages with no way to clear it but
  editing the database. A transient outage self-heals; this is the permanent
  case only. Fails loud rather than silent, which is why it did not block merge.
- **A whole-fleet stall is undetectable.** Rules are pure functions over a single
  cycle, so a fleet that stops advancing *together* looks perfect — every node
  agrees, at the same height, forever. Needs cross-cycle state.
- **A node ahead of the quorum never fires anything.** `node_behind` measures in
  one direction only.
- **`engine.SILENT_RULES` is dead code.** Suppression happens at delivery via
  `NEVER_DELIVERED`; two sources of truth for one idea.
- **A failed heartbeat ping is silent** — no log, no counter. One line, and it
  would help a postmortem.
- **A tied fork fires both `consensus_health` and `tip_divergence`.** Two
  emergency incidents for one event. Redundant rather than wrong; the spec's
  precedence rule covers only `majority_unreachable`/`consensus_health`.

## Won't fix

- **`Quorum` carries no denominator**, so 2-of-9 would look like unanimity.
  Irrelevant at three voters; revisit if the fleet grows.
- **`height=0` / `tip_hash=""` pass the `reachable` check.** Inert — the rules
  compare `hashes_at`, never `tip_hash`.
- **`majority_unreachable` reports an empty node tuple when the majority is lost
  purely to *missing* nodes.** Structurally unreachable: `poll_cycle` emits one
  observation per configured node.
- **Assorted cosmetics** — a redundant `dict()` conversion, ISO-vs-epoch columns
  in one table, a recovery message rendering a Python list repr, node lists
  joined in caller order while stored sorted.

## Deliberately out of scope

- **Sub-project A, the node-side reorg event feed.** Separate spec, separate PR.
  Nothing here depends on it, and it was sequenced second on purpose: this
  watcher works against RPCs that exist today, so it keeps working if A is
  delayed.

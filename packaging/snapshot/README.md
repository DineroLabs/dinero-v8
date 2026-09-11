# Snapshot publication transition

The mainnet exporter selects v4 below base height 111000 and v5 at/above it.
Changing a file's version bytes does not create a binding proof. Upgrade the
exporting daemon and the publisher's throwaway validation daemon together.

`dinero-snapshot-publish.sh` is a reusable Linux/systemd publishing job derived
from EU1's deployed job. Configure DATADIR, PUBROOT, CLI_BIN, DINEROD and KEY.
It retains a pending snapshot instead of continually redumping an unburied tip,
checks its base against the selected chain, waits for 288 descendants for v5,
and requires the fresh daemon's full snapshot-load gate before atomic publication.
The daemon, not the scheduler's height check, enforces the proof and burial.
An activated v4 dump fails closed and keeps the previous published set.

A signing key remains required for legacy snapshots. Activated v5 publication
may omit it. When configured, the publisher signs as before. The fetcher requires
the pinned signature for legacy/pre-activation snapshots; activated v5 may be
staged without it. `--require-publisher-signature` explicitly adds the fleet-key
pin. Regardless of signature, the daemon must validate v5 against its independently
obtained selected PoW header chain before importing state. The fetcher only
checks transfer and header format; it does not call a download consensus-valid.

`--mirror` permits another publisher. The built-in three URLs are defaults,
not decentralized discovery. Automatic discovery and mobile downloader parity
remain separate work. No claim of fully decentralized distribution is made.

Before deployment: run the transition tests and a regtest publisher lifecycle,
review the exact script, back up the installed job, and confirm the deployed
export/validation daemon supports v5. Do not remove legacy signature checks.
The first mainnet v5 base 111000 becomes eligible at tip 111288. Until then,
retain the last successfully validated publication while a v5 candidate waits.

Tests: `python3 -m unittest discover -s packaging/snapshot -p 'test_*transition.py'`.
These isolated tests cover scheduling/download policy; they do not replace the
Core forged-proof rejection and real-node snapshot-load lifecycle tests.

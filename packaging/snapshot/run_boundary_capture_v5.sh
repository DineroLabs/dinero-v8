#!/bin/bash
# Supervised one-shot entry point. Keep the daily publisher timer held until
# exact H149999 is captured; a service restart must recheck that condition.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SYSTEMCTL_BIN=${SYSTEMCTL_BIN:-/usr/bin/systemctl}
TIMER_UNIT=${SNAPSHOT_PUBLISHER_TIMER_UNIT:-dinero-snapshot.timer}

if ! TIMER_STATE=$("$SYSTEMCTL_BIN" show -p ActiveState --value "$TIMER_UNIT"); then
    echo "boundary capture refused: cannot read $TIMER_UNIT state" >&2
    exit 1
fi
case "$TIMER_STATE" in
    inactive|failed) ;;
    active|activating|reloading|deactivating)
        echo "boundary capture refused: $TIMER_UNIT is $TIMER_STATE; timer must be held" >&2
        exit 2  # Terminal until an operator holds the timer and restarts us.
        ;;
    *)
        echo "boundary capture refused: unexpected $TIMER_UNIT state: $TIMER_STATE" >&2
        exit 1
        ;;
esac

exec "${PYTHON_BIN:-python3}" "$SCRIPT_DIR/capture_boundary_v5.py" --watch

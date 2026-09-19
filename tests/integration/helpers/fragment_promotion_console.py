#!/usr/bin/env python3
"""Exercise promotion assertions with a fragmented console diagnostic.

Only captured stdout is changed. The actual daemon, RPC results, and serialized
Logger file are untouched. CTest enables this so restoring the old console grep
deterministically fails instead of waiting for a thread-interleaving accident.
"""

import signal
import subprocess
import sys


def main():
    child = subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)

    def forward(signum, _frame):
        try:
            child.send_signal(signum)
        except ProcessLookupError:
            pass

    for signum in (signal.SIGTERM, signal.SIGINT):
        signal.signal(signum, forward)

    prefix = b"ConnectTip SUCCEEDED for height "
    with child.stdout:
        for line in child.stdout:
            line = line.replace(
                prefix,
                prefix + b"[test: interleaved console diagnostic]\n",
            )
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()
    result = child.wait()
    return result if result >= 0 else 128 - result


if __name__ == "__main__":
    sys.exit(main())

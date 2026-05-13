#!/usr/bin/env python3
"""
Cross-platform timeout wrapper for commands
Usage: python3 scripts/timeout-wrapper.py 30 ./some-command arg1 arg2
"""

import subprocess
import sys
import signal
import time

def main():
    if len(sys.argv) < 3:
        print("Usage: timeout-wrapper.py <seconds> <command> [args...]", file=sys.stderr)
        sys.exit(1)
    
    try:
        timeout_seconds = int(sys.argv[1])
    except ValueError:
        print(f"Invalid timeout: {sys.argv[1]}", file=sys.stderr)
        sys.exit(1)
    
    command = sys.argv[2:]
    
    try:
        process = subprocess.Popen(command)
        process.wait(timeout=timeout_seconds)
        sys.exit(process.returncode)
    except subprocess.TimeoutExpired:
        print(f"Command timed out after {timeout_seconds} seconds", file=sys.stderr)
        try:
            process.send_signal(signal.SIGINT)
            time.sleep(0.5)
            if process.poll() is None:
                process.kill()
        except:
            pass
        sys.exit(124)  # Standard timeout exit code
    except KeyboardInterrupt:
        try:
            process.send_signal(signal.SIGINT)
            time.sleep(0.5)
            if process.poll() is None:
                process.kill()
        except:
            pass
        sys.exit(130)  # Standard SIGINT exit code

if __name__ == '__main__':
    main()

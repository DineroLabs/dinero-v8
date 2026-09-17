#!/usr/bin/env python3
"""Exercise actual daemon ownership in full and CSN modes, using isolated data."""
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
root = Path(tempfile.mkdtemp(prefix="dinero-service-release-"))
success = False
try:
    for mode in ("full", "csn"):
        datadir = root / mode
        datadir.mkdir()
        config = datadir / "dinero.conf"
        config.write_text("p2p.offline=true\n")
        sockets = [socket.socket() for _ in range(3)]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        ports = [sock.getsockname()[1] for sock in sockets]
        for sock in sockets:
            sock.close()
        command = [binary, "--regtest", f"--datadir={datadir}", f"--conf={config}",
                   f"--rpcport={ports[0]}", f"--port={ports[1]}",
                   f"--wallet-socket-port={ports[2]}", "--listen=0", "--utreexo=1",
                   "--utreexo-stateless=1" if mode == "csn" else "--utreexo-bridge=1"]
        log = root / f"{mode}.log"
        with log.open("w") as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=120)
        output = log.read_text(errors="replace")
        if result.returncode != 0:
            print(output[-20000:], flush=True)
            raise RuntimeError(f"{mode} daemon release failed: exit {result.returncode}")
        assert " real daemon services released" in output, "missing release assertion"
        print(f"PASS {mode} daemon ownership and repeated Stop", flush=True)
    success = True
finally:
    if success:
        shutil.rmtree(root)
    else:
        print(f"Retained service-release evidence: {root}", flush=True)

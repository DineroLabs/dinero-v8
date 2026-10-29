#!/usr/bin/env python3
"""A backend-OFF daemon must refuse all activation controls before DB creation."""
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="orchard-off-handoff-") as directory:
    for name in (".orchard-handoff-selected-v1", ".orchard-handoff-selected-v1.partial",
                 ".orchard-handoff-selected-v1.done", ".orchard-handoff-selected-v1.done.partial"):
        datadir = Path(directory) / name.removeprefix(".")
        chain = datadir / "blockchain"
        chain.mkdir(parents=True)
        control = chain / name
        control.write_bytes(b"untrusted control must be retained\n")
        config = datadir / "dinero.conf"
        config.write_text("p2p.stun.enabled=false\np2p.upnp=false\np2p.natpmp=false\n"
                          "p2p.portmap=off\np2p.relay=off\np2p.tor_mode=disabled\n")
        result = subprocess.run([binary, "--regtest", f"--datadir={datadir}",
                                 f"--conf={config}", "--utreexo=1", "--listen=0"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=30)
        assert result.returncode != 0, result.stdout
        assert "Authenticated activation handoff requires the Orchard runtime" in result.stdout, result.stdout
        assert control.read_bytes() == b"untrusted control must be retained\n"
        assert not (chain / "chaindb").exists(), "refusal created a replacement database"
        print(f"PASS: refused {name} without creating ChainDB")

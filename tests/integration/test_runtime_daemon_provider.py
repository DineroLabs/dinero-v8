#!/usr/bin/env python3
"""One ordinary scheduled-regtest DaemonApp Start/pass/Stop in isolated data."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import shutil

binary=str(Path(sys.argv[1]).resolve())
root=Path(tempfile.mkdtemp(prefix="dinero-runtime-provider-"))
success=False
try:
    config=root/"dinero.conf"
    config.write_text("p2p.offline=true\npool.accounting.enable=true\nlightning.oracles.enable=false\nwallet.socket.enable=false\n")
    sockets=[socket.socket() for _ in range(3)]
    for sock in sockets:sock.bind(("127.0.0.1",0))
    ports=[sock.getsockname()[1] for sock in sockets]
    for sock in sockets:sock.close()
    command=[binary,"--fixture-scheduled-orchard","--regtest",f"--datadir={root}",f"--conf={config}",
             f"--rpcport={ports[0]}",f"--port={ports[1]}",f"--wallet-socket-port={ports[2]}",
             "--listen=0","--utreexo=1","--utreexo-bridge=1"]
    with (root/"execution.log").open("w") as log:
        result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=120)
    output=(root/"execution.log").read_text(errors="replace")
    if result.returncode:
        print(output[-24000:],flush=True)
        raise RuntimeError(f"scheduled provider fixture failed: exit {result.returncode}")
    required=["PASS scheduled regtest daemon installed actual typed provider",
              "PASS actual daemon typed provider matches backend and profile after worker startup",
              "PASS actual daemon typed provider detached before dependency release",
              "PASS actual daemon initial durable scan without fabricated progress",
              "PASS actual daemon delivery worker drained before dependency release",
              "PASS actual daemon pool maintenance initial pass or explicit absence",
              "PASS actual daemon pool callback lifetime closed"]
    for marker in required:assert output.count(marker)==1,marker
    installed="Typed runtime notifications installed; consumer recovery remains asynchronous"
    assert output.count(installed)==1
    assert output.index(installed)<output.index("[DaemonApp] Starting core services...")
    assert output.index(installed)<output.index("Startup recovery complete; starting external listeners")
    recovered="Runtime recovery workers woken after core startup and mempool recovery"
    assert output.count(recovered)==1
    assert output.index("[DaemonApp] ✅ Core services started")<output.index(recovered)
    assert output.index("Loading mempool from disk...")<output.index(recovered)
    assert output.index(recovered)<output.index("Startup recovery complete; starting external listeners")
    assert output.count("phase=runtime_notifications_detached")==1
    assert output.index("phase=runtime_notifications_detached")<output.index("phase=runtime_delivery_stopped")<output.index("phase=pool_payments_closed")
    assert " real daemon services released" in output
    assert "ChainOracleClient connection failed" not in output
    assert "TimeOracleClient connection failed" not in output
    assert "TransactionOracleClient connection failed" not in output
    print("\n".join(line for line in output.splitlines() if line.startswith("PASS ")),flush=True)
    print("PASS scheduled daemon provider installed before core startup and released before dependencies",flush=True)
    # Separate fresh process/datadir; actual enabled configuration must refuse
    # initialization before constructing or connecting legacy IPC clients.
    refused=root/"enabled-oracles";refused.mkdir()
    refused_config=refused/"dinero.conf"
    refused_config.write_text("p2p.offline=true\npool.accounting.enable=true\nlightning.oracles.enable=true\nwallet.socket.enable=false\n")
    refused_command=[arg for arg in command]
    refused_command[1]="--fixture-oracle-enabled-refusal"
    refused_command=[f"--datadir={refused}" if arg.startswith("--datadir=") else f"--conf={refused_config}" if arg.startswith("--conf=") else arg for arg in refused_command]
    with (root/"enabled-refusal.log").open("w") as log:
        result=subprocess.run(refused_command,stdout=log,stderr=subprocess.STDOUT,timeout=120)
    output=(root/"enabled-refusal.log").read_text(errors="replace")
    if result.returncode:
        print(output[-24000:],flush=True)
        raise RuntimeError(f"enabled-oracle refusal fixture failed: exit {result.returncode}")
    marker="PASS scheduled daemon refused enabled legacy oracles before IPC probe"
    assert output.count(marker)==1
    assert "refusing before IPC probe" in output
    assert "Typed runtime notifications installed" not in output
    assert "[DaemonApp] Starting core services..." not in output
    for name in ["ChainOracleClient","TimeOracleClient","TransactionOracleClient"]:
        assert name+" connection failed" not in output
        assert name+" wired" not in output
    print(marker,flush=True)
    destruct=root/"destructor";destruct.mkdir()
    destructor_config=destruct/"dinero.conf"
    destructor_config.write_text("p2p.offline=true\npool.accounting.enable=true\nlightning.oracles.enable=false\nwallet.socket.enable=false\n")
    destructor_command=[arg for arg in command];destructor_command[1]="--fixture-provider-destructor"
    destructor_command=[f"--datadir={destruct}" if arg.startswith("--datadir=") else f"--conf={destructor_config}" if arg.startswith("--conf=") else arg for arg in destructor_command]
    with (root/"destructor.log").open("w") as log:
        result=subprocess.run(destructor_command,stdout=log,stderr=subprocess.STDOUT,timeout=120)
    output=(root/"destructor.log").read_text(errors="replace")
    if result.returncode:
        print(output[-24000:],flush=True)
        raise RuntimeError(f"provider destructor fixture failed: exit {result.returncode}")
    marker="PASS scheduled daemon destructor detached provider and closed actual workers"
    assert output.count(marker)==1
    assert output.count("phase=runtime_notifications_detached")==1
    assert output.index("phase=runtime_notifications_detached")<output.index("phase=runtime_delivery_stopped")<output.index("phase=pool_payments_closed")
    print(marker,flush=True)
    success=True
finally:
    if success:shutil.rmtree(root)
    else:print(f"Retained scheduled-provider evidence: {root}",flush=True)

#!/usr/bin/env python3
"""Exercise the harness's actual TCP rule in an isolated network namespace."""
import argparse
import errno
import os
from pathlib import Path
import shlex
import socket
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--harness',required=True,type=Path)
args=parser.parse_args()
if os.geteuid()!=0 or os.readlink('/proc/self/ns/net')==os.readlink('/proc/1/ns/net'):
    raise SystemExit('Run only as root under unshare --net; host networking must stay untouched')
lines=[x.strip() for x in args.harness.read_text().splitlines()
       if x.strip().startswith('-s "${TARGET_IP}" -d "${RELAY_IP}" -p tcp ')]
if len(lines)!=2 or lines[0]!=lines[1]:
    raise SystemExit('Expected matching insert/delete reconnect-hold rules')
subprocess.run(['ip','link','set','lo','up'],check=True)
server=socket.socket();server.settimeout(2);server.bind(('127.0.0.1',0));server.listen(8)
port=server.getsockname()[1]
client=socket.create_connection(('127.0.0.1',port),timeout=2)
peer,_=server.accept();peer.settimeout(2)
parts=shlex.split(lines[0])
if not parts or parts.pop()!='||':
    raise SystemExit('Expected shell failure guard after each reconnect-hold rule')
values={'${TARGET_IP}':'127.0.0.1','${RELAY_IP}':'127.0.0.1','${RELAY_P2P}':str(port)}
rule=[values.get(x,x) for x in parts]
command=['iptables','-I','OUTPUT','1',*rule]
subprocess.run(command,check=True)
try:
    # The already-established stream must carry its teardown through the rule.
    client.shutdown(socket.SHUT_WR)
    try:closed=peer.recv(1)==b''
    except OSError:closed=False
    if not closed:
        raise AssertionError('reconnect hold blocks established TCP teardown')
    # A fresh dial must remain blocked while that same rule is installed.
    with socket.socket() as fresh:
        fresh.settimeout(2)
        result=fresh.connect_ex(('127.0.0.1',port))
        if result not in (errno.ECONNREFUSED,errno.EACCES,errno.EPERM):
            raise AssertionError(f'reconnect hold did not reject new TCP connection: {result}')
    print('PASS actual reconnect rule permits established teardown and rejects new dials')
finally:
    subprocess.run(['iptables','-D','OUTPUT',*rule],check=True)
    peer.close();client.close();server.close()

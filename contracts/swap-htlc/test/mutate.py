#!/usr/bin/env python3
"""Mutation check for DinSwapHTLC: breaks one rule at a time and requires the
Foundry suite to fail for each. Run from contracts/swap-htlc: python3 test/mutate.py
Exit 0 only if every mutation is caught. The source file is always restored."""
import re,shutil,subprocess,sys
P='src/DinSwapHTLC.sol'
M=[
("sha256->keccak","if (sha256(abi.encodePacked(preimage)) != s.hashlock)","if (keccak256(abi.encodePacked(preimage)) != s.hashlock)"),
("claim at T allowed","if (block.timestamp >= s.timelock) revert ClaimWindowClosed();","if (block.timestamp > s.timelock) revert ClaimWindowClosed();"),
("refund before T","if (block.timestamp < s.timelock) revert RefundNotOpen();","if (block.timestamp + 1 < s.timelock) revert RefundNotOpen();"),
("claim pays submitter","_pay(s.token, s.recipient, s.amount - tip);","_pay(s.token, msg.sender, s.amount - tip);"),
("id without chain","block.chainid, address(this), sender,","address(this), sender,"),
("id without sender","block.chainid, address(this), sender, recipient,","block.chainid, address(this), recipient,"),
("no IdUsed","if (swaps[id].state != State.None) revert IdUsed();",""),
("state not set on claim","        s.state = State.Claimed;\n",""),
("no reentrancy guard","        if (entered != 1) revert Reentered();\n",""),
("failed payout reverts","            credits[token][to] += amount;\n            emit Credited(token, to, amount);","            revert TransferFailed();"),
("no fee-on-transfer check","if (_balanceOf(token) != before + amount) revert FeeOnTransfer();",""),
("no ETH value check","if (msg.value != amount) revert BadValue();",""),
("no-return tokens fail","return size == 0 || (size >= 32 && word == 1);","return size >= 32 && word == 1;"),
("no timelock bounds","if (timelock < block.timestamp + MIN_LOCK || timelock > block.timestamp + MAX_LOCK) revert BadTimelock();",""),
("tip > amount allowed","if (amount == 0 || tip > amount) revert BadAmount();","if (amount == 0) revert BadAmount();"),
("refund pays tip","        _pay(s.token, s.refundTo, s.amount);\n","        _pay(s.token, s.refundTo, s.amount - s.tip);\n        if (s.tip != 0) _pay(s.token, msg.sender, s.tip);\n"),
("unbounded payout gas","_sendEth(to, amount, ETH_PAYOUT_GAS)","_sendEth(to, amount, gasleft())"),
("credit not cleared","        credits[token][msg.sender] = 0;\n",""),
("preimage not stored","        preimages[id] = preimage;\n",""),
("token tip to recipient","if (tip != 0) _pay(s.token, msg.sender, tip);","if (tip != 0) _pay(s.token, s.recipient, tip);"),
("refund pays sender","        _pay(s.token, s.refundTo, s.amount);\n","        _pay(s.token, s.sender, s.amount);\n"),
# The invariant alone must catch a claim that reverts for a rejecting payee.
("failed payout reverts [invariant only]","            credits[token][to] += amount;\n            emit Credited(token, to, amount);","            revert TransferFailed();",["--match-contract","Invariants"]),
]
src=open(P).read()
bad=0
try:
  for m in M:
      name,a,b=m[:3]; extra=list(m[3]) if len(m)>3 else []
      if src.count(a)!=1: print("SKIP(count)",name,src.count(a)); bad=1; continue
      open(P,'w').write(src.replace(a,b))
      shutil.rmtree('cache/invariant',ignore_errors=True)  # no replayed failures
      r=subprocess.run(['forge','test']+extra,capture_output=True,text=True)
      out=r.stdout+r.stderr
      fails=sorted({m.group(1) for l in out.splitlines() if l.startswith('[FAIL') for m in [re.search(r'((?:test|testFuzz|invariant)_\w+)', l)] if m})
      failed=any(l.startswith('[FAIL') for l in out.splitlines())
      status="CAUGHT" if failed else ("COMPILE-ERR" if 'Compiler run failed' in out else "SURVIVED")
      if status!="CAUGHT": bad=1
      print(f"{status:9} {name:40} {', '.join(fails)[:150]}")
finally:
  open(P,"w").write(src)
sys.exit(bad)

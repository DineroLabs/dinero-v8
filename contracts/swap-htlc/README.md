# DinSwapHTLC

The Ethereum and Base leg of DIN↔ETH atomic swaps: one immutable, ownerless
hash-time-locked contract for ETH and ERC-20 tokens. The design and the rule
each test checks are in `docs/design/din-eth-atomic-swaps.md`.

Not deployed. Mainnet deployment waits for an external audit.

```sh
forge build
forge test                 # unit, fuzz (2,000 runs each) and the accounting invariant
python3 test/mutate.py     # breaks each rule in turn; every break must fail a test
```

`forge test` also runs as the `SwapEvmContract` ctest when `forge` is installed.

| Item | Value |
| --- | --- |
| Compiler | solc 0.8.28, optimizer 10,000 runs, EVM cancun, no metadata hash |
| Dependencies | none (tests declare only the cheatcodes they use) |
| Hash | SHA-256 of a 32-byte secret, the same hashlock as the Dinero Taproot leaf |
| Windows | claim while `block.timestamp < timelock`; refund once `>= timelock` |
| Timelock bounds | now + 1 h to now + 7 days |
| Failed payouts | credited to the payee; `withdraw(token, to)` by the payee |
| ETH payout gas | 100,000; a payee needing more is credited instead |

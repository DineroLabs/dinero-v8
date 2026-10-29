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

The first `forge build` downloads solc 0.8.28. An offline CI runner needs that
compiler installed beforehand (`forge build` once with network, or `svm install 0.8.28`).

## Notes for auditors

In scope: `src/DinSwapHTLC.sol`, against the "Ethereum contract" section of the
design. Out of scope, handled by the client and stated to users:

- **Token allowlist.** Any token can be locked; the client offers only ETH and
  USDC. A token that lies about balances, or loses its code after `lock`
  (created and self-destructed in the same transaction), is not defended against
  on chain.
- **Relayer tip front-running.** Whoever submits the claim takes the tip; the
  amount still goes to the fixed recipient.
- **Issuer freezes.** If Circle freezes the contract itself, locked USDC is stuck.
  A frozen payee is credited and withdraws to another address.

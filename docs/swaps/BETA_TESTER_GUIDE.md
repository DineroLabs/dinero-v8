# DIN ↔ BTC atomic swaps: beta tester guide

This is a **small-amount mainnet beta between known testers**. The code has
had internal and independent reviews but no third-party audit yet. Use only
amounts you can afford to lose.

## What a swap is

- **Alice** sells DIN for BTC and **Bob** buys DIN with BTC. Each side locks
  coins in a script on its own chain. Either both sides complete, or both get
  their coins back after the deadlines. Nobody holds the other's coins, and no
  server is involved.
- **The order of events:**
  1. Alice locks her DIN first.
  2. Bob locks his BTC only after Alice's DIN lock has 30 confirmations.
  3. Alice claims the BTC, which reveals a secret.
  4. Bob's node uses that secret to claim the DIN.
- **Deadlines:** if Bob never locks, Alice gets her DIN back after the DIN
  deadline (about 4 days). If Alice never claims, Bob gets his BTC back after
  the BTC deadline (about 2 days).

## Limits during the beta

| | Default | Hard ceiling (cannot be raised) |
|---|---|---|
| BTC per swap | 0.001 BTC | 0.01 BTC |
| DIN per swap | 10,000 DIN | 100,000 DIN |

Your node checks these limits both on offers you make and on offers you
accept.

**Other rules your node enforces:**
- **Minimum sizes:** at least 100× the urgent DIN fee (1 DIN by default) and
  20× the BTC fee (0.0002 BTC by default), so fees always fit.
- **Lock lengths:** a taker refuses any lock longer than 7 days.
- **Confirmations:** at least 3 BTC confirmations before Alice reveals her
  secret. A Dinero outcome counts as final only after 30 confirmations.

## What you need

1. **dinerod** built from the swap beta branch, with your own wallet. The
   wallet **must be encrypted** (`wallet.encrypt`).
2. **Bitcoin Core** (v25 or newer; pruned is fine) that you control, with RPC
   enabled.
   - Bob funds his BTC from that node's wallet. **Load exactly one wallet.**
   - Alice only needs it to watch the chain. Her BTC arrives at the address she
     types.
3. **dinero-qt** built with `-DDIN_ENABLE_SWAP_UI=ON`. You can also use the
   `swap.*` RPCs directly.

## Configure dinerod

Add to `dinero.conf`:

```
swap.enable=1
swap.mainnet_beta=1
swap.btc_rpc=127.0.0.1:8332
swap.btc_rpc_user=<bitcoin rpc user>
swap.btc_rpc_pass=<bitcoin rpc password>
# required on mainnet to BUY DIN (Bob): a running watchtower (see below)
# swap.tower_inbox=/path/to/tower/inbox
```

Without `swap.mainnet_beta=1`, dinerod refuses to start with swaps enabled.
**Never expose your node's RPC port to the internet.**

## Doing a swap in dinero-qt (Swap tab)

**Alice (selling DIN):**

1. Enter the DIN amount, the BTC amount you want, and your Bitcoin address.
   Review the dialog, then click **Create offer**.
2. Send the offer text (`dinswap1o…`) to Bob over any channel.
3. When Bob's accept (`dinswap1a…`) arrives, paste it, click **Review…** and
   confirm. Your node locks your DIN.
4. Keep the node running and the wallet unlocked. Your node claims the BTC
   automatically, at least 6 hours before the BTC deadline.

**Bob (buying DIN):**

1. Paste Alice's offer, enter your Bitcoin refund address and click
   **Review…**. Check the amounts, the rate and both deadlines, then confirm.
   - On mainnet this needs a running watchtower; see below.
2. Send your accept text back to Alice.
3. Your node waits for 30 confirmations of Alice's DIN lock, then:
   - builds and signs your BTC funding **without sending it**;
   - hands your watchtower a package for exactly that funding;
   - sends the BTC **only after the tower confirms it holds the package.**

   If the tower doesn't confirm within 30 minutes, nothing is sent and you can
   cancel.
4. When Alice claims the BTC, your node (or your tower) claims the DIN.
   - The claim first pays a swap key of yours. A second small transaction then
     moves the DIN to your wallet; it can also raise the fee if the claim is
     stuck.
   - The tab shows **"Done — moving the DIN to your wallet"** until that
     second transaction confirms.

## Your two duties

1. **Stay online with the wallet unlocked** until the swap is Done or Refunded.
   - Swaps already running keep going if the wallet locks itself again later.
   - A daemon **restarted** while the wallet is locked pauses its swaps until
     you unlock it. The tab says "Paused — unlock the wallet".
   - Bob is most at risk: if he is offline when Alice claims the BTC, he must
     claim the DIN before the DIN deadline. Bob can run
     `dinero-swap-tower --inbox DIR …` on an always-on machine and set
     `swap.tower_inbox=DIR`. The tower holds only pre-signed transactions,
     never keys. **On mainnet this is required to buy DIN.**
2. **Don't start a swap you can't follow through.**
   - You can **cancel** only before your coins are locked.
   - After that, the swap finishes or refunds on its own, as long as your
     node runs.

## If something goes wrong

- **Manual refund.** `swap.refund <id>` broadcasts your own refund right away,
  whatever the swap's state. It never reveals the secret. Before the deadline
  the node refuses it as "non-final", which is expected.
- **Recovery from your seed.** Swap keys are derived from your wallet seed, at
  path `m/1398227280'/…`. A wallet restored from the seed can decrypt its files
  in `<datadir>/swaps` and finish or refund.
- **Alice's secret is random per offer** and lives only in those encrypted
  files. Losing the file of an offer nobody accepted locks nothing.
- **Keep `<datadir>/swaps` with your backups.** The files are encrypted.
- **"Lost" state:** contact the team immediately. Include the swap id (shown in
  the tab), the state and the events.
- **Reporting issues:** send the swap id, both nodes' `swap.status` output and
  the `[Swap]` lines from the dinerod log to team@dinerolabs.org. Don't send
  wallet files or seeds.

## Known limitations

- **One fee bump on Dinero.** Dinero nodes don't replace transactions by fee.
  - The DIN claim fee is chosen by how close the DIN deadline is, and once
    sent the same claim is re-sent unchanged.
  - A stuck claim gets exactly one bump, through a child transaction (CPFP).
    It is held back until it matters: within 12 hours of the DIN deadline,
    or after the claim has sat unmined for 6 hours. It pays at least the
    urgent fee (the tower sends its largest pre-signed child).
  - Regtest showed that nodes accept the child. That miners prefer the pair
    when blocks are full is not yet tested.
- **Small BTC fee bumps.** A stuck BTC claim or refund is re-sent with a
  doubling fee, capped at 5% of the BTC amount. At the smallest swap sizes
  the cap leaves little room to bump.
- **Bob's node and his tower may both bump his BTC refund.** Each replaces
  the other's at a higher fee; this costs fee (up to the cap), not funds.
- **Slow first look after a restart.** Right after dinerod starts, each swap
  scans the blocks since it began. Until that finishes, `swap.*` commands
  (including `swap.refund`) can take a few minutes to answer.
- **One Bitcoin wallet only.** Bitcoin Core must have exactly one loaded
  wallet; there is no Electrum support yet.
- **Copy-paste only.** Offers are exchanged by copy-paste; there is no order
  book.

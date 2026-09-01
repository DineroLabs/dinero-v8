# Covenant post-activation smoke transaction

Mainnet covenant activation is height **100,000**. This procedure prepares a
small CTV transaction for review without signing or broadcasting it.

## Hard gates

1. Confirm three independent nodes report the same active-chain tip at or
   above 100,000 and are not in initial block download.
2. Confirm the running daemon commit contains the covenant profile and wallet
   recovery migrations, and `wallet.covenant.list` succeeds.
3. Export and verify the receiving address out of band. Use a disposable
   amount of 0.001 DIN (100,000 una) and an explicit 1,000 una fee.
4. Keep the wallet locked while preparing and reviewing the template.

## Prepare only

Call `wallet.covenant.ctvcreate` with one sequence, one 99,000 una destination
output, and `track:false`. Save the returned descriptor, template hash,
unsigned template hex, address, and current tip in the evidence package.

The returned transaction has zero prevouts by design and is marked
`broadcastable:false`. Do not send it to `sendrawtransaction`. Review these
items byte-for-byte on a second machine:

- destination script and amount;
- input count, sequence `0xfffffffe`, version, and locktime;
- template hash and descriptor checksum;
- Taproot output address, tapscript, and control block;
- wallet descriptor count remains unchanged.

## Authorized execution after review

Only after two-person review, unlock the disposable test wallet and invoke
`wallet.covenant.ctvfund` with the same outputs and fee. Record the funding
txid, descriptor id, mempool acceptance, confirmation height, restart
discovery, and descriptor export. Then exercise the committed spend on the
small output and retain raw transaction and validation evidence.

Stop immediately on height disagreement, activation rejection, unexpected
descriptor mutation, fee/output mismatch, or a non-empty signature/witness in
the preparation artifact.

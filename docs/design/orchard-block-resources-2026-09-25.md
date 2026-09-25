# Draft Orchard mixed-block resource profile

The staged post-activation block preparation path now enforces three aggregate
bounds: eight Orchard bundles, 32 Orchard actions (including padding), and
80,000 signature operations. Transaction/block bytes and weight remain separate
existing bounds. These are draft profile constants, not live activation or a
claim that loaded-node/mobile performance qualification has passed. Their final
capacity must be reviewed against those measurements before release activation.

## Ordering and accounting

Before any coin lookup, native signature verification or Orchard proof
verification, block preparation charges every transaction body. Orchard bundles
and actions come from the bounded, typed decoder's facts; no validity is assumed
from those facts. Both transaction families' output scripts and historical
scriptSigs contribute their static signature-operation counts. The coinbase is
included in this static accounting.

During ordered coin resolution, the path charges input authorization work from
authenticated previous-output scripts before verifying that transaction. A
same-block child uses its actual parent's output. P2PKH, P2WPKH, P2MR and Taproot
key-path each add one operation; Taproot script-path counts CHECKSIG,
CHECKSIGVERIFY and CHECKSIGADD in the script after removing an annex. The
separate script validator still checks script type, shape, authorization and
activation. Unknown types are not granted spendability by resource accounting.

This is a deliberately explicit post-activation profile: it also counts key-path
Taproot input work instead of treating it as zero. It is not a claim of Bitcoin
sigop-policy equivalence and does not modify historical validation. Static
output costs and later spend costs both contribute. Potential operations in
unexecuted branches count conservatively. Different signature algorithms are
not claimed to have equal elapsed cost; PQ, script/hash work, verification
latency and hostile-load responsiveness still need full platform qualification.

The opcode walker uses bounded offsets, skips push payloads, and stops at a
truncated push. Multisig uses the previous opcode, never a byte within pushed
data. Script validity remains a distinct obligation. Every accumulator updates a
private copy and publishes usage only on success. The prepared block result
exposes the charged usage for later service/miner integration.

## Qualification and remaining callers

`OrchardResources` checks exact limits, over-limit failure without partial usage,
input/prevout shape, annex/script selection, push parsing, and whole-block early
rejection through the real mixed-block preparation entry point. A fresh real
8-action proof supplies the large-shape fixture. Framing-only variants used for
preflight tests do not claim complete transaction authorization or block validity.
`OrchardBlockCoins` additionally checks same-block child signature accounting.

The workflow builds the new test and requires it in both the inventory and
execution selector. Block preparation remains a staged component. Production
mempool/package selection, relay, mining and ConnectTip still need to call the
same accounting while enforcing all remaining consensus obligations. No runtime
caller or activation switch is enabled by this change.

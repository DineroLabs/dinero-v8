# Covenant activation deferral — 2026-08-22

Historical status: this stop decision was superseded by the 2026-08-30
re-authorization after the defect was corrected and the complete assurance
gate was rerun. The text below records the state and rationale on 2026-08-22;
it is not the current activation policy.

## Decision

Mainnet CTV and CCV remain coupled and are deferred to `UINT32_MAX`. Block
100,000 does not activate either opcode. No replacement height is authorized
by this change. Regtest remains active at height 20 and testnet remains
dormant.

Current mainnet consensus checksum:

```text
48bb4b27879a492dd8a83fd1e4826ec422f6b9ac3b1ae6797c9469783036c76e
```

## Superseded freeze

The original review record froze commit
`b04df276` at mainnet height 78,245, selected height 99,000 as its go/no-go
checkpoint, and proposed activation at height 100,000. That plan is retained
in `../consensus/COVENANT_MAINNET_ACTIVATION_100000.md` for audit history only.

## Material finding

Completing issue #483 imported the full upstream BIP119 transaction corpus.
Valid P2WSH and Taproot script-path vectors exposed that `VerifyScript` checked
the inner witness/tapscript result but left the evaluated witness program on
the outer stack. The final `CLEANSTACK` check therefore rejected otherwise
valid spends.

The corrected path requires exactly one true tapscript result and normalizes
successful P2WSH and Taproot script-path execution to the authoritative inner
result before the final outer-stack check. Because that is consensus-loosening
relative to deployed nodes, it is staged behind
`SCRIPT_VERIFY_CHECKTEMPLATEVERIFY`. Mainnet deferral therefore preserves the
deployed rejection behavior; regtest and the imported BIP119 corpus exercise
the corrected future profile. This remains a material post-freeze finding even
though it does not change live mainnet acceptance rules.

Project policy forbids preserving the old activation height by compressing the
review and deployment window after a material finding. The safe result is
deferral until a new freeze, public review opportunity, release-candidate
evidence, and explicitly authorized height exist.

## Vector provenance and coverage

The exact upstream source is Bitcoin BIPs commit
`ae747e2b909ab5dd32632ed3a8b09839193d53e3`:

| Vendored file | SHA-256 | Cases | Inputs |
|---|---|---:|---:|
| `tests/vectors/bip119_tx_valid.json` | `e7b56a4b434b041c06f8d66a25ebd3a1acbc49efed96082d0f611a53115281e0` | 19 | 23 |
| `tests/vectors/bip119_tx_invalid.json` | `2dcae11cfcd6918eebff3f2a3233e8e55105e8d14bc1413b848c460f4eef7ea2` | 11 | 15 |

The runner pins those hashes and exact corpus counts, covers all nine
conditional-softfork and nine skip-excluded cases, maps every supported flag
explicitly, matches prevouts by outpoint, rejects unknown flags/script tokens,
and tolerates zero skipped vectors. Valid cases run with the candidate CTV
profile enabled; four P2WSH/Taproot cases additionally prove that deployed
flags preserve legacy rejection while the deferred future profile accepts the
upstream-valid spend.

## Reproducible evidence

```text
cmake --build build-stability --target test_bip119_ctv_vectors test_covenant_activation -j8
ctest --test-dir build-stability --output-on-failure \
  -R '^(BIP119CTVVectors|CovenantActivation)$'
```

Both suites pass with the deferred parameters. This engineering change does
not deploy a binary to the live fleet; deployment remains a separate release
operation.

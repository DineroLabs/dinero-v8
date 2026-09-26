# First-event originated-history preflight

Known-script origin adoption now requires existing originating history for an
owned transparent spend in the actual first source event, as well as for spends
before the origin. The ordinary preflight completes this check before the index
transaction can commit. The ordinary writer and already-applied first-event
retry also check it. Existing receipt formats and ordered transactions remain.

The check starts from projected unspent coins in the ordinary script domain,
then walks actual typed first-event effects in order. Consuming an owned input
requires a matching existing history identity. Owned creations are added for
later transactions in the event. This prevents a missing originating record from
being hidden by a later output and does not infer send categories, fees, labels
or change income. A refusal leaves the existing history and both stores' progress
unchanged. The real service source binds the event; no event or cursor is made up.

This is a requirement for the explicit origin adopter, not a new general policy
for the narrower low-level delivery API. It does not recreate lost metadata,
establish complete script/key discovery, or supply durable pending bodies,
reservations, mempool acceptance or broadcast ownership. Orchard nullifier-only
ownership remains a separate account obligation. Provider/startup/activation
installation and steps 1–4 remain incomplete.

## Qualification

The actual signed service case first supplied only pre-origin send history. The
preceding consumer incorrectly accepted adoption despite missing first-event
originated history. That regression was observed before changing production code.
The corrected case requires refusal with neither store enrolled, then supplies
the existing first-event metadata and continues the signed adoption, rollback,
reopen, undo and reconnect checks. CI requires its separate execution marker.
Fresh declared service/index/replay targets and the full daemon build passed.
Three CTests passed: AssumeUtxoReplay (4.06s, eleven internal cases),
OrchardServiceDeliverySource (10.02s), and OrchardIndexDelivery (78.35s). Both
unchanged workflow selectors enumerate 46 enabled root tests; only these three
suites ran locally.

All 203 linked project C++ units were freshly ASan/UBSan instrumented. The actual
service fixture passed; normal and control maps contain no project C++ archive
members. Copied original-consumer, omitted-preflight and omitted-origin-coins
controls failed their intended assertions without sanitizer diagnostics. The
omitted-preflight control specifically leaves an index prefix and fails the
no-index-progress assertion, demonstrating why a check only in the later ordinary
writer is insufficient. The restored fixture passed. External/Rust libraries are
uninstrumented, macOS leak detection is off, and the ARM dependency gate is open.
Changed C++ sources are absent from the genuine OFF graph qualified at b1b3;
there was no new OFF build. Inherited labels and prebuilt OpenSSL are not release
provenance. No new fresh-process, physical-power-loss or whole-node claim is made.

The case uses a previous-block owned change input. In-order tracking also handles
known outputs created within the first event, but new same-block originated-
history qualification is not claimed here. All signed-spend fixture limitations
in [the preceding qualification](wallet-origin-signed-spends-2026-09-26.md) remain.

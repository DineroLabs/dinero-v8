# Reindex wire inventory — typed replay prerequisite

The flatfile inventory now retains each checksum-checked frame's common header
and exact body bytes. Chain selection uses header work and parent linkage.
Legacy transaction decoding runs only after a selected frame has a height;
an undecodable selected body aborts instead of disappearing from the inventory.
Short terminal magic is corruption, not successful EOF. Existing framing bounds
and checksum/header skip accounting remain. The iterative selection algorithm
and the deep-chain fixture assertions are unchanged apart from the record field.

This is an implementation prerequisite, not complete Orchard reindex support.
The current historical apply loop still cannot process the Orchard profile.
Typed application, independently validated activation prefix, genuine separated
candidate layout, retained delivery chronology/digest verification, and atomic
promotion remain required. Existing HTTP full-reindex assertions are unchanged.
No production configuration, activation height, wallet/account cursor or master
key is changed. Header linkage and local frame checksums are not body validity.

The new component fixture preserves a historical frame, an independently encoded
mixed-envelope frame rejected by the legacy decoder, and a malformed body with a
valid header. It checks exact frame bytes, offsets, normal/anchored selection and
truncated framing/IO failures. These are scanner/selector checks, not proof,
script, canonical replay, crash, promotion, or release qualification. Duplicate
header hashes retain existing first-record selection; competing body variants
need consideration by the complete typed replay owner.

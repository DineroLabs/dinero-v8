# Compact canonical disconnect integration

The compact canonical writer can reverse an enrolled selected block without a
full coin table or full forest. Its live owner remains bound to the database
and writer mutex. Disconnect requires exact current and predecessor catalog
records, their hash link, header/work/profile/stump bindings, and the retained
conventional undo digest.

The staging path authenticates inputs against the predecessor catalog and
candidate proof, revalidates the mixed transactions, and reproduces the forward
stump and byte-exact delta. It reconstructs conventional undo from those
validated changes and requires equality with the retained undo. The writer also
reconstructs the complete child catalog and requires every immutable node to
already exist with identical bytes. Missing nodes are refused; disconnect does
not repair or invent them.

Full-node and compact disconnect share Orchard state, retirement, body, filter,
transaction/height index, checkpoint and tip obligations. Full-node coin and
forest checks remain. Canonical locators and delivery outbox stay in the same
outer batch, and the preallocated compact predecessor is published only after
the existing checked synchronous commit.

The compact live owner retains the genuine retirement record obtained at first
enrollment. After undo crosses the activation boundary, reconnect uses that
retained authority. An optional conflicting caller record is refused. Database
absence or decoding a catalog does not authorize new enrollment.

Five new component cases cover boundary undo/reconnect and abandonment, late
commit refusal, actual legacy/modern/same-block full-node oracle comparisons,
corrupt predecessor/missing child nodes, and changed conventional undo.
Fresh full ON/OFF daemon and replay builds passed 17 selected CTests (65 ON and
15 OFF cases). All 323 linked C++ files were freshly ASan/UBSan instrumented;
65 selected cases passed. Two separately rebuilt copied data controls failed
the intended assertions, followed by 28 restored passing cases. Exact source,
commands, maps and results were independently verified and privately preserved.
Other external/Rust/C/PQClean dependencies remain uninstrumented; macOS LSan
was off. Full daemon/OFF binaries and whole-node/platform behavior are outside
that sanitizer scope. Existing assertions and deadlines were retained; there
is no initial-original-RED claim. Retaining the live owner while reopening an
isolated database is not a fresh-process startup test.

These results cover the undo source before the subsequent storage-mode guard.
They do not qualify that later implementation.

Production CSN routing remains guarded. Fresh-process enrollment, storage-mode
ownership, reindex, configured consumers and complete whole-node qualification
remain open. No deployment or mainnet activation is enabled.

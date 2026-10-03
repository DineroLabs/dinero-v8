# Orchard dependency advisory gate

The Orchard component workflow audits the committed Cargo.lock on every
applicable run. It installs exactly cargo-audit 0.22.2 with the tool's locked
dependencies and the backend's pinned Rust compiler. The advisory database is
fetched fresh; its commit, the source commit, lockfile SHA-256, tool version,
JSON report and exit status are retained as artifacts, including on failure.

Known vulnerabilities fail through cargo-audit's default policy. Unsoundness
warnings and yanked crates also fail explicitly. Scanner/network errors are not
converted to success. There are no advisory ignore IDs or automatic dependency
updates. Informational maintenance warnings remain in the full report and the
job summary; they do not independently fail this gate.

The first local scan found zero listed vulnerabilities and one maintenance
warning: RUSTSEC-2023-0089, atomic-polyfill 1.0.3, retained through the optional
postcard/heapless graph. `cargo tree --locked --target all --all-features -i
atomic-polyfill` reports no selected reverse path for this backend. The full
lockfile is still audited, rather than filtering the warning from the report.
No cryptographic dependency or lockfile was changed to obtain this result.

An initial experimental `--deny warnings` scan failed on that notice. The
committed policy distinguishes maintenance notices from known vulnerabilities,
unsoundness and yanks; a green result must not be described as “no advisories.”
The initial and policy reports are retained in the qualification ledger.

This detects published dependency advisories. It does not prove cryptographic
soundness, completeness of the advisory database, or safety of Dinero's host
integration. The source pin and lockfile must still be reviewed for each update.

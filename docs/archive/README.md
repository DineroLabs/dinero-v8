# docs/archive/

Historical markdown files that used to live at the repository root.

## What's here

511 `.md` files moved from the repo root on 2026-05-17. They are
**session-artifact status reports** — old AI-session outputs, finished
TODOs, "X_COMPLETE.md" / "X_AUDIT.md" / "X_FIXED.md" style notes
from earlier development phases. None of them are load-bearing
documentation.

## Why archived instead of deleted

Some contain historical context that might be useful if you're trying
to reconstruct *why* a decision was made. Keeping the files preserves
that archaeology without polluting `ls` at the repo root.

## What does NOT go here

- New documentation → write it as a proper doc in `docs/` (without
  the `archive/` prefix)
- New design proposals → open a GitHub issue / PR
- Decision logs → write commit messages, not `.md` files

## What lives at the repo root instead

Only canonical project docs:

- `README.md` — project overview
- `SECURITY.md` — points at the canonical Dinero Labs security policy
  at <https://dinerolabs.org/security/>
- `CHANGELOG.md` — release history
- `CONTRIBUTING.md` — how to contribute

Everything else at the root that you might be tempted to add
belongs either in `docs/`, in a GitHub issue, or in a commit message.

# Release Policy

## Purpose

DineroCoin releases mark **production-grade, subsystem-stable milestones**.
A release is created only when a defined scope of the system has been fully
validated, tested, and certified.

## What a Release Represents

A GitHub release:
- Corresponds to a specific git tag
- Represents the **entire repository state**, not a patch or diff
- Indicates that the covered subsystem(s) are considered stable and locked
- Is backed by documented tests and certification where applicable

## Criteria for Creating a Release

A release may be created only when:

- The daemon starts and runs without crashes
- All tests relevant to the release scope pass
- No known correctness or safety issues remain in scope
- Any temporary debug scaffolding has been removed
- Certification documentation (if applicable) is archived

## Scope-Based Releases

Releases may certify a subset of the system (e.g. mining, wallet, consensus).

Example:
- `v0.15.0-f5` certifies the mining subsystem (Phase F.5)

Certified subsystems are considered **invariant-stable** unless explicitly
superseded by a future release.

## Non-Goals

A release does **not** imply:
- Feature completeness
- Mainnet readiness (unless stated)
- Absence of future changes

It implies **correctness and stability within the declared scope**.

## Versioning

Version suffixes (e.g. `-f5`) correspond to internal development phases and
certified milestones.

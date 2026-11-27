# CLAUDE.md

Guidance for Claude Code in this repository.

## What this is

PresentationEngine: a headless-capable Metal presentation engine (C++20) plus the contracts its clients and plugins build against. The Mac app is a separate repo, `jterhorst/PresentationMacUI`, which consumes these contracts.

Read `docs/architecture.md` for the plan and `CONTRACTS.md` for the rules before changing any contract.

## Commands

- All contract checks: `tests/run_all.sh` (needs `pip install grpcio-tools jsonschema slangpy` and clang).
- Regenerate the 3D projection fixture, only on purpose: `python3 tests/reference/projection.py --write`.

## Rules

- **Never regress rendering performance.** A feature that is off costs zero GPU time and zero allocations per frame. New features bring a reference scene.
- **Keep the core small.** If it can be a plugin or module, it is one; extend the plugin API rather than the core.
- **Protobuf evolution:** additive only within a major version. Never reuse or renumber a field; mark removed fields `reserved`. Fields 50 (`metadata`) and 51 (`extensions`) exist on every persistent entity; 100–199 are reserved for core.
- **Round-trip unknown data.** Extensions, metadata, plugin elements and unknown fields must survive every read/write.
- **Module ABI** (`include/engine/module.h`): C only; append-only structs carrying `struct_size`; bump `EM_ABI_VERSION` only for incompatible changes.
- Every contract change updates `CONTRACTS.md` and adds or adjusts a check in `tests/`.
- License: MPL-2.0. Proprietary SDKs stay in separately distributed modules.

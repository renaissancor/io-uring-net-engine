# `doc/` — project guides + per-file code specs

This file indexes the **project-level guides** at `doc/` root. The
**per-source-file specs** live under `doc/<category>/` mirroring `src/`; their
build order and dependency graph are in [`INDEX.md`](INDEX.md) (the entry point
for building from spec). The *why* — dated rationale — lives in `../design-notes/`.

## What each guide covers

| File                                        | Audience              | Subject                                                                                                |
|---------------------------------------------|-----------------------|--------------------------------------------------------------------------------------------------------|
| [`00-overview.md`](00-overview.md)          | everyone, first       | Scope, layered subsystem map, design tenets, non-goals, glossary.                                      |
| [`01-windows-to-linux-mapping.md`](01-windows-to-linux-mapping.md) | porters from IOCP/Win32 | Master Win32 → Linux/POSIX API mapping (`SLIST`, `WaitForSingleObject`, `WSARecv`, `IOCP`, …).     |
| [`02-build-and-toolchain.md`](02-build-and-toolchain.md) | design reader   | Language standard, kernel requirements, dependency floors, repo layout. *What* we build against.       |
| [`04-coding-style.md`](04-coding-style.md)  | implementor           | Naming, error model, namespace tiers, project type aliases (`expected` / `unexpected`), header conventions. |
| [`05-cmake.md`](05-cmake.md)                | implementor           | CMake target shape, warning set, sanitizer presets, `cmake/deps.cmake` resolution pattern.             |
| [`06-system-setup.md`](06-system-setup.md)  | new contributor       | Distro-specific install runbook, smoke tests, kernel sanity check.                                     |
| [`07-ci-and-reproducibility.md`](07-ci-and-reproducibility.md) | release engineer | CI matrix, floor job, Dockerfile, devcontainer, `version-snapshot.txt`.                            |
| [`08-test-strategy.md`](08-test-strategy.md) | test author          | Test pyramid, per-subsystem coverage targets, sanitizer policy, wire-format parity test.              |
| [`2026-05-14-project-split.md`](../../design-notes/2026-05-14-project-split.md) | architect, integrator | Library / product two-repo architecture, boundary criteria, `find_package` seam, when to split. *(Historical — the split was later rejected; monorepo now.)* |
| [`10-realtime-server-architecture.md`](../../server-uring/doc/10-realtime-server-architecture.md) | everyone | Runtime shape, thread roles, ownership invariants, client state machine, first three-thread milestone. |

The `03-` slot is intentionally empty; an earlier
`03-second-pass-findings.md` retrospective was folded into the active
docs and dropped (the journal is preserved in git history).

## Suggested reading order

- **Building a unit from spec:** `INDEX.md` → the unit's `doc/<path>.md`.
- **First-time reader:** `00` → `10` → `01` → `04`. After that you can read any
  per-file spec independently.
- **Setting up a dev box:** `06` → `02` (kernel section) → `05`.
- **Touching CI / release:** `07` → `02` (kernel section) → `05`.
- **Writing a test:** `08` → the relevant per-file spec.

# <unit> — <one-line role>

<!--
  doc/ SPEC TEMPLATE.  Copy this for every source unit; the file mirrors src/
  1:1 (a header and its .cpp share ONE doc file: doc/<path>.md for
  src/<path>.h[/.cpp]).

  CONTRACT: doc/ is the SOURCE OF TRUTH. An agent must be able to implement the
  unit from THIS FILE ALONE — without opening design/. If it can't, the spec is
  incomplete. The "why" lives in design/; link to it, never depend on it.

  Keep the ## API block exact and normative — those signatures ARE the contract.
  Delete this comment in real specs.
-->

> **Status:** planned | in-progress | landed
> **Source:** `src/<path>.h` [ + `src/<path>.cpp` ]
> **Namespace:** `<ns>`
> **Depends:** `<unit>`, `<unit>`   (units that must be built first; names as in INDEX.md)

## Purpose

1–3 sentences: what this unit is and the one reason it exists. No history.

## API

The exact, normative signatures a caller sees. This is the contract the
implementation must satisfy and the agent implements against. Group logically
(construction / producer side / consumer side / observers). Include compile-time
constants, type aliases, and `static_assert`ed preconditions.

```cpp
namespace <ns> {
// ... exact declarations ...
}
```

## Invariants

Bullet list of what must ALWAYS hold — ownership rules, threading contract,
all-or-nothing guarantees. State whether violations are guarded (trap) or UB.

## Errors & edge cases

Each failure mode and the DEFINED behavior: return `0` / `false` / `nullptr`,
`LNX_CHECK` trap, no-op, etc. Cover empty/full/oversize/boundary explicitly.

## Notes

Non-obvious implementation constraints only: alignment, non-movable storage,
sizing math, cache-line placement, sanitizer gotchas. Skip the obvious.

## Test plan

The cases that prove the contract, mapped to `tests/<path>_test.cpp`. One line
per case.

## Done when

- [ ] Builds on `default` and `floor` presets
- [ ] Tests pass under ASan+UBSan
- [ ] This spec matches the built API (doc-is-source-of-truth)
- [ ] <unit-specific gate>

## Rationale

One-way links into the journal for the "why" (human-only; the spec above is
self-sufficient without them): `design/<date>-<topic>.md`, `.omc/wiki/<page>.md`.

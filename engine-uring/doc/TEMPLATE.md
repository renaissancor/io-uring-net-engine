# <unit> — <one-line role>

<!--
  doc/ TEMPLATE. One file per source unit, mirroring src/ 1:1 (a header and
  its .cpp share ONE doc: doc/<path>.md for src/<path>.h[/.cpp]).

  CONTRACT: this file DESCRIBES THE BUILT CODE. Write it from the header. If
  the header and this file disagree, the header is right and this file is
  stale. Do not describe planned behaviour; if a source comment mentions a
  future change, omit it or give it one clause marked "not built". The "why"
  is dated deliberation in ../../design-notes/ — link to it if useful, never
  depend on it.

  Delete this comment in real docs. Filled reference:
  ../../server-uring/doc/mesh.md
-->

> **Status:** landed | in-progress
> **Source:** `src/<path>.h` [ + `src/<path>.cpp` ]
> **Namespace:** `<ns>`
> **Depends:** `<unit>`, `<unit>`   (units that must exist first; names as in INDEX.md)

## Purpose

1–3 sentences: what this unit is and the one reason it exists. No history.

## API

The exact public declarations, copied from the header and trimmed of
bodies. Keep every signature, constant, type alias, and `static_assert`.
Keep the header's own comments where they state a contract.

```cpp
namespace <ns> {
// ... exact declarations ...
}
```

## Invariants

What must ALWAYS hold — ownership, threading contract, all-or-nothing
guarantees. Say whether a violation traps (`LNX_CHECK`) or is undefined.

## Errors & edge cases

Each failure mode and its DEFINED behaviour: return value, trap, no-op.
Cover empty / full / oversize / boundary explicitly. A table reads well.

## Notes

Non-obvious implementation constraints only: alignment, non-movable storage,
sizing math, cache-line placement, sanitizer gotchas. Skip the obvious.

## Test plan

One line per EXISTING test case that exercises this unit, mapped to
`tests/<path>_test.cpp`. If none: "No dedicated test." Never list a test
that has not been written.

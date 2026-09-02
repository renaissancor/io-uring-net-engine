# expected — project aliases over `tl::expected`

> **Status:** landed
> **Source:** `src/error/expected.h`
> **Namespace:** — (global aliases)
> **Depends:** `tl::expected` (external, resolved by `cmake/deps.cmake`)

## Purpose

The project's value-or-error return type, spelled `expected<T, E>` and
`unexpected<E>` without the `tl::` prefix, so call sites are unchanged if the
implementation is ever swapped for `std::expected`.

## API

```cpp
#include <tl/expected.hpp>

template <class T, class E>
using expected = tl::expected<T, E>;

template <class E>
using unexpected = tl::unexpected<E>;
```

## Invariants

- These are the only spellings of the type permitted in project code; see
  [`04-coding-style.md`](../04-coding-style.md) § error model. `tl::` is not
  written outside this header.
- Recoverable failure is returned through `expected`; unrecoverable failure
  traps through `LNX_CHECK` ([`check.md`](../check.md)). Exceptions are not
  used.

## Errors & edge cases

None of its own. Semantics are `tl::expected`'s: `has_value()`, `value()`,
`error()`, `operator bool`, monadic `and_then` / `map` / `or_else`.

## Notes

`tl::expected` is a header-only dependency pinned by the build; the floor
preset compiles it under the project's minimum compiler. Nothing in
`src/` or `../server-uring/src/` returns an `expected` today; the alias is
in place for the data-path units that will.

## Test plan

No dedicated test.

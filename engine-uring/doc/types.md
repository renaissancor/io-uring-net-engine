# types — fixed-width integer aliases

> **Status:** landed
> **Source:** `src/types.h`
> **Namespace:** — (global aliases)
> **Depends:** none

## Purpose

One spelling for every integer width the project uses, so signatures read
`u16` / `usize` rather than `std::uint16_t` / `std::size_t`. Nothing else
lives here.

## API

```cpp
using byte  = unsigned char;

using i08 = std::int8_t;    using u08 = std::uint8_t;
using i16 = std::int16_t;   using u16 = std::uint16_t;
using i32 = std::int32_t;   using u32 = std::uint32_t;
using i64 = std::int64_t;   using u64 = std::uint64_t;

using usize = std::size_t;
using isize = std::ptrdiff_t;

using uptr = std::uintptr_t;
using iptr = std::intptr_t;
```

## Invariants

- Aliases only. No functions, no types with behaviour, no macros.
- `byte` is `unsigned char`, not `std::byte`: it is used for raw buffer
  arithmetic and must support `+`, `<`, and `reinterpret_cast` to and from
  object pointers without a cast per use.

## Errors & edge cases

None; the header has no runtime.

## Notes

The two-digit `i08` / `u08` spelling exists so the eight-bit names line up
with `i16` / `u16` in aligned declarations.

## Test plan

No dedicated test. Every test in `tests/` compiles against these aliases.

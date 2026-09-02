# session_id — flat identity aliases shared by acceptor and worker

> **Status:** landed
> **Source:** `src/session_id.h`
> **Namespace:** `app`
> **Depends:** `types`

## Purpose

The integer identity vocabulary used by the SessionManager authority table
(`session_record`, `session_table`) and by the mesh messages that cross from
acceptor to worker and back. Plain aliases over fixed-width integers plus the
two reserved sentinel values.

## API

```cpp
namespace app {

//   session_id  monotonic, process-unique. 0 is the reserved invalid id.
//   account_id  fake guest identity for v1 (== session_id until DB/auth lands).
//   room_id     interaction-space id; 0 is the reserved "no room" sentinel.
//   worker_id   owner-thread index into the worker pool (0 .. k_worker_count-1).
using session_id = u64;
using account_id = u64;
using room_id    = u32;
using worker_id  = u32;

inline constexpr session_id k_invalid_session = 0;
inline constexpr room_id    k_no_room         = 0;

}  // namespace app
```

## Invariants

- **Zero is never a live id:** `session_table::allocate()` mints ids starting
  above `k_invalid_session`, and `remove()` writes `k_invalid_session` back into
  a freed slot. `find(k_invalid_session)` returns `nullptr`.
- **Zero is never a room:** `k_no_room` is the value `adopt_session_msg::initial_room`
  carries on the immediate-adopt path.
- **Aliases, not wrappers:** the four names are `using` aliases. The compiler
  does not stop a `room_id` from being passed where a `session_id` is expected,
  or a `worker_id` where an `account_id` is expected.
- **Widths are the wire layout:** because these types appear directly in the
  POD structs of `message.h`, their widths (u64/u64/u32/u32) fix the byte
  layout of every mesh frame.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `session_id` value `0` used as a lookup key | `session_table::find` returns `nullptr`; `validate` returns `false` |
| Mixing alias types (e.g. `room_id` into a `session_id` parameter) | compiles silently — implicit integer conversion, no guard |

## Notes

- `worker_id` is unsigned, but `session_record::owner_worker` is `i32` so the
  record can hold `-1` for "unassigned". `session_table::mark_assigning` takes
  `i32`, so any `worker_id` → `i32` conversion happens at its caller.
- `account_id` is currently populated by the caller of `session_table::allocate`
  as an opaque value; nothing in this component derives or validates it.

## Test plan

No dedicated test. Exercised indirectly:

`tests/session_table_test.cpp`:
- allocate mints unique ids and starts accepted — asserts minted `id != k_invalid_session`
- find locates live records, misses absent ids — asserts `find(k_invalid_session) == nullptr`
- generation guard rejects stale references — asserts `validate(k_invalid_session, 0)` is false

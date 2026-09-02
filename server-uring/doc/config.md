# config — runtime configuration record passed to the thread ctls

> **Status:** landed
> **Source:** `src/config.h`
> **Namespace:** `app`
> **Depends:** `types`

## Purpose

The single configuration value the supervisor constructs and hands to every
`worker_ctl` / `acceptor_ctl` by `const config&`. Today it carries one
compile-time constant: the capacity of the acceptor-side `session_table`.

## API

```cpp
namespace app {

struct config {
    // SessionManager authority-table size (compile-time: it backs a fixed
    // inline session_record array, no heap). Fixed CAPACITY, not a ceiling on a
    // runtime value — the table is always exactly this many slots, which is why
    // it is `_capacity` and not `_max`.
    static constexpr usize k_session_capacity = 256;
};

}  // namespace app
```

`config` has no non-static data members: `sizeof(config)` is 1 and
`app::config{}` is a complete, valid value.

## Invariants

- **Compile-time capacity:** `k_session_capacity` sizes the inline
  `session_record _slots[]` array in `session_table` and is the value
  `session_table::capacity()` returns. There is no runtime override.
- **Thread roster is elsewhere:** nothing in `config` decides how many threads
  exist or how much storage they own; that is fixed in `src/roster.h`.
- **Copied by value into ctls:** `worker_ctl` and `acceptor_ctl` each store a
  `config _cfg` member copied from the ctor argument, so the supervisor's
  instance need not outlive them.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `k_session_capacity * mesh_frame_size(sizeof(session_closed_msg))` exceeds `worker_to_acceptor_pipe::capacity()` | compile error — `static_assert` in `src/mesh.h` (one in-flight close per session must fit the close-notify pipe) |
| `session_table` holds `k_session_capacity` live records | `allocate()` returns `nullptr` (see `session_table`) |

## Notes

- The header comment describes `config` as a v1 placeholder whose fields
  (listen address, db connect string, pre-warm overrides) are expected later;
  none of those fields exist — not built.
- Raising `k_session_capacity` is bounded by the `mesh.h` static_assert above:
  the worker→acceptor pipe must be widened in the same change or the build
  fails.

## Test plan

No dedicated test. Exercised indirectly:

`tests/session_table_test.cpp`:
- allocate returns nullptr when full — fills exactly `config::k_session_capacity`
  slots and checks `count()` equals it

`tests/worker_ctl_skeleton_test.cpp`:
- worker_ctl spawns, runs, drains, stops clean — constructs `app::config{}` and
  passes it to `worker_ctl`
- worker_ctl::request_stop is idempotent — same
- request_stop on starting ctl transitions to draining — same

# session_record — acceptor-side authority record for one session

> **Status:** landed
> **Source:** `src/session_record.h`
> **Namespace:** `app`
> **Depends:** `session_id`, `types`

## Purpose

The per-slot record held by the SessionManager (acceptor) `session_table`: a
session's identity, its lifecycle state, and which worker currently owns its
fd. It is the id → state and id → owner mapping only; it holds no fd, no I/O
rings, and no room or entity data.

## API

```cpp
namespace app {

// SessionManager-side lifecycle. Distinct from the thread `state` enum — this
// tracks a session's journey through the mesh, not a thread's runtime.
enum class session_state : u08 {
    accepted,       // fd accepted, id minted, not yet handed to a worker
    assigning,      // adopt_session posted to a worker; awaiting confirmation
    in_world,       // worker owns the fd and the session has entered a room
    disconnecting,  // close in flight
    closed,         // slot free for reuse
};

struct session_record {
    session_id    id;          // process-unique; k_invalid_session when free
    u32           generation;  // bumped per slot reuse; guards stale messages
    account_id    account;     // fake guest id for v1
    i32           owner_worker; // -1 before assignment
    session_state state;
};

}  // namespace app
```

## Invariants

- **Free slot means `id == k_invalid_session`:** `session_table` treats the id
  field, not `state`, as the occupancy flag. The table ctor and `remove` both
  leave a free slot with `id == k_invalid_session`, `owner_worker == -1`, and
  `state == closed`; `remove` does not clear `account`.
- **Generation survives reuse:** `session_table::remove` preserves
  `generation`; `allocate` increments it on the slot it reuses. A
  `(id, generation)` pair therefore names one incarnation of a slot, and any
  mesh message carrying an older generation is rejected by `validate`.
- **`owner_worker` is `-1` until assigned:** set by `session_table` when the
  acceptor hands the fd to a worker; `-1` on allocate and on remove.
- **Fresh record starts `accepted`:** `session_table::allocate` returns a
  record with `state == session_state::accepted`.
- **No I/O ownership:** the record never holds the fd. Once the fd is handed to
  a worker via `adopt_session_msg`, the acceptor keeps only this mapping.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `state` read on a slot whose `id == k_invalid_session` | always `closed` — the `session_table` ctor and `remove` both write it; occupancy is still decided by `id`, not `state` |
| `owner_worker` compared with a `worker_id` (`u32`) | signed/unsigned mismatch; `session_table::mark_assigning` takes `i32`, so the conversion sits at its caller |
| `state` transitions | not enforced by this struct; `session_table` writes whatever its caller asks for |

## Notes

- `session_state` is `u08`, so `session_record` is 32 bytes on x86-64 with
  padding after `owner_worker` and `state`. `session_table` stores
  `config::k_session_capacity` (256) of them inline — 8 KiB, no heap.
- The struct is aggregate-initialisable and has no constructor;
  `session_table` writes every field explicitly on allocate and remove.

## Test plan

No dedicated test. Exercised through the table that owns it,
`tests/session_table_test.cpp`:
- allocate mints unique ids and starts accepted — fresh record has
  `state == accepted` and a non-zero `id`
- find locates live records, misses absent ids — record lookup by `id`
- generation guard rejects stale references — `generation` bump on slot reuse
- state + owner transitions — `state` moves `accepted → assigning → in_world`,
  `owner_worker` set
- remove frees the slot and decrements count — slot returns to free
- allocate returns nullptr when full — all 256 records live

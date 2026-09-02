# session_table — the acceptor's fixed-capacity session authority map

> **Status:** landed
> **Source:** `src/session_table.h`, `src/session_table.cpp`
> **Namespace:** `app`
> **Depends:** `config`, `session_id`, `session_record`, `types`

## Purpose

Maps a `session_id` to its `session_record` (state + owner worker + generation)
on behalf of the acceptor, which is the sole authority for session identity and
ownership. A flat inline array of `config::k_session_capacity` slots, scanned
linearly; no heap, no `std::` container.

## API

```cpp
namespace app {

class session_table {
    session_record _slots[config::k_session_capacity];
    session_id     _next_id = 1;   // monotonic; 0 stays reserved as invalid
    u32            _count   = 0;

public:
    session_table() noexcept;

    session_table(const session_table&)            = delete;
    session_table& operator=(const session_table&) = delete;
    session_table(session_table&&)                 = delete;
    session_table& operator=(session_table&&)      = delete;

    static constexpr usize capacity() noexcept { return config::k_session_capacity; }
    u32 count() const noexcept { return _count; }

    // Mint a new session in state `accepted`. Assigns a process-unique id and a
    // per-slot generation. Returns the record (owned by the table) or nullptr
    // when full. owner_worker starts at -1 (unassigned).
    session_record* allocate(account_id account) noexcept;

    // Linear-scan lookup by id. Returns nullptr if absent (never allocated,
    // or already removed). O(capacity).
    session_record* find(session_id id) noexcept;

    // True iff a live record with this id exists AND its generation matches —
    // the guard that lets the acceptor discard a message referring to a
    // recycled slot. A closed/absent id fails.
    bool validate(session_id id, u32 generation) noexcept;

    // Record the owning worker and flip accepted -> assigning. Returns false if
    // the id is absent.
    bool mark_assigning(session_id id, i32 worker) noexcept;

    // Flip to in_world (worker confirmed adoption + room entry). False if absent.
    bool mark_in_world(session_id id) noexcept;

    // Free the slot for reuse (state -> closed, id -> invalid). Returns false if
    // the id was not present. Bumps nothing here — the generation advances on
    // the NEXT allocate() into this slot.
    bool remove(session_id id) noexcept;
};

}  // namespace app
```

## Invariants

- **Single owner:** the table is constructed, read, and mutated by the acceptor
  thread alone. There is no lock and no atomic; other threads learn session
  state only through mesh messages.
- **Free slot = invalid id:** a slot is free iff `id == k_invalid_session`. The
  constructor puts every slot in that state with `generation = 0`,
  `owner_worker = -1`, `state = closed`.
- **Ids are process-unique and monotonic:** `allocate` hands out `_next_id++`
  starting at 1; an id is never reissued, even after its slot is freed.
- **Generation survives removal:** `remove` clears the id but keeps the slot's
  generation; the next `allocate` into that slot increments it. A message
  carrying the old `(id, generation)` therefore fails `validate` twice over:
  the old id is gone, and the new id carries a newer generation.
- **`count()` tracks live slots exactly:** incremented on successful
  `allocate`, decremented on successful `remove`.
- **Records are table-owned:** pointers returned by `allocate`/`find` stay
  valid for the table's lifetime and refer to the same slot even after reuse.
- **Non-copyable, non-movable:** the array is inline and pointers into it are
  handed out, so the object never relocates.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `allocate` with every slot in use | returns `nullptr`; `count()` unchanged |
| `find` / `validate` / `mark_*` / `remove` with `k_invalid_session` | absent: `nullptr` / `false` |
| `find` with an id never minted or already removed | `nullptr` |
| `validate` with right id, wrong generation | `false` |
| `mark_assigning` / `mark_in_world` on absent id | `false`, no state change |
| `mark_assigning` on a record not in `accepted` | no state check; overwrites owner and sets `assigning` |
| `remove` twice for the same id | second call returns `false`, `count()` unchanged |
| `_next_id` wrap | not guarded; `session_id` is exhausted before this matters in practice |

## Notes

- Every lookup is an O(`k_session_capacity`) scan over 256 slots; the header
  labels this a correctness-first shape.
- `find` and the mutators share one private `slot_of` scan; `validate` reads
  through a `const` pointer to the same slot.
- The `disconnecting` value of `session_state` is defined in `session_record`
  but no `session_table` method sets it.

## Test plan

`tests/session_table_test.cpp`:
- allocate mints unique ids, starts `accepted` with `owner_worker == -1`, bumps count
- find locates live records; misses `k_invalid_session` and unknown ids
- generation guard: wrong generation refused; remove + reallocate reuses the slot with generation+1 and rejects the stale generation
- state + owner transitions: `mark_assigning` sets owner and `assigning`, `mark_in_world` sets `in_world`, absent id returns false
- remove frees the slot, decrements count, `find` misses it, double-remove returns false
- allocate returns `nullptr` when full after exactly `k_session_capacity` successes
